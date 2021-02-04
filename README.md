[![Tests](https://github.com/samthor/gumnut/workflows/Tests/badge.svg)](https://github.com/samthor/gumnut/actions)

A fast, permissive JavaScript tokenizer and parser in C.
See a [demo syntax highlighter](https://samthor.github.io/gumnut/src/harness/).

Supports ESM code only (i.e., `type="module"`, which is implicitly strict).
Supports all language features in the [draft specification](https://github.com/tc39/proposals/blob/master/finished-proposals.md) (as of January 2021).

This is compiled via Web Assembly to run on the web or inside Node without native bindings.
It's not reentrant, so you can't parse another file from within its callbacks.
It does not generate an AST (although does emit enough data to do so in JS), does not modify the input, and does not use `malloc` or `free`.

## Usage

Import and install via your favourite package manager.
This requires Node [v13.10.0](https://twitter.com/guybedford/status/1235306690901422080?lang=en) or higher.

This works by invoking callbacks on every token as well as open/close announcements for a 'stack', which roughly maps to something you might make an AST node out of.

```js
import {buildHarness} from 'gumnut';

const harness = await buildHarness();  // WebAssembly instantiation is async

// returns Uint8Array to place code
const memory = harness.prepare(code.length);
memory.set(fs.readFileSync('source.js'));

harness.handle({
  callback() { /* check harness.token to see token */ },
  open(stackType) { /* open stack type, return false to skip contents */ },
  close(stackType) { /* close stack type */ },
});

harness.run();
```

This is fairly low-level.
Check out a [module rewriting demo](https://github.com/samthor/gumnut/tree/main/src/tool/imports).

## Coverage

This correctly parses all 'pass-explicit' tests from [test262-parser-tests](https://github.com/tc39/test262-parser-tests), _except_ those which rely on non-strict mode behavior (e.g., use variable names like `static` and `let`).

## Note

JavaScript has a single open-ended 'after-the-fact' ambiguity, as `async` is not always a keyword—even in strict mode.
Here's an example:

```js
// this is a function call of a method named "async"
async(/* anything can go here */) {}

// this is an async arrow function
async(/* anything can go here */) => {}

// this calls the async method on foo, and is _not_ ambiguous
foo.async() {}
```

This parser has to walk over code like this at most twice to resolve whether `async` is a keyword.
See [arrow functions break JavaScript parsers](https://dev.to/samthor/arrow-functions-break-javascript-parsers-1ldp) for more details.

It also needs to walk over non-async functions at most twice—like `(a, b) =>`—to correctly label the argument as either _creating_ new variables in scope, or just using them (like a function call or simple parens).

## History

Since engineers like to rewrite everything all the time, see [the 2020 branch](https://github.com/samthor/gumnut/tree/legacy-2020) of this code.

