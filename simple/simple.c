
#include <string.h>
#include "simple.h"
#include "../tokens/lit.h"

#define SSTACK__STMT     0  // within regular statement (not a hoist)
#define SSTACK__GROUP    1  // group execution context "()" or "[]"
#define SSTACK__BLOCK    2  // block execution context
#define SSTACK__DICT     3  // within regular dict "{}"
#define SSTACK__FUNC     4  // expects upcoming "name () {}"
#define SSTACK__CLASS    5  // expects "extends X"? "{}"
#define SSTACK__DO_WHILE 6  // state machine for "do ... while"
#define SSTACK__MODULE   7  // state machine for import/export defs

// SSTACK__GROUP
#define SPECIAL__FOR        1

// SSTACK__BLOCK
#define SPECIAL__INIT       1

// SSTACK__CLASS
// for e.g. "class extends {} {}", left is value (just one single token, or e.g. "(foo)")
#define SPECIAL__FREE_VALUE 1


#ifdef __EMSCRIPTEN__
#define debugf (void)sizeof
#else
#include <stdio.h>
#define debugf(...) fprintf(stderr, __VA_ARGS__)
#endif


typedef struct {
  token t1;
  token t2;
  uint8_t stype : 4;
  uint8_t context : 4;
  uint8_t special : 1;
} sstack;


typedef struct {
  tokendef *td;
  token *next;  // convenience
  token tok;
  int tok_has_value;  // has_value current tok was read with
  int is_module;

  prsr_callback cb;
  void *arg;

  sstack *curr;
  sstack stack[256];
} simpledef;


// stores a virtual token in the stream, and yields it before the current token
static void yield_virt(simpledef *sd, int type) {
  token *t = &(sd->curr->t1);
  sd->curr->t2 = *t;  // move t1 => t2
  bzero(t, sizeof(token));
  t->line_no = sd->curr->t2.line_no;  // ... on prev line
  t->type = type;

  sd->cb(sd->arg, t);
}


// places the next useful token in sd->tok, yielding previous current
static int skip_walk(simpledef *sd, int has_value) {
  if (sd->tok.p) {
    sd->cb(sd->arg, &(sd->tok));
  }
  for (;;) {
    // prsr_next_token can reveal comments, loop until over them
    int out = prsr_next_token(sd->td, &(sd->tok), has_value);
    if (out || sd->tok.type != TOKEN_COMMENT) {
      sd->tok_has_value = has_value;
      return out;
    }
    sd->cb(sd->arg, &(sd->tok));
  }
}


// records/yields the current token, places the next useful token
static int record_walk(simpledef *sd, int has_value) {
  sd->curr->t2 = sd->curr->t1;
  sd->curr->t1 = sd->tok;
  return skip_walk(sd, has_value);
}


static int is_optional_keyword(uint32_t hash, uint8_t context) {
  if (context & CONTEXT__ASYNC && hash == LIT_AWAIT) {
    return 1;
  } else if (context & (CONTEXT__GENERATOR | CONTEXT__STRICT) && hash == LIT_YIELD) {
    // yield is invalid outside a generator in strict mode, but it's a keyword
    return 1;
  }
  return 0;
}


static int is_always_keyword(uint32_t hash, uint8_t context) {
  return (hash & _MASK_KEYWORD) ||
      ((context & CONTEXT__STRICT) && (hash & _MASK_STRICT_KEYWORD));
}


static int is_label(uint32_t hash, uint8_t context) {
  return !is_always_keyword(hash, context) && !is_optional_keyword(hash, context);
}


static int is_valid_name(uint32_t hash, uint8_t context) {
  uint32_t mask = _MASK_KEYWORD | _MASK_MASQUERADE;
  if (context & CONTEXT__STRICT) {
    mask |= _MASK_STRICT_KEYWORD;
  }

  if ((context & CONTEXT__ASYNC) && hash == LIT_AWAIT) {
    // await is a keyword inside async function
    return 0;
  }

  if ((context & CONTEXT__GENERATOR) && hash == LIT_YIELD) {
    // yield is a keyword inside generator function
    return 0;
  }

  return !(hash & mask);
}


static int is_unary(uint32_t hash, uint8_t context) {
  // check if we're also a keyword, to avoid matching 'await' and 'yield' by default
  uint32_t mask = _MASK_UNARY_OP | _MASK_KEYWORD;
  return ((hash & mask) == mask) || is_optional_keyword(hash, context);
}


// whether the current position has a function decl/stmt
static int peek_function(simpledef *sd) {
  if (sd->tok.type != TOKEN_LIT) {
    return 0;
  } else if (sd->tok.hash == LIT_ASYNC) { 
    return sd->next->type == TOKEN_LIT && sd->next->hash == LIT_FUNCTION;
  }
  return sd->tok.hash == LIT_FUNCTION;
}


