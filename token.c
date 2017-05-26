#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include "token.h"
#include "utils.h"

#define STACK__BRACE      0  // is {}s
#define STACK__ARRAY      1  // is []s
#define STACK__PAREN      2  // is ()s
#define STACK__TYPEMASK   3  // mask for types
#define STACK__STATEMENT  4  // the next {} under us is a statement (e.g., var x = class{};)

#define FLAG__SLASH_IS_OP 8
#define FLAG__AFTER_OP    16
#define FLAG__EXPECT_ID   32

#define _STACK_SIZE 224

typedef struct {
  char *buf;
  int curr;
  int len;
  int line_no;

  // parsing state
  uint8_t flags;
  uint8_t depth;
  uint8_t stack[_STACK_SIZE];
} tokendef;

typedef struct {
  int len;
  int type;
} eat_out;

char peek_char(tokendef *d, int len) {
  int out = d->curr + len;
  if (out < d->len) {
    return d->buf[out];
  }
  return 0;
}

inline int modify_stack(tokendef *d, int inc, int type) {
  if (inc) {
    if (d->depth == _STACK_SIZE - 1) {
      return -1;
    }
    ++d->depth;
    d->stack[d->depth] = type;
    return 0;
  }

  uint8_t prev = d->stack[d->depth--];
  if (d->depth < 0) {
    return -2;
  } else if ((prev & STACK__TYPEMASK) != type) {
    return prev & STACK__TYPEMASK;
  }
  return 0;
}

