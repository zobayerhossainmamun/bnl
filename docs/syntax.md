# Language reference

## Source encoding

UTF-8. Every keyword has both an English and a Bangla form; either is accepted at
any callsite, including inside a single file.

## Comments

```bnl
// line comment — no block comments
```

## Keywords

Reserved in both forms. `for` is reserved but the parser does not yet accept it.

| English | Bangla | English | Bangla |
|---|---|---|---|
| `if` | `যদি` | `class` | `শ্রেণী` |
| `else` | `নাহলে` | `extends` | `প্রসারিত` |
| `while` | `যতক্ষণ` | `super` | `উপরের` |
| `function` | `ফাংশন` | `import` | `আমদানি` |
| `return` | `ফেরত` | `as` | `যেমন` |
| `var` | `চলক` | `and` | `এবং` |
| `true` | `সত্য` | `or` | `অথবা` |
| `false` | `মিথ্যা` | `not` | `না` |
| `null` | `নাই` | | |

`print`, `str`, `type`, `to_number`, `chr`, `try_call` are **global functions**, not
keywords. Their Bangla aliases are `দেখাও`, `লেখ`, `ধরণ` (others are English-only
for now).

## Identifiers

ASCII letters, digits, underscores, and any non-ASCII codepoint (so Bangla letters
are valid in identifier positions). Cannot start with a digit.

```bnl
var greeting = "hi";
চলক নাম = "alice";       // fully Bangla identifier
```

## Literals

| Form | Example |
|---|---|
| Number | `42`, `3.14`, `-0.5` (all stored as IEEE-754 double) |
| String | `"hello"`, `"\n"`, `"\t"`, `"\""`, `"\\"` |
| Boolean | `true` / `সত্য`, `false` / `মিথ্যা` |
| Null | `null` / `নাই` |
| List | `[1, 2, "three"]` |
| Map | `{name: "alice", age: 30}` or `{"name": "alice"}` |

Map keys may be bare identifiers (sugar for the same string) or string literals.
String escapes: `\n`, `\t`, `\r`, `\"`, `\\`. No string interpolation. No multi-line
strings.

## Operators

By precedence, lowest to highest:

| Level | Operators | Notes |
|---|---|---|
| Logical or | `or` / `অথবা`, `\|\|` | Short-circuit |
| Logical and | `and` / `এবং`, `&&` | Short-circuit |
| Equality | `==`, `!=` | |
| Comparison | `<`, `<=`, `>`, `>=` | Numbers and strings |
| Additive | `+`, `-` | `+` also concatenates strings |
| Multiplicative | `*`, `/`, `%` | |
| Unary | `-`, `not` / `না`, `!` | |
| Call / index / member | `f(x)`, `xs[i]`, `obj.field` | Left-associative |

`==` is value-equality on primitives; identity on lists, maps, modules, instances.

## Types

`type(value)` returns one of the strings:

| `type()` returns | Origin |
|---|---|
| `"null"` | `null` |
| `"bool"` | `true` / `false` |
| `"number"` | numeric literals, arithmetic results |
| `"string"` | string literals, concatenation |
| `"function"` | named functions, function expressions, methods |
| `"class"` | class declarations |
| `"instance"` | results of calling a class |
| `"module"` | imports |
| `"list"` | `[...]` |
| `"map"` | `{...}` |

## Variables and scope

```bnl
var x = 10;       // declaration
x = x + 1;        // assignment to existing binding
{
    var x = 99;   // shadows outer x within this block
}
print(x);         // 11
```

Lexical scope. Functions close over their defining environment.

## Statements

### Block

```bnl
{ statement; statement; }
```

### If / else

```bnl
if (cond) { ... } else { ... }
যদি (cond) { ... } নাহলে { ... }
```

### While

```bnl
while (cond) { ... }
যতক্ষণ (cond) { ... }
```

There is no `for` loop yet. Iterate by index:

```bnl
var i = 0;
while (i < xs.length) {
    print(xs[i]);
    i = i + 1;
}
```

### Function declaration

```bnl
function add(a, b) { return a + b; }
ফাংশন যোগ(ক, খ) { ফেরত ক + খ; }
```

### Function expression (anonymous)

```bnl
var double = function (n) { return n * 2; };
```

Used heavily for callbacks (timers, async io).

### Return