// matches any current function decl/stmt
static int match_function(simpledef *sd) {
  if (!peek_function(sd)) {
    return -1;
  }

  uint8_t context = (sd->curr->context & CONTEXT__STRICT);
  if (sd->tok.hash == LIT_ASYNC) {
    context |= CONTEXT__ASYNC;
    sd->tok.type = TOKEN_KEYWORD;
    skip_walk(sd, 0);  // consume "async"
  }
  sd->tok.type = TOKEN_KEYWORD;
  record_walk(sd, 0);  // consume "function"

  // optionally consume generator star
  if (sd->tok.type == TOKEN_OP && sd->tok.hash == MISC_STAR) {
    skip_walk(sd, 0);
    context |= CONTEXT__GENERATOR;
  }

  // nb. does NOT consume name
  return context;
}


static int match_class(simpledef *sd) {
  if (sd->tok.hash != LIT_CLASS) {
    return -1;
  }
  sd->tok.type = TOKEN_KEYWORD;
  record_walk(sd, 0);

  int special = 0;

  // optionally consume class name
  if (sd->tok.type == TOKEN_LIT) {
    if (sd->tok.hash == LIT_EXTENDS) {
      special = SPECIAL__FREE_VALUE;
    } else {
      int hoist = (sd->curr - 1)->stype == SSTACK__BLOCK;
      if (!is_valid_name(sd->tok.hash, sd->curr->context) || sd->tok.hash == LIT_YIELD || sd->tok.hash == LIT_LET) {
        // nb. "yield" or "let" is always an invalid class name, even in non-strict (doesn't apply to function)
        // ... this might actually be a V8 "feature", but it's the same in Firefox
        sd->tok.type = TOKEN_KEYWORD;  // "class if" is invalid
      } else {
        sd->tok.type = TOKEN_SYMBOL;
      }
      skip_walk(sd, 0);  // consume name
    }
  }

  // consume extends
  if (special || (sd->tok.type == TOKEN_LIT && sd->tok.hash == LIT_EXTENDS)) {
    sd->tok.type = TOKEN_KEYWORD;
    skip_walk(sd, 0);  // consume "extends" keyword
    special = SPECIAL__FREE_VALUE;
  }

  return special;
}


// matches var/const/let, optional let based on context
static int match_decl(simpledef *sd) {
  if (!(sd->tok.hash & _MASK_DECL)) {
    return -1;
  }

  // in strict mode, 'let' is always a keyword (well, reserved)
  if (!(sd->curr->context & CONTEXT__STRICT) && sd->tok.hash == LIT_LET) {
    if (sd->next->type == TOKEN_BRACE || sd->next->type == TOKEN_ARRAY || !(sd->next->hash & _MASK_REL_OP)) {
      // OK: const, var or e.g. "let[..]" or "let{..}", destructuring
    } else {
      return -1;  // no following literal (e.g. "let = 1", "instanceof" counts as op)
    }
  }

  sd->tok.type = TOKEN_KEYWORD;
  return record_walk(sd, 0);
}


// matches a "break foo;" or "continue;", emits ASI if required
static int match_label_keyword(simpledef *sd) {
  if (sd->tok.hash != LIT_BREAK && sd->tok.hash != LIT_CONTINUE) {
    return -1;
  }

  int line_no = sd->tok.line_no;

  sd->tok.type = TOKEN_KEYWORD;
  skip_walk(sd, 0);

  if (sd->tok.line_no == line_no && is_label(sd->tok.hash, sd->curr->context)) {
    sd->tok.type = TOKEN_LABEL;
    skip_walk(sd, 0);
  }

  // e.g. "break\n" or "break foo\n"
  if (sd->tok.line_no != line_no) {
    yield_virt(sd, TOKEN_SEMICOLON);  // yield semi because line change
  } else if (sd->tok.type == TOKEN_SEMICOLON) {
    skip_walk(sd, 0);  // consume valid semi
  }

  return 0;
}


static int is_use_strict(token *t) {
  if (t->type != TOKEN_STRING || t->len != 12) {
    return 0;
  }
  return !memcmp(t->p, "'use strict'", 12) || !memcmp(t->p, "\"use strict\"", 12);
}


static sstack *stack_inc(simpledef *sd, uint8_t stype) {
  // TODO: check bounds
  ++sd->curr;
  bzero(sd->curr, sizeof(sstack) * 2);  // also bzero next stack
  sd->curr->stype = stype;
  sd->curr->context = (sd->curr - 1)->context;  // copy context
  return sd->curr;
}