eat_out next_token(tokendef *d) {
  // consume whitespace (look for newline, zero char)
  char c;
  for (;; ++d->curr) {
    c = peek_char(d, 0);
    // newlines are magic in JS
    if (c == '\n') {
      ++d->line_no;
      return (eat_out) {1, TOKEN_NEWLINE};
    } else if (!c) {
      return (eat_out) {0, TOKEN_EOF};  // end of file
    } else if (!isspace(c)) {
      break;
    }
  }

  // comments (C99 and long)
  char next = peek_char(d, 1);
  do {
    char *find;
    if (c != '/') {
      break;
    } else if (next == '/') {
      find = "\n";
    } else if (next == '*') {
      find = "*/";
    } else {
      break;
    }

    const char *search = (const char *) d->buf + d->curr + 2;
    char *at = strstr(search, find);
    if (at == NULL) {
      return (eat_out) {d->len - d->curr, TOKEN_COMMENT};  // consumed whole string, not found
    }
    int len = at - search + 2;  // add preamble

    if (next == '/') {
      return (eat_out) {len, TOKEN_COMMENT};  // single line, done
    }

    // count \n's
    char *newline = (char *) search;
    for (;;) {
      newline = strchr(newline, '\n');
      if (!newline || newline >= at) {
        break;
      }
      ++d->line_no;
      ++newline;
    }
    return (eat_out) {len + 2, TOKEN_COMMENT};  // eat "*/"
  } while (0);

  // save flags, reset to zero default
  int flags = d->flags;
  d->flags = 0;

  // various simple punctuation
  switch (c) {
#define _punct(_letter, _type) case _letter: return (eat_out) {1, _type};
    _punct(';', TOKEN_SEMICOLON);
    _punct(',', TOKEN_ELISON);
    _punct(':', TOKEN_COLON);
    _punct('?', TOKEN_TERNARY);
#undef _punct

#define _op(_letter, _inc, _stack, _type) case _letter: \
    if (modify_stack(d, _inc, _stack)) { \
      return (eat_out) {-1, -1}; \
    } \
    return (eat_out) {1, _type}

    _op('[', 1, STACK__ARRAY, TOKEN_ARRAY);
    _op(']', 0, STACK__ARRAY, TOKEN_ARRAY);
    _op('(', 1, STACK__PAREN, TOKEN_PAREN);
    _op(')', 0, STACK__PAREN, TOKEN_PAREN);
    _op('{', 1, STACK__BRACE, TOKEN_BRACE);
#undef _op

    case '}':
      if (modify_stack(d, 0, STACK__BRACE)) {
        return (eat_out) {-1, -1};
      }

      // if that was the mandatory statement after a class/function, slash will be an op
      if (d->stack[d->depth] & STACK__STATEMENT) {
        d->stack[d->depth] &= STACK__TYPEMASK;
        d->flags |= FLAG__SLASH_IS_OP;
      }

      return (eat_out) {1, TOKEN_BRACE};
  }

  // ops: i.e., anything made up of =<& etc
  do {
    if (c == '/' && !(flags & FLAG__SLASH_IS_OP)) {
      break;
    }
    const char start = c;
    int len = 0;
    int allowed;  // how many ops of the same type we can safely consume

    if (strchr("=&|^~!%/+-", c)) {
      allowed = 1;
    } else if (c == '*' || c == '<') {
      allowed = 2;  // exponention operator **, or shift
    } else if (c == '>') {
      allowed = 3;  // right shift, or zero-fill right shift
    } else if (c == 'i' && next == 'n') {
      // this _could_ be 'in' or 'instanceof'
      if (isspace(peek_char(d, 2))) {
        len = 2;
      } else if (strstr(d->buf + d->curr + 2, "stanceof") && isspace(d->buf[d->curr + 10])) {
        len = 10;
      } else {
        break;
      }
    } else {
      break;
    }

    d->flags |= FLAG__AFTER_OP;

    if (len > 0) {
      return (eat_out) {len, TOKEN_OP};
    }
    while (len < allowed) {
      c = peek_char(d, ++len);
      if (c != start) {
        break;
      }
    }

    if (start == '=' && c == '>') {
      // arrow function
      return (eat_out) {2, TOKEN_ARROW};
    } else if (c == start && strchr("+-|&", start)) {
      // FIXME: catch ++ or -- ?
      ++len;  // eat --, ++, || or &&: but no more
    } else if (c == '=') {
      // consume a suffix '=' (or whole ===, !==)
      c = peek_char(d, ++len);
      if (c == '=' && (start == '=' || start == '!')) {
        ++len;
      }
    }
    return (eat_out) {len, TOKEN_OP};
  } while (0);

  // number: "0", ".01", "0x100"
  if (isnum(c) || (c == '.' && isnum(next))) {
    int len = 1;
    c = next;
    for (;;) {
      if (!(isalnum(c) || c == '.')) {  // letters, dots, etc- misuse is invalid, so eat anyway
        break;
      }
      c = peek_char(d, ++len);
    }
    d->flags |= FLAG__SLASH_IS_OP;
    return (eat_out) {len, TOKEN_NUMBER};
  }

  // dot notation
  if (c == '.') {
    if (next == '.' && peek_char(d, 2) == '.') {
      return (eat_out) {3, TOKEN_SPREAD};  // '...' operator
    }
    d->flags |= FLAG__EXPECT_ID;
    return (eat_out) {1, TOKEN_DOT};  // it's valid to say e.g., "foo . bar", so separate token
  }

  // strings
  if (c == '\'' || c == '"' || c == '`') {
    char start = c;
    int len = 0;
    while ((c = peek_char(d, ++len))) {
      // TODO: strchr for final, and check
      if (c == start) {
        ++len;
        break;
      } else if (c == '\\') {
        c = peek_char(d, ++len);
      }
      if (c == '\n') {
        ++d->line_no;  // look for \n
      }
    }
    d->flags |= FLAG__SLASH_IS_OP;
    return (eat_out) {len, TOKEN_STRING};
  }

  // regexp
  if (c == '/') {
    int is_charexpr = 0;
    int len = 1;

    c = next;
    do {
      if (c == '[') {
        is_charexpr = 1;
      } else if (c == ']') {
        is_charexpr = 0;
      } else if (c == '\\') {
        c = peek_char(d, ++len);
      } else if (!is_charexpr && c == '/') {
        c = peek_char(d, ++len);
        break;
      }
      if (c == '\n') {
        ++d->line_no;  // nb. invalid in spec, but we allow here
      }
      c = peek_char(d, ++len);
    } while (c);

    // eat trailing flags
    while (isalnum(c)) {
      c = peek_char(d, ++len);
    }

    d->flags |= FLAG__SLASH_IS_OP;
    return (eat_out) {len, TOKEN_REGEXP};
  }

  // keywords or vars
  do {
    int len = 0;
    do {
      if (c == '\\') {
        ++len;  // don't care, eat whatever aferwards
        c = peek_char(d, ++len);
        if (c != '{') {
          continue;
        }
        while (c && c != '}') {
          c = peek_char(d, ++len);
        }
        ++len;
        continue;
      }

      // nb. `c < 0` == `((unsigned char) c) > 127`
      int valid = (len ? isalnum(c) : isalpha(c)) || c == '$' || c == '_' || c < 0;
      if (!valid) {
        break;
      }
      c = peek_char(d, ++len);
    } while (c);

    // if we're not expecting a symbol, then look for keywords
    char *s = d->buf + d->curr;
    if (!(flags & FLAG__EXPECT_ID) && is_keyword(s, len)) {
      int candidate = (d->stack[d->depth] & STACK__TYPEMASK) || (flags & FLAG__AFTER_OP);
      if (candidate && is_hoist_keyword(s, len)) {
        // got hoist keyword (function, class) and inside ([ or after op: next {} is a statement
        d->stack[d->depth] |= STACK__STATEMENT;
      }
      return (eat_out) {len, TOKEN_KEYWORD};
    }

    // otherwise this is a symbol
    d->flags |= FLAG__SLASH_IS_OP;
    return (eat_out) {len, TOKEN_SYMBOL};
  } while (0);

  // found nothing :(
  return (eat_out) {0, -1};
}

int prsr_token(char *buf, int (*fp)(token *)) {
  tokendef d;
  memset(&d, 0, sizeof(d));
  d.buf = buf;
  d.len = strlen(buf);
  d.line_no = 1;

  for (;;) {
    token out;
    out.p = NULL;
    out.whitespace_after = 0;
    out.line_no = d.line_no;

    eat_out eo = next_token(&d);  // after d->line_no, as this might increase it
    out.len = eo.len;
    out.type = eo.type;

    if (out.type <= 0 || out.len < 0) {
      if (d.curr < d.len) {
        return d.curr;
      }
      break;
    }

    out.p = d.buf + d.curr;
    out.whitespace_after = isspace(d.buf[d.curr + out.len]);  // might hit '\0', is fine
    d.curr += out.len;

    fp(&out);
  }

  return 0;
}