`return;` returns null. `return value;` returns a value. Returning from the top
level of a script is allowed but has no effect.

### Class

```bnl
class Counter {
    function init(self, start) { self.value = start; }
    function inc (self)        { self.value = self.value + 1; }
    function get (self)        { return self.value; }
}

var c = Counter(10);
c.inc();
print(c.get());     // 11
```

- First parameter is conventionally `self`. When a method uses `super`, the
  first parameter MUST be named `self`.
- `init` is the constructor. Class arity is reported as `init`'s arity minus 1.

### Single inheritance

```bnl
class Animal {
    function init(self, name) { self.name = name; }
    function speak(self)      { return self.name + " makes a sound"; }
}

class Dog extends Animal {
    function init (self, name, breed) {
        super(name);                       // calls Animal.init(self, name)
        self.breed = breed;
    }
    function speak(self) {
        return super.speak() + " (woof, I'm a " + self.breed + ")";
    }
}
```

`super.method(...)` resolves statically to the parent of the *defining* class.
Bare `super(args)` is sugar for `super.init(args)`.

### Import

```bnl
import "sys" as sys;                          // built-in native module
import "mathx" as m;                          // dep (deps/mathx/)
import "./utils.bnl" as utils;                // relative path
import "./mathx.dll" as plugin;               // direct-path FFI plugin

আমদানি "sys" যেমন sys;                       // bilingual form
```

The string is resolved by the module loader (see [Guide](./guide.md#imports-and-modules)
for the full chain).

## Indexing and member access

```bnl
xs[0]                  // list index
xs[0] = 99;            // list index set
m["key"]               // map index — returns null if missing
m.key                  // map dot-access — same as m["key"]
m.key = "v";           // map field set
obj.field              // instance field, or method (returns bound method)
obj.field = v;         // instance field set
mod.exported           // module export
```

## Built-in globals

| Name | Bangla | Signature | Notes |
|---|---|---|---|
| `print(...)` | `দেখাও` | varargs | Joins args with single space, then newline |
| `str(v)` | `লেখ` | `(any) -> string` | Same as the display form `print` produces |
| `type(v)` | `ধরণ` | `(any) -> string` | See [Types](#types) |
| `to_number(s)` | — | `(string\|number) -> number\|null` | `null` on parse failure |
| `chr(n)` | — | `(number) -> string` | Single byte as a string |
| `try_call(thunk, on_err)` | — | `(0-arg fn, 1-arg fn) -> any` | Catches runtime errors |

## Intrinsic accessors

| Container | Accessor | Returns |
|---|---|---|
| string | `.length` | codepoint count |
| string | `.byte_length` | byte count |
| string | `.byte_at(i)` | byte at index, as a number |
| string | `.byte_slice(start, end?)` | byte-level substring |
| string | `.slice(start, end?)` | codepoint-aware substring |
| string | `.char_at(i)` | i-th codepoint as a string |
| string | `.index_of(needle, start?)` | first index, or -1 |
| string | `.starts_with(s)` / `.ends_with(s)` | bool |
| string | `.split(sep)` | list of strings (empty sep splits to codepoints) |
| string | `.trim()` | strips ASCII whitespace |
| string | `.to_lower()` / `.to_upper()` | ASCII only |
| string | `.replace(needle, with)` | replaces all occurrences |
| list | `.length` | length |
| list | `.push(v)` / `.pop()` | mutate in place |
| map | `.size` | entry count (shadows any literal `size` key) |
| map | `.has(k)` | bool |
| map | `.keys()` | list of keys |

## Errors

Runtime errors abort the script. There is no `try`/`catch` syntax yet, but
`try_call(thunk, on_err)` catches runtime errors thrown from a 0-arg `thunk`:

```bnl
var result = try_call(
    function () { return risky(); },
    function (msg) { print("error:", msg); return null; }
);
```

Async callbacks (timers, io) follow the Node-style `(err, data)` convention.

## Notes / known gaps

- No `for` loop syntax (planned).
- No string interpolation, no template strings, no multi-line strings.
- No native error type / `try/catch` syntax — use `try_call`.
- Maps return `null` for missing keys (not an error). Use `.has(k)` to disambiguate
  null-valued keys from absent keys.
- Identifier-start uses a permissive rule (any non-ASCII codepoint is allowed).
  Proper Unicode identifier classification (XID_Start) is not yet wired.