static int yield_valid_asi(simpledef *sd) {
  if (!sd->curr->stype) {
    sstack *up = sd->curr;
    --sd->curr;
    if (up->t1.type) {
      yield_virt(sd, TOKEN_SEMICOLON);
    }
    debugf("added ASI to zero stype, now: %d\n", sd->curr->stype);
    return 1;
  }

  if (sd->curr->stype == SSTACK__BLOCK && sd->curr->t1.type) {
    // if parent is __BLOCK, just pretend a statement happened anyway
    stack_inc(sd, 0);
    --sd->curr;
    yield_virt(sd, TOKEN_SEMICOLON);
    return 1;
  }

  // can't emit here (but JS probably invalid?)
  return 0;
}


static int is_token_valuelike(token *t) {
  if (t->type == TOKEN_LIT) {
    return !(t->hash & _MASK_REL_OP);
  }
  return t->type == TOKEN_SYMBOL || t->type == TOKEN_NUMBER || t->type == TOKEN_STRING || t->type == TOKEN_BRACE;
}


// is the next token valuelike following a keyword? (e.g. "extends []")
static int is_token_valuelike_keyword(token *t) {
  if (is_token_valuelike(t)) {
    return 1;
  }
  return t->type == TOKEN_PAREN || t->type == TOKEN_ARRAY || t->type == TOKEN_BRACE;
}


static int simple_consume(simpledef *sd) {
restart:

  // import state
  if (sd->curr->stype == SSTACK__MODULE) {

    switch (sd->tok.type) {
      case TOKEN_BRACE:
        record_walk(sd, 0);
        stack_inc(sd, SSTACK__MODULE);
        return 0;

      // unexpected, but handle anyway
      case TOKEN_T_BRACE:
      case TOKEN_PAREN:
      case TOKEN_ARRAY:
        record_walk(sd, 0);
        stack_inc(sd, SSTACK__GROUP);
        return 0;

      case TOKEN_LIT:
        break;

      case TOKEN_COMMA:
        return record_walk(sd, 0);

      case TOKEN_CLOSE:
        if ((sd->curr - 1)->stype != SSTACK__MODULE) {
          return ERROR__INTERNAL;  // impossible, we're at top-level
        }
        int line_no = sd->tok.line_no;
        skip_walk(sd, 0);
        --sd->curr;  // close inner

        if ((sd->curr - 1)->stype == SSTACK__MODULE) {
          return 0;  // invalid several descendant case
        }
        --sd->curr;  // close outer

        if (sd->tok.type == TOKEN_LIT && sd->tok.hash == LIT_FROM) {
          // ... inner {} must have trailer "from './path'"
          sd->tok.type = TOKEN_KEYWORD;
          record_walk(sd, 0);
          if (sd->tok.type == TOKEN_STRING) {
            sd->tok.mark = MARK_IMPORT;
          }
        } else if (sd->tok.type != TOKEN_SEMICOLON && sd->tok.line_no != line_no) {
          // ... or just abandon, generating semi if needed (valid in export case)
          yield_virt(sd, TOKEN_SEMICOLON);
        }
        return 0;

      case TOKEN_OP:
        if (sd->tok.hash == MISC_STAR) {
          sd->tok.type = TOKEN_SYMBOL;  // pretend this is symbol
          return record_walk(sd, 0);
        }
        // fall-through

      default:
        if ((sd->curr - 1)->stype != SSTACK__MODULE) {
          debugf("abandoning module for reasons\n");
          --sd->curr;
          goto restart;  // not inside submodule, just give up
        }
        return record_walk(sd, 0);
    }

    // consume and bail out on "from" if it follows a symbol or close brace
    if ((sd->curr - 1)->stype != SSTACK__MODULE &&
        sd->curr->t1.type == TOKEN_SYMBOL &&
        sd->tok.hash == LIT_FROM) {
      --sd->curr;  // close outer
      sd->tok.type = TOKEN_KEYWORD;
      record_walk(sd, 0);

      // restart as regular statement, marking as import name
      if (sd->tok.type == TOKEN_STRING) {
        sd->tok.mark = MARK_IMPORT;
      }
      return 0;
    }

    // consume "as" as a keyword if it follows a symbol
    if (sd->curr->t1.type == TOKEN_SYMBOL && sd->tok.hash == LIT_AS) {
      sd->tok.type = TOKEN_KEYWORD;
      return record_walk(sd, 0);
    }

    // otherwise just mask as symbol or keyword
    if (is_valid_name(sd->tok.hash, sd->curr->context)) {
      sd->tok.type = TOKEN_SYMBOL;
    } else {
      // ... invalid, of course
      sd->tok.type = TOKEN_KEYWORD;
    }
    return record_walk(sd, 0);
  }

  // dict state (left)
  if (sd->curr->stype == SSTACK__DICT) {
    uint8_t context = 0;

    // search for function
    // ... look for 'static' without '(' next
    if (sd->tok.type == TOKEN_LIT &&
        sd->td->next.type != TOKEN_PAREN &&
        sd->tok.hash == LIT_STATIC) {
      sd->tok.type = TOKEN_KEYWORD;
      record_walk(sd, 0);
    }

    // ... look for 'async' without '(' next
    if (sd->tok.type == TOKEN_LIT &&
        sd->td->next.type != TOKEN_PAREN &&
        sd->tok.hash == LIT_ASYNC) {
      sd->tok.type = TOKEN_KEYWORD;
      record_walk(sd, 0);
      context |= CONTEXT__ASYNC;
    }

    // ... look for '*'
    if (sd->tok.type == TOKEN_OP && sd->tok.hash == MISC_STAR) {
      context |= CONTEXT__GENERATOR;
      record_walk(sd, 0);
    }

    // ... look for get/set without '(' next
    if (sd->tok.type == TOKEN_LIT &&
        sd->td->next.type != TOKEN_PAREN &&
        (sd->tok.hash == LIT_GET || sd->tok.hash == LIT_SET)) {
      sd->tok.type = TOKEN_KEYWORD;
      record_walk(sd, 0);
    }

    // terminal state of left side
    switch (sd->tok.type) {
      // ... anything that looks like it could be a function, that way (and let stack fail)
      case TOKEN_LIT:
      case TOKEN_PAREN:
      case TOKEN_BRACE:
      case TOKEN_ARRAY:
        debugf("pretending to be function: %.*s\n", sd->tok.len, sd->tok.p);
        stack_inc(sd, SSTACK__FUNC);
        sd->curr->context = context;
        goto restart;

      case TOKEN_T_BRACE:  // incase someone puts `${foo}` on the left
      case TOKEN_COLON:
        record_walk(sd, 0);
        stack_inc(sd, SSTACK__GROUP);
        debugf("pushing group for colon\n");
        return 0;

      case TOKEN_CLOSE:
        skip_walk(sd, 1);
        --sd->curr;
        goto restart;

      case TOKEN_COMMA:  // valid
      default:           // invalid, but whatever
        return record_walk(sd, 0);
    }
  }

  // function state, allow () or {}
  if (sd->curr->stype == SSTACK__FUNC) {

    if (sd->tok.type == TOKEN_ARRAY) {
      // allow "function ['name']"
      record_walk(sd, 0);
      stack_inc(sd, SSTACK__GROUP);

      // ... but "{async [await 'name']..." doesn't take await from our context
      sd->curr->context = (sd->curr - 2)->context;
      return 0;
    }

    if (sd->tok.type == TOKEN_LIT) {
      int context = (sd->curr - 1)->context;  // "async function await() {}" is valid :(
      int parent = (sd->curr - 1)->stype;

      // we're only maybe a keyword in non-dict modes
      if (parent != SSTACK__DICT && !is_valid_name(sd->tok.hash, context)) {
        sd->tok.type = TOKEN_KEYWORD;
      } else {
        sd->tok.type = TOKEN_SYMBOL;
      }
      return record_walk(sd, 0);
    }

    if (sd->tok.type == TOKEN_PAREN) {
      // allow "function ()"
      record_walk(sd, 0);
      stack_inc(sd, SSTACK__GROUP);
      return 0;
    }

    if (sd->tok.type == TOKEN_BRACE) {
      // terminal state of func, pop and insert normal block w/retained context
      uint8_t context = sd->curr->context;
      --sd->curr;
      record_walk(sd, 0);
      stack_inc(sd, SSTACK__BLOCK);
      sd->curr->context = context;
      sd->curr->special = SPECIAL__INIT;
      return 0;
    }

    // invalid, abandon function def
    --sd->curr;
    goto restart;
  }

  // class state, just insert group (for extends) or bail
  if (sd->curr->stype == SSTACK__CLASS) {
    if (!is_token_valuelike_keyword(&(sd->tok))) {
      // invalid, not a valuelike for either extends or main class def
      --sd->curr;
    } else if (sd->curr->special == SPECIAL__FREE_VALUE) {
      // found extendable value, just parse it below
      sd->curr->special = 0;
    } else if (sd->tok.type != TOKEN_BRACE) {
      // invalid, not a brace for main class def
      --sd->curr;
    } else {
      // terminal state of class definition, pop and insert dict
      --sd->curr;
      record_walk(sd, 0);
      stack_inc(sd, SSTACK__DICT);
      return 0;
    }
  }

  // do...while state
  if (sd->curr->stype == SSTACK__DO_WHILE) {
    if (sd->curr->t1.type) {
      // this is end of valid group, emit ASI if there's not one
      // occurs regardless of newline, e.g. "do ; while (0) foo;" is valid, ASI after close paren
      if (sd->tok.type == TOKEN_SEMICOLON) {
        skip_walk(sd, 0);
      } else {
        yield_virt(sd, TOKEN_SEMICOLON);
      }
      --sd->curr;
      goto restart;
    }

    // start of do...while, just push block
    stack_inc(sd, SSTACK__BLOCK);
  }

  // zero state, determine what to push
  if (sd->curr->stype == SSTACK__BLOCK) {

    // finished our first _anything_ clear initial bit
    if (sd->curr->t1.type && sd->curr->special) {
      sd->curr->special = 0;

      // look for 'use strict' of single statement
      if (sd->curr->t1.type == TOKEN_SEMICOLON) {
        sstack *prev = (sd->curr + 1);  // this will be previous statement
        if (!prev->t2.type && is_use_strict(&(prev->t1))) {
          sd->curr->context |= CONTEXT__STRICT;
          debugf("got use strict in single statement\n");
        }
      }
    }

    // check incase at least one statement has occured in DO_WHILE > BLOCK
    int has_statement = (sd->curr->t1.type == TOKEN_SEMICOLON || sd->curr->t1.type == TOKEN_BRACE);
    if ((sd->curr - 1)->stype == SSTACK__DO_WHILE && has_statement) {
      --sd->curr;  // pop to DO_WHILE

      // look for following "while (", fail if not
      if (sd->next->type == TOKEN_PAREN &&
          sd->tok.type == TOKEN_LIT &&
          sd->tok.hash == LIT_WHILE) {
        sd->tok.type = TOKEN_KEYWORD;
        record_walk(sd, 0);  // consume while
        record_walk(sd, 0);  // consume paren
        stack_inc(sd, SSTACK__GROUP);
        return 0;
      }

      // couldn't find suffix "while(", retry
      // FIXME: error case
      --sd->curr;
      goto restart;
    }

    // match anonymous block
    if (sd->tok.type == TOKEN_BRACE) {
      debugf("got anon block\n");
      record_walk(sd, 0);
      stack_inc(sd, SSTACK__BLOCK);
      return 0;
    }

    // only care about lit here
    if (sd->tok.type != TOKEN_LIT) {
      goto regular_bail;
    }

    // match label
    if (is_label(sd->tok.hash, sd->curr->context) && sd->next->type == TOKEN_COLON) {
      sd->tok.type = TOKEN_LABEL;
      skip_walk(sd, -1);  // consume label
      skip_walk(sd, 0);  // consume colon
      return 0;
    }

    // match hoisted function (don't insert a regular statement first)
    int context = match_function(sd);
    if (context >= 0) {
      stack_inc(sd, SSTACK__FUNC);
      sd->curr->context = context;
      return 0;
    }

    // match hoisted class
    int special = match_class(sd);
    if (special >= 0) {
      stack_inc(sd, SSTACK__CLASS);
      sd->curr->special = special;
      return 0;
    }

    // match label keyword (e.g. "break foo;")
    if (match_label_keyword(sd) >= 0) {
      return 0;
    }

    // match single-only
    if (sd->tok.hash == LIT_DEBUGGER) {
      int line_no = sd->tok.line_no;
      sd->tok.type = TOKEN_KEYWORD;
      record_walk(sd, 0);

      if (sd->tok.line_no != line_no) {
        yield_valid_asi(sd);
      }
      return 0;
    }

    // match restricted statement starters
    if (sd->tok.hash == LIT_RETURN || sd->tok.hash == LIT_THROW) {
      int line_no = sd->tok.line_no;
      sd->tok.type = TOKEN_KEYWORD;
      record_walk(sd, 0);

      if (sd->tok.line_no != line_no) {
        yield_valid_asi(sd);
        return 0;
      }

      goto regular_bail;  // e.g. "return ..." must be a statement
    }

    // module valid cases
    if (sd->curr == sd->stack && sd->is_module) {

      // match "import" which starts a sstack special
      if (sd->tok.hash == LIT_IMPORT) {
        sd->tok.type = TOKEN_KEYWORD;
        record_walk(sd, 0);

        // short-circuit for "import 'foo'"
        if (sd->tok.type == TOKEN_STRING) {
          sd->tok.mark = MARK_IMPORT;
          goto regular_bail;
        }

        stack_inc(sd, SSTACK__MODULE);
        return 0;
      }

      // match "export" which is sort of a no-op, resets to default state
      if (sd->tok.hash == LIT_EXPORT) {
        sd->tok.type = TOKEN_KEYWORD;
        record_walk(sd, 0);

        if ((sd->tok.type == TOKEN_OP && sd->tok.hash == MISC_STAR) ||
            sd->tok.type == TOKEN_BRACE) {
          stack_inc(sd, SSTACK__MODULE);
          return 0;
        }

        if (sd->tok.type == TOKEN_LIT && sd->tok.hash == LIT_DEFAULT) {
          sd->tok.type = TOKEN_KEYWORD;
          record_walk(sd, 0);
        }

        // interestingly: "export default function() {}" is valid and a decl
        // so classic JS rules around decl must have names are ignored
        // ... "export default if (..)" is invalid, so we don't care if you do wrong things after
        return 0;
      }

    }

    // match "var", "let" and "const"
    if (match_decl(sd) >= 0) {
      goto regular_bail;  // var, const, throw etc must create statement
    }

    // match e.g., "if", "catch"
    if (sd->tok.hash & _MASK_CONTROL) {
      uint32_t hash = sd->tok.hash;
      sd->tok.type = TOKEN_KEYWORD;
      record_walk(sd, 0);

      // match "for await"
      if (sd->tok.type == TOKEN_LIT && hash == LIT_FOR && sd->tok.hash == LIT_AWAIT) {
        // even outside strict/async mode, this is valid, but an error
        sd->tok.type = TOKEN_KEYWORD;
        record_walk(sd, 0);
      }

      // match "do"
      if (hash == LIT_DO) {
        stack_inc(sd, SSTACK__DO_WHILE);
        return 0;
      }

      // if we need a paren, consume without putting into statement
      if ((hash & _MASK_CONTROL_PAREN) && sd->tok.type == TOKEN_PAREN) {
        record_walk(sd, 0);
        stack_inc(sd, SSTACK__GROUP);
        sd->curr->special = (hash == LIT_FOR ? SPECIAL__FOR : 0);
      }

      return 0;
    }

regular_bail:
    // ... or start a regular statement
    stack_inc(sd, 0);
  }

  // match statements
  switch (sd->tok.type) {
    case TOKEN_SEMICOLON:
      if (!sd->curr->stype) {
        --sd->curr;
      }
      record_walk(sd, 0);  // semi goes in block
      return 0;

    case TOKEN_COMMA:
      if ((sd->curr - 1)->stype == SSTACK__DICT) {
        --sd->curr;
        goto restart;
      }

      // relevant in "async () => blah, foo", reset from parent
      sd->curr->context = (sd->curr - 1)->context;
      return record_walk(sd, 0);

    case TOKEN_ARROW:
      if (!(sd->curr->t1.type == TOKEN_PAREN || sd->curr->t1.type == TOKEN_SYMBOL)) {
        // not a valid arrow func, treat as op
        return record_walk(sd, 0);
      }

      uint8_t context = (sd->curr->context & CONTEXT__STRICT);
      if (sd->curr->t2.type == TOKEN_KEYWORD && sd->curr->t2.hash == LIT_ASYNC) {
        context |= CONTEXT__ASYNC;
      }

      if (sd->td->next.type == TOKEN_BRACE) {
        // the sensible arrow function case, with a proper body
        // e.g. "() => { statements }"
        record_walk(sd, -1);  // consume =>
        record_walk(sd, 0);  // consume {
        stack_inc(sd, SSTACK__BLOCK);
        sd->curr->special = SPECIAL__INIT;
      } else {
        // just change statement's context (e.g. () => async () => () => ...)
        record_walk(sd, 0);
      }
      sd->curr->context = context;
      return 0;

    case TOKEN_EOF:
      yield_valid_asi(sd);
      // don't walk over EOF, caller deals with it
      return 0;

    case TOKEN_CLOSE:
      // are we on the right side of a dictionary, closing the dict?
      if (sd->curr->stype == SSTACK__GROUP &&
          (sd->curr - 1)->t1.type == TOKEN_COLON) {
        --sd->curr;
        if (sd->curr->stype != SSTACK__DICT) {
          return ERROR__INTERNAL;
        }
        debugf("closing a dict\n");
        goto restart;  // let dict handle this one (as if it was back on left)
      }

      token *yield = NULL;

      if (sd->curr->stype == SSTACK__GROUP) {
        // closing a group
        --sd->curr;

        // ... look if next token is =>, and resolve any pending "async"
        yield = &(sd->curr->t2);
        if (sd->curr->t1.type == TOKEN_PAREN && yield->type == TOKEN_LIT) {
          yield->type = (sd->next->type == TOKEN_ARROW ? TOKEN_KEYWORD : TOKEN_SYMBOL);
          yield->mark = MARK_RESOLVE;
        } else {
          yield = NULL;
        }

      } else if (sd->curr->stype == SSTACK__BLOCK || !sd->curr->stype) {
        // closing a brace, yield ASI (will decrement !stype)
        yield_valid_asi(sd);
        --sd->curr;  // pop out of block though
      } else {
        // should be normal block or group, no other cases handled
        debugf("can't handle type: %d\n", sd->curr->stype);
        return ERROR__INTERNAL;
      }

      // anything but ending up in naked block has value
      int has_value = (sd->curr->stype != SSTACK__BLOCK);
      if (sd->curr->t1.type == TOKEN_TERNARY) {
        // ... but not "? ... :"
        has_value = 0;
      } else if (sd->curr->stype == SSTACK__DICT) {
        // ... but not in the left side of a dict (although it's probably moot)
        has_value = 0;
      }

      if (sd->tok.type != TOKEN_EOF) {
        // noisy for EOF, where we don't care /shrug
        debugf("popped stack (%ld) in op? has_value=%d (stype=%d)\n", sd->curr - sd->stack, has_value, sd->curr->stype);
      }
      skip_walk(sd, has_value);

      if (yield) {
        sd->cb(sd->arg, yield);
      }
      return 0;

    case TOKEN_BRACE:
      if (sd->tok_has_value && !sd->curr->stype) {
        // found an invalid brace, restart as block
        if (sd->tok.line_no != sd->curr->t1.line_no) {
          // ... with optional ASI
          yield_valid_asi(sd);
        } else {
          --sd->curr;  // yield_valid_asi does this otherwise
        }
        goto restart;
      }
      record_walk(sd, 0);
      stack_inc(sd, SSTACK__DICT);
      return 0;

    case TOKEN_TERNARY:
    case TOKEN_ARRAY:
    case TOKEN_PAREN:
    case TOKEN_T_BRACE:
      record_walk(sd, 0);
      stack_inc(sd, SSTACK__GROUP);
      return 0;

    case TOKEN_LIT:
      if (sd->tok.hash & _MASK_REL_OP) {
        sd->tok.type = TOKEN_OP;
        return record_walk(sd, 0);
      }
      // nb. we catch "await", "delete", "new" etc below
      // fall-through

    case TOKEN_STRING:
      if (sd->curr->t1.type == TOKEN_T_BRACE) {
        // if we're a string following ${}, this is part a of a template literal and doesn't have
        // special ASI casing (e.g. '${\n\n}' isn't really causing a newline)
        return record_walk(sd, 1);
      }
      // fall-through

    case TOKEN_REGEXP:
    case TOKEN_NUMBER:
      // basic ASI detection inside statement: value on a new line than before, with value
      if (!sd->curr->stype && sd->tok.line_no != sd->curr->t1.line_no && sd->tok_has_value) {
        sd->tok_has_value = 0;
        yield_valid_asi(sd);
        goto restart;
      }

      if (sd->tok.type == TOKEN_LIT) {
        break;  // special lit handling
      }
      return record_walk(sd, 1);  // otherwise, just a regular value

    case TOKEN_OP: {
      int has_value = 0;
      if (sd->tok.type == TOKEN_OP && sd->tok.hash == MISC_INCDEC) {
        // if we had value, but are on new line, insert an ASI: this is a PostfixExpression that
        // disallows LineTerminator
        if (sd->tok_has_value && sd->tok.line_no != sd->curr->t1.line_no) {
          sd->tok_has_value = 0;
          yield_valid_asi(sd);
          goto restart;
        }

        // ++ or -- don't change value-ness
        has_value = sd->tok_has_value;
      }
      return record_walk(sd, has_value);
    }

    case TOKEN_COLON:
      if (!sd->curr->stype) {
        --sd->curr;  // this catches cases like "case {}:", pretend that was a statement
      } else {
        // always invalid here
      }
      return record_walk(sd, 0);

    default:
      // nb. This is likely because we haven't resolved a TOKEN_SLASH somewhere.
      debugf("unhandled token=%d `%.*s`\n", sd->tok.type, sd->tok.len, sd->tok.p);
      return ERROR__INTERNAL;

  }

  // match non-hoisted function
  int context = match_function(sd);
  if (context >= 0) {
    // non-hoisted function
    stack_inc(sd, SSTACK__FUNC);
    sd->curr->context = context;
    return 0;
  }

  // match non-hoisted class
  int special = match_class(sd);
  if (special >= 0) {
    stack_inc(sd, SSTACK__CLASS);
    sd->curr->special = special;
    return 0;
  }

  // match valid unary ops
  if (is_unary(sd->tok.hash, sd->curr->context)) {
    sd->tok.type = TOKEN_OP;
    record_walk(sd, 0);

    if (!sd->curr->stype && sd->curr->t1.hash == LIT_YIELD && sd->curr->t1.line_no != sd->tok.line_no) {
      // yield is a restricted keyword
      yield_valid_asi(sd);
    }
    return 0;
  }

  // match non-async await: this is valid iff it _looks_ like unary op use (e.g. await value).
  // this is a lookahead for value, rather than what we normally do
  // FIXME: valuelike is a bit dangerous, should rather be "not oplike" + "no closers" (and 'await' only operates on right)
  if (sd->tok.hash == LIT_AWAIT && is_token_valuelike(sd->next)) {
    // ... to be clear, this is an error, but it IS parsed as a keyword
    sd->tok.type = TOKEN_KEYWORD;
    return record_walk(sd, 0);
  }

  // match curious cases inside "for (" (nb. we eat "for async" => "for", for convenience)
  sstack *up = (sd->curr - 1);
  if (sd->curr->stype == SSTACK__GROUP && sd->curr->special == SPECIAL__FOR) {

    // start of "for (", look for decl (var/let/etc)
    if (sd->curr->t1.type == TOKEN_EOF) {
      if (match_decl(sd) >= 0) {
        goto restart;
      }
    }

    // find "of" between two value-like things
    if (sd->tok.type == TOKEN_LIT &&
        sd->tok.hash == LIT_OF &&
        is_token_valuelike(&(sd->curr->t1)) &&
        is_token_valuelike(sd->next)) {
      sd->tok.type = TOKEN_OP;
      return record_walk(sd, 0);
    }
  }

  // aggressive keyword match inside statement
  if (is_always_keyword(sd->tok.hash, sd->curr->context)) {
    if (!sd->curr->stype && sd->curr->t1.type && sd->tok.line_no != sd->curr->t1.line_no) {
      // if a keyword on a new line would make an invalid statement, restart with it
      yield_valid_asi(sd);
      goto restart;
    }
    // ... otherwise, it's an invalid keyword
    // ... exception: `for (var x;;)` is valid
    sd->tok.type = TOKEN_KEYWORD;
    return record_walk(sd, 0);
  }

  // look for async arrow function
  if (sd->tok.hash == LIT_ASYNC) {
    if (sd->curr->t1.hash == MISC_DOT) {
      sd->tok.type = TOKEN_SYMBOL;   // ".async" is always a funtion call
    } else if (sd->next->type == TOKEN_LIT) {
      sd->tok.type = TOKEN_KEYWORD;  // "async foo" is always a keyword
    } else if (sd->next->type == TOKEN_PAREN) {
      // otherwise, actually ambiguous: leave as TOKEN_LIT
      return record_walk(sd, 0);
    }
  }

  // if nothing else known, treat as symbol
  if (sd->tok.type == TOKEN_LIT) {
    sd->tok.type = TOKEN_SYMBOL;
  }
  return record_walk(sd, 1);
}


int prsr_simple(tokendef *td, int is_module, prsr_callback cb, void *arg) {
  simpledef sd;
  bzero(&sd, sizeof(simpledef));
  sd.curr = sd.stack;
  sd.is_module = is_module;
  if (is_module) {
    sd.curr->context = CONTEXT__STRICT;
  }
  sd.td = td;
  sd.next = &(td->next);
  sd.cb = cb;
  sd.arg = arg;

  sd.curr->stype = SSTACK__BLOCK;
  sd.curr->special = SPECIAL__INIT;
  record_walk(&sd, 0);

  int ret = 0;
  for (;;) {
    int eof = !sd.tok.type;
    char *prev = sd.tok.p;
    ret = simple_consume(&sd);

    if (eof) {
      skip_walk(&sd, 0);
    } else if (ret) {
      // regular failure case
    } else if (prev == sd.tok.p) {
      debugf("simple_consume didn't consume: %d %.*s\n", sd.tok.type, sd.tok.len, sd.tok.p);
      ret = ERROR__TODO;
    } else {
      continue;
    }

    break;
  }

  if (ret) {
    return ret;
  } else if (sd.tok.type != TOKEN_EOF) {
    debugf("no EOF but valid response\n");
    return ERROR__STACK;
  } else if (sd.curr != sd.stack) {

    debugf("stack is %ld too high\n", sd.curr - sd.stack);
    while (sd.curr != sd.stack) {
      debugf("%ld: %d (t1=%d/%d line=%d, t2=%d/%d line=%d)\n", sd.curr - sd.stack, sd.curr->stype, sd.curr->t1.type, sd.curr->t1.hash, sd.curr->t1.line_no, sd.curr->t2.type, sd.curr->t2.hash, sd.curr->t2.line_no);
      --sd.curr;
    }

    return ERROR__STACK;
  }

  return 0;
}