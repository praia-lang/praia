# Praia Language Documentation

Praia is a dynamically typed, interpreted programming language built in C++.

## Table of Contents

- [Getting Started](#getting-started)
- [Variables](#variables)
- [Destructuring](#destructuring)
- [Spread Operator](#spread-operator)
- [Data Types](#data-types)
- [Operators](#operators)
- [Strings](#strings)
- [Arrays](#arrays)
- [Maps](#maps)
- [Integers and Numbers](#integers-and-numbers)
- [Enums](#enums)
- [Control Flow](#control-flow)
- [Error Handling](#error-handling)
- [Loops](#loops)
- [Functions](#functions)
- [Lambdas](#lambdas)
- [Generators](#generators)
- [Classes](#classes)
- [Built-in Functions](#built-in-functions)
- [Universal Methods](#universal-methods)
- [String Methods](#string-methods)
- [Regex](#regex)
- [re Grain (Advanced Regex)](#re-grain-advanced-regex)
- [Array Methods](#array-methods)
- [The sys Namespace](#the-sys-namespace)
- [Terminal I/O](#terminal-io)
- [Pipe Operator](#pipe-operator)
- [JSON](#json)
- [YAML](#yaml)
- [Base64](#base64)
- [Path](#path)
- [URL](#url)
- [Concurrency](#concurrency)
- [Async / Await](#async--await)
- [HTTP Networking](#http-networking)
- [Router](#router)
- [Middleware](#middleware)
- [Logger](#logger)
- [Cookies](#cookies)
- [Sessions](#sessions)
- [SQLite](#sqlite)
- [Math](#math)
- [Random](#random)
- [Time](#time)
- [OS extras (sys)](#os-extras-sys)
- [Bitwise Operators](#bitwise-operators)
- [Bytes](#bytes)
- [Crypto](#crypto)
- [Networking (net)](#networking-net)
- [hex Grain](#hex-grain)
- [colors Grain](#colors-grain)
- [progress Grain](#progress-grain)
- [table Grain](#table-grain)
- [Grains (Modules)](#grains-modules)
- [Comments](#comments)
- [Operator Precedence](#operator-precedence)
- [Error Stack Traces](#error-stack-traces)
- [REPL](#repl)
- [Memory Management](#memory-management)
- [Unicode](#unicode)
- [Native Plugins](#native-plugins)
- [Command-Line Usage](#command-line-usage)

---

## Getting Started

Build and run:

```sh
make
./praia script.praia          # run a file
./praia                       # start the REPL
```

Hello world:

```
print("Hello, World!")
```

---

## Variables

Declare variables with `let`. Uninitialized variables are `nil`.

```
let name = "Ada"
let age = 36
let score              // nil

age = 37               // reassignment
```

### Constants (convention)

Use `UPPER_SNAKE_CASE` names for values that shouldn't change. Praia warns on reassignment:

```
let MAX_RETRIES = 3
let BASE_URL = "https://api.example.com"

MAX_RETRIES = 5        // Warning: reassigning constant 'MAX_RETRIES'
```

This is a convention, not a hard error — the reassignment still happens, but the warning signals a likely mistake. Single-letter names like `N` or `X` do not trigger the warning.

---

## Destructuring

Unpack arrays and maps into variables in a single `let` statement.

### Array destructuring

```
let [a, b, c] = [1, 2, 3]
print(a, b, c)              // 1 2 3

let [first, ...rest] = [1, 2, 3, 4, 5]
print(first)                 // 1
print(rest)                  // [2, 3, 4, 5]
```

Missing elements become `nil`:
```
let [x, y, z] = [1, 2]
print(z)                     // nil
```

### Map destructuring

```
let {name, age} = {name: "Ada", age: 36}
print(name, age)             // Ada 36
```

Rename with `key: varName`:
```
let {name: userName, age: userAge} = {name: "Ada", age: 36}
print(userName)              // Ada
```

Rest collects remaining keys:
```
let {name, ...other} = {name: "Ada", age: 36, lang: "Praia"}
print(other)                 // {age: 36, lang: "Praia"}
```

---

## Spread Operator

The `...` operator spreads arrays and maps into literals.

### Array spread

```
let a = [1, 2, 3]
let b = [4, 5, 6]
let combined = [...a, ...b]       // [1, 2, 3, 4, 5, 6]
let withExtra = [0, ...a, 99]     // [0, 1, 2, 3, 99]
```

### Map spread

```
let defaults = {host: "localhost", port: 8080}
let overrides = {port: 3000, debug: true}
let config = {...defaults, ...overrides}
// {host: "localhost", port: 3000, debug: true}
```

Later spreads override earlier keys (like `Object.assign` in JavaScript).

### Spread in function calls

Spread an array as arguments to a function:

```
func add(a, b, c) { return a + b + c }
let args = [1, 2, 3]
print(add(...args))       // 6

// Mixed positional and spread
func f(a, b, c, d) { return [a, b, c, d] }
print(f(1, 2, ...[3, 4])) // [1, 2, 3, 4]
```

This enables generic function wrappers:

```
func wrapper(fn) {
    return lam{ ...args in fn(...args) }
}
```

---

## Data Types

`type(v)` returns one of the following strings. These are the values you'll
see at runtime; use them with the `is` operator (`x is "int"`, `x is MyClass`).

| Type        | Examples                            | Notes |
|-------------|-------------------------------------|-------|
| `nil`       | `nil`                               | The absence of a value. |
| `bool`      | `true`, `false`                     | |
| `int`       | `42`, `0xFF`, `0b1010`, `0o17`      | 64-bit integer (exact up to 2^63). Supports hex, binary, octal. |
| `float`     | `3.14`, `1e3`, `2.5e-4`             | IEEE-754 double. Supports scientific notation. |
| `string`    | `"hello"`                           | UTF-8, supports interpolation, escape sequences, and Unicode (`\u{...}`). |
| `array`     | `[1, 2, 3]`                         | Ordered, mixed-type, reference semantics. |
| `map`       | `{name: "Ada", [42]: "answer"}`     | Hash map. Reference semantics. Keys can be any hashable value: `nil`, bool, int, float, string, or tagged value. Bare-identifier keys (`name`) desugar to string keys; bracket-expression keys (`[42]`) take the evaluated value. |
| `function`  | `func add(a, b) { ... }`            | First-class, supports closures. Includes user `func` / `lam` and built-in natives. |
| `instance`  | `MyClass(...)`                      | Instance of a user-defined `class`. Use `x is MyClass` to check class identity (walks the `extends` chain). |
| `tagged`    | `Ok(42)`, `Err("nope")`             | Tagged values from a `CapitalizedName(args...)` constructor call on an undefined identifier. Used for sum types / result-like patterns; see the `match` section. |
| `future`    | `async f()`                         | Handle for an in-flight async task. `await` blocks for the result. |
| `generator` | `g()` where `g` contains `yield`    | Iterator produced by a generator function. Use `.next()` / `for x in g`. |

Classes themselves (the `class X { ... }` declaration value) are also `function`-typed — they're callable to construct an instance.

### Number Literals

Integers support multiple bases and underscores as visual separators:

```
42                // decimal
0xFF              // hex
0b1010            // binary
0o755             // octal
1_000_000         // underscores ignored (readability)
0xFF_FF           // works in any base
```

Floats support decimal points and scientific notation:

```
3.14              // decimal float
1e3               // 1000.0 (scientific notation)
2.5e-4            // 0.00025
1_000.5           // separators in floats too
```

Integer overflow automatically promotes to float rather than wrapping.

### Truthiness

Only `nil` and `false` are falsy. Everything else — including `0`, `""` (empty string), and `[]` (empty array) — is truthy.

```
if (0)       { print("truthy") }   // prints (0 is truthy)
if ("")      { print("truthy") }   // prints (empty string is truthy)
if ([])      { print("truthy") }   // prints (empty array is truthy)
if (nil)     { print("truthy") }   // does not print
if (false)   { print("truthy") }   // does not print
```

To check for empty strings or arrays, use `len()`:

```
if (len(name) > 0) { print("has name") }
if (len(items) > 0) { print("has items") }
```

---

## Operators

### Arithmetic

```
2 + 3       // 5
10 - 4      // 6
3 * 7       // 21
15 / 4      // 3.75
17 % 5      // 2
[1,2] + [3,4]  // [1, 2, 3, 4]  (array concat)
```

### Comparison

Works on numbers and strings (lexicographic ordering).

```
3 < 5           // true
3 >= 5          // false
"apple" < "banana"  // true
"abc" <= "abc"      // true
```

### Equality

Works on any types. Arrays and maps compare by value.

```
1 == 1              // true
"hi" == "hi"        // true
[1, 2] == [1, 2]    // true
nil == nil           // true
1 == "1"             // false
```

**Floating-point note:** `==` uses exact equality for numbers, like all major languages. Due to IEEE 754 representation, `0.1 + 0.2 != 0.3`. Use `math.approx()` for approximate comparison:

```
0.1 + 0.2 == 0.3              // false (floating-point)
math.approx(0.1 + 0.2, 0.3)   // true
```

### Logical

`&&` and `||` short-circuit and return the deciding value, not just `true`/`false`.

```
true && "yes"       // "yes"
false && "yes"      // false
nil || "default"    // "default"
true || "other"     // true
!true               // false
!nil                // true
```

### Type checking (`is`)

The `is` operator checks types and class hierarchy. Use a string for primitive types, or a class for instanceof checks.

```
42 is "int"             // true
"hello" is "string"     // true
nil is "nil"            // true
[1, 2] is "array"       // true

class Animal {}
class Dog extends Animal {}
let d = Dog()
d is Dog                // true
d is Animal             // true (walks inheritance chain)
```

Supported type names: `"nil"`, `"bool"`, `"int"`, `"float"`, `"string"`, `"array"`, `"map"`, `"function"`, `"instance"`.

Negate with `!`: `!(x is "string")`.

### Ternary

```
let label = x > 5 ? "big" : "small"
let grade = score >= 90 ? "A" : score >= 80 ? "B" : "C"   // nests right-to-left
```

### Optional Chaining

`?.` accesses a property only if the object is non-nil. Returns `nil` if the object is `nil` or the field doesn't exist.

```
let user = {address: {city: "Lisbon"}}
print(user?.address?.city)    // "Lisbon"
print(user?.phone?.number)    // nil (no error)

let x = nil
print(x?.name)                // nil
```

`?[` does the same for index access:

```
let arr = nil
print(arr?[0])                // nil
```

### Nil Coalescing

`??` returns the left side if it's non-nil, otherwise evaluates and returns the right side. The right side is only evaluated if needed (short-circuit).

```
let name = nil ?? "anonymous"      // "anonymous"
let port = config?.port ?? 8080    // 8080 if port is nil
let x = 0 ?? 42                   // 0 (not nil, so left wins)
let y = false ?? true              // false (not nil)
```

Chains naturally with `?.`:

```
let city = user?.address?.city ?? "unknown"
```

### Compound Assignment

```
let x = 10
x += 5              // x is now 15
x -= 3              // x is now 12
x *= 2              // x is now 24
x /= 4              // x is now 6
x %= 4              // x is now 2
```

### Increment / Decrement

```
let i = 0
i++                 // i is now 1
i--                 // i is now 0
```

### String Concatenation

`+` concatenates when either side is a string:

```
"hello " + "world"  // "hello world"
"count: " + 42      // "count: 42"
```

---

## Strings

Strings are enclosed in double quotes.

### Escape Sequences

| Escape | Meaning |
|--------|---------|
| `\n` | Newline |
| `\t` | Tab |
| `\r` | Carriage return |
| `\0` | Null byte |
| `\\` | Backslash |
| `\"` | Double quote |
| `\'` | Single quote |
| `\%` | Literal `%` (prevents interpolation) |
| `\xHH` | Byte from 2-digit hex value |
| `\u{HHHH}` | Unicode codepoint (1-6 hex digits) |

#### Unicode escapes

`\u{...}` inserts any Unicode codepoint by its hex value:

```
"\u{E9}"        // "e" (e-acute)
"\u{1F600}"     // "😀"
"\u{1F1F5}\u{1F1F9}"  // "🇵🇹" (flag)
```

### String Interpolation

Use `%{expression}` inside strings:

```
let name = "Ada"
let age = 36
print("%{name} is %{age} years old")
// Ada is 36 years old

print("2 + 2 = %{2 + 2}")
// 2 + 2 = 4
```

### Multiline Strings

Triple-quoted strings (`"""` or `'''`) span multiple lines. The first newline after the opening quotes is stripped.

```
let html = """
<html>
  <body>
    <h1>%{title}</h1>
  </body>
</html>
"""
```

Interpolation and escape sequences work inside triple-quoted strings.

### String Indexing

Strings are indexed by grapheme cluster (visible character), not by byte. This means emoji and accented characters work correctly.

```
let s = "hello"
print(s[0])         // h
print(s[-1])        // o (negative = from end)

let emoji = "Hi👋"
print(len(emoji))   // 3
print(emoji[2])     // 👋
```

---

## Arrays

Arrays are ordered, mixed-type collections with reference semantics.

```
let nums = [1, 2, 3]
let mixed = [1, "two", true, nil]
let empty = []
```

### Index Access and Assignment

```
let arr = [10, 20, 30]
print(arr[0])       // 10
print(arr[-1])      // 30

arr[1] = 99
print(arr)          // [10, 99, 30]
```

### Nested Arrays

```
let matrix = [[1, 2], [3, 4]]
print(matrix[1][0]) // 3
```

### Reference Semantics

```
let a = [1, 2, 3]
let b = a           // b points to the same array
b.push(4)
print(a)            // [1, 2, 3, 4]
```

---

## Maps

Maps hold key-value pairs. Keys can be any hashable value: strings, integers, floats, booleans, or nil.

```
let person = {name: "Ada", age: 36}
let config = {"api-key": "abc123"}
let empty = {}
```

### Computed Keys

Use `[expr]` for computed or non-string keys in map literals:

```
let m = {[42]: "answer", [true]: "yes", name: "Ada"}
print(m[42])       // answer
print(m[true])     // yes
print(m.name)      // Ada
```

Identifier keys like `name:` are sugar for the string key `"name":`.

### Access and Assignment

```
// Dot notation (string keys only)
print(person.name)          // Ada
person.email = "ada@ex.com"

// Bracket notation (any key type)
print(person["name"])       // Ada
person["city"] = "London"
person[1] = "one"           // integer key
```

Arrays, maps, instances, and functions cannot be used as keys (they are not hashable).

### Reference Semantics

Maps, like arrays, use reference semantics:

```
let a = {x: 1}
let b = a
b.y = 2
print(a)            // {x: 1, y: 2}
```

### Map Methods

| Method | Description |
|--------|-------------|
| `.has(key)` | Returns `true` if the key exists |
| `.get(key, default?)` | Returns the value for key, or default (nil if omitted) |
| `.delete(key)` | Removes the key, returns `true` if it existed |
| `.merge(other)` | Returns a new map combining both, with `other`'s values taking priority |
| `.entries()` | Returns `[[key, value], ...]` array of all entries |
| `.clear()` | Remove all entries from the map |

```
let m = {a: 1, b: 2}
m.has("a")           // true
m.get("z", 0)        // 0
m.delete("b")        // true, m is now {a: 1}
let m2 = {a: 1}.merge({b: 2, a: 99})  // {a: 99, b: 2}
```

Note: map entries named `has`, `get`, `delete`, or `merge` shadow the methods.

---

## Integers and Numbers

Praia has two numeric types: 64-bit integers (`int`) and double-precision floats (`float`). Integer literals (no decimal point) create ints; decimal literals create floats.

```
type(42)        // "int"
type(3.14)      // "float"
```

### Arithmetic rules

- `int + int`, `int - int`, `int * int`, `int % int` → int
- Anything involving a double → double
- `/` always returns double: `7 / 2` → `3.5` (like Python 3)

```
42 + 8          // 50 (int)
42 + 0.5        // 42.5 (number)
7 / 2           // 3.5 (always double)
7 % 2           // 1 (int)
```

### Large integers

Integers are 64-bit, so they're exact up to 2^63. No precision loss like doubles above 2^53:

```
let big = 9007199254740993
print(big + 1)     // 9007199254740994 (exact)
```

### Comparison

Ints and doubles compare by value: `42 == 42.0` is `true`.

---

## Enums

Enums create named constants with auto-incrementing integer values.

```
enum Color { Red, Green, Blue }
print(Color.Red)      // 0
print(Color.Green)    // 1
print(Color.Blue)     // 2
```

### Custom values

```
enum Status { Active = 1, Inactive = 0, Pending = 2 }

if (status == Status.Active) {
    print("active")
}
```

### Auto-increment continues from the last value

```
enum Level { Low = 10, Medium, High }
print(Level.Medium)   // 11
print(Level.High)     // 12
```

Enums are maps — you can pass them around, iterate their keys, etc.

## Tagged Values

Tagged values are data-carrying variants — like Rust enums with data. Any capitalized function call to an undefined name creates a tagged value:

```
let result = Ok(42)
let error = Err("not found")
let point = Point(1, 2)
let nothing = None()

print(result)       // Ok(42)
print(type(result)) // "tagged"
print(result.tag)   // "Ok"
print(result.values) // [42]
```

### Pattern matching

Tagged values work with `match` for destructuring:

```
match (result) {
    Ok(val) { print("success: " + str(val)) }
    Err(msg) { print("error: " + msg) }
}

match (Point(3, 4)) {
    Point(x, y) { print("sum: " + str(x + y)) }   // 7
}

match (RGB(255, 0, 128)) {
    RGB(r, g, b) { print(r) }   // 255
}
```

### Equality

Two tagged values are equal if their tag name and all values match:

```
Ok(1) == Ok(1)     // true
Ok(1) == Ok(2)     // false
Ok(1) == Err(1)    // false
```

### Coexistence with classes

Class constructors take priority. Tagged values only apply when the name isn't defined:

```
class Foo {}
let f = Foo()   // class instance
let t = Bar(1)  // tagged value (Bar is not a class)
```

---

## Control Flow

### if / elif / else

Conditions are always wrapped in parentheses. Bodies use braces.

```
let score = 85

if (score >= 90) {
    print("A")
} elif (score >= 80) {
    print("B")
} elif (score >= 70) {
    print("C")
} else {
    print("F")
}
```

Truthiness check:

```
let name = nil
if (name) {
    print(name)
} else {
    print("no name set")
}
```

### match

Match a value against multiple cases. Cases are tested top-to-bottom; first match wins. Use `_` for the default case.

```
let cmd = "stop"

match (cmd) {
    "start" { print("starting") }
    "stop" { print("stopping") }
    "restart" { print("restarting") }
    _ { print("unknown command") }
}
```

Cases can be any expression (compared with `==`, which respects `__eq` operator overloading):

```
let x = 10

match (x) {
    5 * 2 { print("ten") }
    5 * 3 { print("fifteen") }
    _ { print("other") }
}
// ten
```

#### Type patterns with `is`

Use `is` to match by type name or class:

```
match (value) {
    is "int"    { print("integer") }
    is "string" { print("string") }
    is "array"  { print("array") }
    is MyClass  { print("a MyClass instance") }
    _           { print("something else") }
}
```

Type names: `"nil"`, `"bool"`, `"int"`, `"float"`, `"string"`, `"array"`, `"map"`, `"function"`, `"instance"`. Class names check inheritance (matches the class and any subclass).

#### Guard clauses with `when`

Use `when` for conditional matching:

```
let score = 85

match (score) {
    when score >= 90 { print("A") }
    when score >= 80 { print("B") }
    when score >= 70 { print("C") }
    _                { print("F") }
}
// B
```

#### Mixing patterns

Equality, type, and guard patterns can be freely mixed in a single match:

```
let x = -5

match (x) {
    0           { print("zero") }
    when x > 0  { print("positive") }
    when x < 0  { print("negative") }
}
```

If no case matches and there's no default, nothing happens.

---

## Error Handling

### try / catch

Wrap code that might fail in a `try` block. If an error occurs — either from `throw` or a runtime error — execution jumps to the `catch` block with the error value.

```
try {
    let data = fs.read("config.txt")
    print(data)
} catch (err) {
    print("failed to read config:", err)
}
```

### finally

Add a `finally` block for cleanup that always runs — whether the try succeeds, the catch runs, or an exception is re-thrown.

```
let file = fs.read("data.txt")
try {
    process(file)
} catch (err) {
    print("error:", err)
} finally {
    cleanup()    // always runs
}
```

### throw

Throw any value as an error. If not caught by a `try/catch`, the program terminates.

```
func divide(a, b) {
    if (b == 0) {
        throw "division by zero"
    }
    return a / b
}

try {
    print(divide(10, 0))
} catch (err) {
    print("error:", err)     // error: division by zero
}
```

You can throw any value — strings, numbers, maps:

```
throw {code: 404, message: "not found"}
```

Runtime errors (type errors, index out of bounds, etc.) are also caught:

```
try {
    let arr = [1, 2, 3]
    print(arr[99])
} catch (err) {
    print(err)              // Array index out of bounds
}
```

### defer

`defer` registers an expression to run when the enclosing function exits — whether by normal return, explicit `return`, or thrown exception. Multiple defers execute in LIFO order (last registered runs first).

```
func processFile(path) {
    let db = sqlite.open("app.db")
    defer db.close()

    let sock = net.connect("host", 80)
    defer net.close(sock)

    // ... use db and sock ...
    // On function exit: net.close(sock) runs first, then db.close()
}
```

Defers run even if an exception is thrown:

```
func f() {
    defer print("cleanup")
    throw "error"
}
try { f() } catch (e) {}
// prints "cleanup" before the catch
```

If a defer itself throws, it doesn't prevent other defers from running.

### ensure

`ensure` is an early-exit guard (like Swift's `guard`). If the condition is falsy, the `else` block runs — which should exit the scope (typically `return` or `throw`).

```
func greet(name) {
    ensure (name) else {
        print("no name provided")
        return
    }
    print("hello %{name}!")
}

greet("Ada")    // hello Ada!
greet(nil)      // no name provided
```

`ensure` is useful for input validation at the top of functions:

```
func processAge(age) {
    ensure (type(age) == "int") else {
        throw "age must be a number"
    }
    ensure (age >= 0 && age <= 150) else {
        throw "age out of range"
    }
    print("valid age: %{age}")
}
```

### Interrupts (Ctrl+C)

Pressing Ctrl+C throws `"Interrupted"`, which is catchable with `try/catch`:

```
try {
    for (i in 0..999999999) {
        doWork(i)
    }
} catch (err) {
    print("stopped: " + str(err))   // "stopped: Interrupted"
    cleanup()
}
```

Without a `try/catch`, Ctrl+C prints `Uncaught error: Interrupted` and exits. This is similar to Python's `KeyboardInterrupt`.

---

## Loops

### while

```
let i = 0
while (i < 5) {
    print(i)
    i++
}
```

### for (range)

`for (var in start..end)` — end is exclusive.

```
for (i in 0..5) {
    print(i)            // 0, 1, 2, 3, 4
}

// Expressions work as bounds
let n = 10
for (i in 1..n + 1) {
    print(i)            // 1 through 10
}
```

### for-in (arrays)

```
let names = ["alice", "bob", "charlie"]
for (name in names) {
    print("hello %{name}")
}
```

### for-in (maps)

Iterating a map yields `{key, value}` entries. You can destructure directly in the loop:

```
let config = {host: "localhost", port: 8080}

// Destructuring (preferred)
for ({key, value} in config) {
    print("%{key}: %{value}")
}

// Without destructuring
for (entry in config) {
    print("%{entry.key}: %{entry.value}")
}
```

### break and continue

`break` exits the innermost loop. `continue` skips to the next iteration. Both work in `while`, `for`, and `for-in` loops.

```
// Skip odd numbers
for (i in 0..10) {
    if (i % 2 != 0) { continue }
    print(i)                        // 0, 2, 4, 6, 8
}

// Stop at first match
let names = ["alice", "bob", "charlie"]
for (name in names) {
    if (name == "bob") { break }
    print(name)                     // alice
}

// break in while
let n = 0
while (true) {
    if (n >= 3) { break }
    print(n)                        // 0, 1, 2
    n++
}
```

In nested loops, `break` and `continue` only affect the innermost loop.

---

## Functions

Define functions with `func`. Functions are first-class values.

```
func add(a, b) {
    return a + b
}

print(add(2, 3))    // 5
```

### Default parameters

Parameters can have default values. Non-default parameters must come before default ones.

```
func greet(name, greeting = "Hello") {
    print("%{greeting}, %{name}!")
}

greet("Ada")              // Hello, Ada!
greet("Ada", "Welcome")   // Welcome, Ada!
```

Defaults also work in lambdas and class methods:

```
let inc = lam{ x, step = 1 in x + step }
print(inc(10))       // 11
print(inc(10, 5))    // 15

class Server {
    func init(port = 8080) {
        this.port = port
    }
}
```

### Rest parameters

Use `...name` as the last parameter to collect all remaining arguments into an array:

```
func log(level, ...messages) {
    print("[" + level + "]", messages.join(" "))
}

log("INFO", "server", "started", "on", "8080")
// [INFO] server started on 8080
```

Rest parameters work in functions, lambdas, and class methods:

```
let sum = lam{ ...nums in
    let total = 0
    for (n in nums) { total = total + n }
    return total
}
print(sum(1, 2, 3, 4))    // 10

class Logger {
    func init(prefix) { this.prefix = prefix }
    func log(...args) { print(this.prefix, args.join(" ")) }
}
```

If no extra arguments are passed, the rest parameter is an empty array.

### Named arguments

Arguments can be passed by name using `name: value` syntax. Positional arguments must come first; once a named argument appears, all remaining arguments must be named.

```
func createUser(name, age, role = "user") {
    return {name: name, age: age, role: role}
}

createUser("Ada", 36)                       // all positional
createUser("Ada", role: "admin", age: 36)   // mixed
createUser(name: "Ada", age: 36)            // all named, role uses default
```

Named arguments work with lambdas, class constructors, and the pipe operator:

```
let add = lam{ a, b in a + b }
add(b: 10, a: 5)          // 15

class Point {
    func init(x, y) { this.x = x; this.y = y }
}
let p = Point(y: 20, x: 10)

// Pipe — the left value is the first positional arg
func format(value, prefix = "", suffix = "") {
    return prefix + str(value) + suffix
}
42 |> format(suffix: "!")  // "42!"
```

Unknown parameter names and duplicate names throw a runtime error. Native built-in functions do not support named arguments.

### Implicit nil Return

Functions without an explicit `return` return `nil`.

```
func greet(name) {
    print("hello %{name}")
}
```

### Closures

Functions capture their enclosing scope:

```
func makeCounter() {
    let count = 0
    func increment() {
        count = count + 1
        return count
    }
    return increment
}

let counter = makeCounter()
print(counter())    // 1
print(counter())    // 2
print(counter())    // 3
```

### Recursion

```
func fib(n) {
    if (n <= 1) { return n }
    return fib(n - 1) + fib(n - 2)
}
print(fib(10))      // 55
```

### Functions as Values

```
func apply(f, x) {
    return f(x)
}

func double(n) { return n * 2 }

print(apply(double, 21))   // 42
```

### Decorators

Decorators wrap a function with another function using the `@` syntax. They are pure syntactic sugar — `@dec func f(){}` desugars to `func f(){}; f = dec(f)`.

```
func log(fn) {
    return lam{ ...args in
        print("calling " + str(fn))
        return fn(...args)
    }
}

@log
func add(a, b) { return a + b }

add(2, 3)   // prints "calling <function add>", returns 5
```

Multiple decorators are applied bottom-up (innermost first):

```
@auth
@log
func handler(req) { ... }
// equivalent to: handler = auth(log(handler))
```

Decorators can take arguments by calling the decorator to produce the wrapper:

```
func role(required) {
    return lam{ fn in
        return lam{ ...args in
            print("checking role: " + required)
            return fn(...args)
        }
    }
}

@role("admin")
func deleteUser(id) { ... }
```

Decorators also work on class methods (both instance and static):

```
func logged(fn) {
    return lam{ ...args in
        print("calling")
        return fn(...args)
    }
}

class API {
    @logged
    func fetch(url) { return http.get(url) }

    @logged
    static func version() { return "1.0" }
}
```

---

## Lambdas

Lambdas are anonymous functions defined inline with `lam{ params in body }`.

### Single expression (auto-returned)

```
let double = lam{ x in x * 2 }
let add = lam{ a, b in a + b }

print(double(5))        // 10
print(add(3, 4))        // 7
```

A single-expression lambda automatically returns its result — no `return` needed.

### Multi-line (explicit return)

```
let process = lam{ x, y in
    let sum = x + y
    let product = x * y
    return {sum: sum, product: product}
}
```

### No parameters

```
let sayHi = lam{ in print("hello!") }
sayHi()
```

### Passing lambdas to functions

Lambdas are ideal for callbacks, filters, and transforms:

```
func filter(arr, predicate) {
    let result = []
    for (item in arr) {
        if (predicate(item)) { result.push(item) }
    }
    return result
}

func map(arr, transform) {
    let result = []
    for (item in arr) { result.push(transform(item)) }
    return result
}

let nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
let evens = filter(nums, lam{ n in n % 2 == 0 })
let squares = map(nums, lam{ n in n * n })

print(evens)            // [2, 4, 6, 8, 10]
print(squares)          // [1, 4, 9, 16, 25, 36, 49, 64, 81, 100]
```

### Closures

Lambdas capture their enclosing scope, just like named functions:

```
func makeMultiplier(factor) {
    return lam{ x in x * factor }
}

let triple = makeMultiplier(3)
print(triple(5))        // 15
```

### Lambdas in maps

```
let actions = {
    double: lam{ x in x * 2 },
    negate: lam{ x in -x }
}
print(actions.double(21))   // 42
```

---

## Generators

A generator is a function whose body contains `yield`. Calling it returns a **generator object** instead of executing the body. The body runs lazily, pausing at each `yield` and resuming on the next `.next()` call.

No special keyword is needed — any function or lambda that uses `yield` becomes a generator automatically.

### Basic usage

```
func countdown(n) {
    while (n > 0) { yield n; n = n - 1 }
}

let g = countdown(3)
print(g.next())   // {value: 3, done: false}
print(g.next())   // {value: 2, done: false}
print(g.next())   // {value: 1, done: false}
print(g.next())   // {value: nil, done: true}
```

`.next()` returns a map with `value` (the yielded value) and `done` (whether the generator is exhausted). After the last yield, `done` becomes `true`.

### for-in integration

Generators work directly with `for-in` loops. Iteration is lazy — values are produced one at a time, not materialized into an array.

```
func range(n) {
    for (i in 0..n) { yield i }
}

for (x in range(5)) { print(x) }   // 0 1 2 3 4
```

### Infinite generators

Since generators are lazy, they can produce infinite sequences. Use `break` to stop.

```
func naturals() {
    let n = 0
    while (true) { yield n; n = n + 1 }
}

for (x in naturals()) {
    if (x >= 5) { break }
    print(x)    // 0 1 2 3 4
}
```

### yield as expression (send values)

`yield` is an expression that returns the value passed to `.next(arg)`. The first `.next()` call primes the generator (runs until the first yield); subsequent calls resume execution with the sent value.

```
func accumulator() {
    let total = 0
    while (true) {
        let val = yield total
        total = total + val
    }
}

let acc = accumulator()
acc.next()              // prime — {value: 0, done: false}
acc.next(10)            // {value: 10, done: false}
acc.next(5)             // {value: 15, done: false}
acc.next(25)            // {value: 40, done: false}
```

### Generator lambdas

Lambdas can be generators too.

```
let squares = lam{ n in for (i in 0..n) { yield i * i } }

for (x in squares(5)) { print(x) }   // 0 1 4 9 16
```

### Return value

A `return` inside a generator sets `done: true` and the return value as the final `value`.

```
func gen() {
    yield 1
    return 99
}

let g = gen()
print(g.next())   // {value: 1, done: false}
print(g.next())   // {value: 99, done: true}
```

### Generator properties

| Property/Method | Description |
|---|---|
| `.next()` | Resume, return `{value, done}` |
| `.next(val)` | Resume with sent value |
| `.done` | `true` if generator is exhausted |

---

## Classes

### Defining a class

```
class Animal {
    func init(name, sound) {
        this.name = name
        this.sound = sound
    }

    func speak() {
        print("%{this.name} says %{this.sound}")
    }
}
```

- `class` keyword defines a class
- `func init` is the constructor (called automatically when creating instances)
- `this` refers to the current instance
- Methods use `func` just like top-level functions

### Creating instances

Call the class like a function — no `new` keyword:

```
let cat = Animal("Whiskers", "meow")
cat.speak()         // Whiskers says meow
```

### Properties

Properties are set on `this` inside methods and accessed with dot notation:

```
print(cat.name)     // Whiskers
cat.name = "Luna"   // reassignment works
cat.speak()         // Luna says meow
```

### Inheritance

Use `extends` for single inheritance:

```
class Dog extends Animal {
    func init(name) {
        super.init(name, "woof")
        this.tricks = []
    }

    func learn(trick) {
        this.tricks.push(trick)
    }
}

let buddy = Dog("Buddy")
buddy.speak()           // Buddy says woof (inherited)
buddy.learn("sit")
```

### super

Use `super.method()` to call the parent class's version of a method:

```
class Cat extends Animal {
    func init(name) {
        super.init(name, "meow")
    }

    func describe() {
        return "%{this.name} the cat"
    }
}
```

`super` works correctly with multi-level inheritance (e.g., Kitten -> Cat -> Animal).

### Method overriding

Child classes can override parent methods. The child's version is used:

```
class Animal {
    func describe() { return "an animal" }
}

class Cat extends Animal {
    func describe() { return "a cat" }
}

let c = Cat()
print(c.describe())    // a cat
c.speak()              // still works (inherited from Animal)
```

### Classes are values

Classes are first-class — they can be stored in variables and passed around:

```
let MyClass = Animal
let a = MyClass("Rex", "woof")
a.speak()
```

### Instance equality

Instances use reference equality:

```
let a = Animal("Rex", "woof")
let b = a
print(a == b)       // true (same reference)

let c = Animal("Rex", "woof")
print(a == c)       // false (different instances)
```

### Operator overloading

Classes can define special "dunder" methods to customize how operators work on instances.

```
class Vec {
    func init(x, y) { this.x = x; this.y = y }
    func __add(other) { return Vec(this.x + other.x, this.y + other.y) }
    func __eq(other)  { return this.x == other.x && this.y == other.y }
    func __neg()      { return Vec(-this.x, -this.y) }
    func __str()      { return "(%{this.x}, %{this.y})" }
    func __len()      { return 2 }
    func __index(key) { if (key == 0) { return this.x } return this.y }
    func __indexSet(key, val) {
        if (key == 0) { this.x = val } else { this.y = val }
    }
}

let a = Vec(1, 2) + Vec(3, 4)   // Vec(4, 6)
print(-a)                        // (-4, -6)
print(a == Vec(4, 6))            // true
print(len(a))                    // 2
print(a[0])                      // 4
```

| Method | Operators |
|--------|-----------|
| `__add(other)` | `+` |
| `__sub(other)` | `-` (binary) |
| `__mul(other)` | `*` |
| `__div(other)` | `/` |
| `__mod(other)` | `%` |
| `__eq(other)` | `==`, `!=` (negated) |
| `__lt(other)` | `<`, `>=` (negated) |
| `__gt(other)` | `>`, `<=` (negated) |
| `__neg()` | unary `-` |
| `__str()` | `str()`, string interpolation |
| `__len()` | `len()` |
| `__index(key)` | `obj[key]` |
| `__indexSet(key, val)` | `obj[key] = val` |

The existing `toString()` method convention continues to work — `str()` checks `__str` first, then falls back to `toString()` for backwards compatibility.

Classes without operator overloads use default behavior (reference equality for `==`, errors for arithmetic).

### Static methods

Define class-level methods with `static func`. Static methods are called on the class, not on instances, and don't receive `this`.

```
class Point {
    func init(x, y) { this.x = x; this.y = y }
    static func origin() { return Point(0, 0) }
    static func fromArray(arr) { return Point(arr[0], arr[1]) }
}

let p = Point.origin()          // factory method
let q = Point.fromArray([3, 4]) // another factory
```

Static methods are inherited by subclasses and can be overridden:

```
class Animal {
    static func type() { return "animal" }
}
class Dog extends Animal {
    static func type() { return "dog" }
}
print(Dog.type())    // "dog"
```

---

## Built-in Functions

| Function | Description |
|----------|-------------|
| `print(args...)` | Print values separated by spaces, followed by a newline |
| `len(value)` | Length of an array, string, or map |
| `push(array, value)` | Append a value to an array |
| `pop(array)` | Remove and return the last element of an array |
| `type(value)` | Return the type as a string: `"nil"`, `"bool"`, `"int"`, `"float"`, `"string"`, `"array"`, `"map"`, `"function"` |
| `str(value)` | Convert any value to a string |
| `num(value)` | Convert a string or number to a number |
| `filter(arr, fn)` | Keep elements where fn returns truthy |
| `map(arr, fn)` | Transform each element |
| `each(arr, fn)` | Call fn on each element, returns the array |
| `sort(arr)` | Return sorted copy (ascending) |
| `keys(map)` | Return array of map keys |
| `values(map)` | Return array of map values |

```
print(len([1, 2, 3]))      // 3
print(len("hello"))         // 5
print(len({a: 1, b: 2}))   // 2
print(len("café"))          // 4 (grapheme clusters, not bytes)
print(len("👨‍👩‍👧‍👦"))              // 1 (family emoji = 1 grapheme)

print(type(42))             // int
print(type(3.14))           // float
print(type("hi"))           // string

print(str(42) + "!")        // 42!
print(num("3.14") * 2)     // 6.28
```

---

## Universal Methods

These work on any value type via dot notation.

| Method | Description |
|--------|-------------|
| `.toString()` | Convert any value to its string representation |
| `.toNum()` | Convert to number — works on numbers (identity), bools (`true`=1, `false`=0), and numeric strings. Also handles `"true"`/`"false"` (case-insensitive). Throws on invalid strings. |

```
42.toString()           // "42"
true.toString()         // "true"
[1, 2].toString()       // "[1, 2]"

true.toNum()            // 1
false.toNum()           // 0
"3.14".toNum()          // 3.14
"TRUE".toNum()          // 1
"hello".toNum()         // Error: Cannot convert 'hello' to number
```

---

## String Methods

Methods are called with dot notation on string values.

| Method | Description |
|--------|-------------|
| `.upper()` | Uppercase copy |
| `.lower()` | Lowercase copy |
| `.strip()` | Remove leading/trailing whitespace |
| `.split(sep)` | Split into array by separator |
| `.contains(sub)` | Check if substring exists |
| `.replace(old, new)` | Replace all occurrences |
| `.startsWith(prefix)` | Check prefix |
| `.endsWith(suffix)` | Check suffix |
| `.title()` | Capitalize first letter of each word, lowercase the rest |
| `.capitalize()` | Capitalize first letter, lowercase the rest |
| `.capitalizeFirst()` | Capitalize first letter, leave the rest intact |
| `.slice(start, end?)` | Extract substring (negative indices supported) |
| `.indexOf(substr, start?)` | Find first position (-1 if not found) |
| `.lastIndexOf(substr)` | Find last position (-1 if not found) |
| `.repeat(count)` | Repeat string N times |
| `.padStart(len, char?)` | Left-pad to width (default: space) |
| `.padEnd(len, char?)` | Right-pad to width (default: space) |
| `.center(width, char?)` | Center-pad both sides (default: space) |
| `.count(substr)` | Count non-overlapping occurrences of substring |
| `.isDigit()` | `true` if all characters are digits |
| `.isAlpha()` | `true` if all characters are alphabetic |
| `.isAlnum()` | `true` if all characters are alphanumeric |
| `.isSpace()` | `true` if all characters are whitespace |
| `.isUpper()` | `true` if all alphabetic characters are uppercase |
| `.isLower()` | `true` if all alphabetic characters are lowercase |
| `.trimStart()` | Remove leading whitespace |
| `.trimEnd()` | Remove trailing whitespace |
| `.graphemes()` | Split into array of grapheme clusters |
| `.codepoints()` | Array of Unicode codepoint values (integers) |
| `.bytes()` | Array of raw byte values (integers) |

All positional methods (`len`, indexing, `slice`, `split("")`, `indexOf`, `padStart`, `padEnd`) operate on **grapheme clusters** — visible characters, not bytes. This means emoji, accented characters, and flags all count as single units.

```
"hello".upper()                  // "HELLO"
"  hello  ".strip()              // "hello"
"a,b,c".split(",")              // ["a", "b", "c"]
"hello world".contains("world") // true
"hello".replace("l", "r")       // "herro"
"hello".startsWith("hel")      // true

// Casing variants
"how old is Ada?".title()           // "How Old Is Ada?"
"how old is Ada?".capitalize()      // "How old is ada?"
"how old is Ada?".capitalizeFirst() // "How old is Ada?"

// Chaining works
"  Hello World  ".strip().lower()   // "hello world"

// Unicode-aware case conversion
"café".upper()                      // "CAFÉ"
"ÜBER".lower()                      // "über"

// Grapheme-aware operations
"Hi👋".slice(0, 2)                  // "Hi"
"A😀BC".indexOf("B")               // 2

// Inspecting string internals
"A😀".graphemes()                   // ["A", "😀"]
"A😀".codepoints()                  // [65, 128512]
"é".bytes()                         // [195, 169] (UTF-8 encoding)
```

---

## Regex

Regular expressions are available as string methods. When built with RE2 (the default on most systems), regex operations are guaranteed O(n) with no risk of catastrophic backtracking. Without RE2, Praia falls back to the C++ standard regex engine.

| Method | Description |
|--------|-------------|
| `.test(pattern)` | Returns `true` if the pattern matches anywhere in the string |
| `.match(pattern)` | Returns a map with `match`, `groups`, and `index` for the first match, or `nil` |
| `.matchAll(pattern)` | Returns an array of match maps for all matches |
| `.replacePattern(pattern, replacement)` | Replaces all matches with the replacement string |

### test

```
"hello123".test("[0-9]+")       // true
"hello".test("[0-9]+")          // false
```

### match

Returns a map with the full match, capture groups, and position — or `nil` if no match:

```
let m = "age: 25".match("(\\w+): (\\d+)")
print(m.match)      // age: 25
print(m.groups)     // ["age", "25"]
print(m.index)      // 0

"hello".match("\\d+")   // nil
```

### matchAll

Returns an array of match maps:

```
let nums = "abc123def456".matchAll("\\d+")
for (m in nums) {
    print(m.match, "at", m.index)
}
// 123 at 3
// 456 at 9
```

### replacePattern

Replaces all regex matches. Supports back-references (`$1`, `$2`):

```
"hello   world".replacePattern("\\s+", " ")         // "hello world"
"John Smith".replacePattern("(\\w+) (\\w+)", "$2, $1")  // "Smith, John"
```

Use `.replace()` for literal string replacement, `.replacePattern()` for regex.

### Error handling

Invalid regex patterns throw a catchable error:

```
try {
    "test".test("[invalid")
} catch (err) {
    print(err)      // Invalid regex: ...
}
```

### Practical examples

```
// Email validation
let email = "ada@example.com"
if (email.test("^[\\w.+-]+@[\\w-]+\\.[\\w.]+$")) {
    print("valid email")
}

// Extract all words
let words = "Hello, World! 123".matchAll("[a-zA-Z]+")
for (w in words) { print(w.match) }

// Clean up whitespace
let clean = "  too   many   spaces  ".strip().replacePattern("\\s+", " ")
print(clean)    // "too many spaces"
```

---

## re Grain (Advanced Regex)

The `re` grain provides named capture groups, regex split, and escape — features not available on the built-in string methods.

```
use "re"
```

### Functions

| Function | Description |
|----------|-------------|
| `re.test(str, pattern)` | Returns `true` if pattern matches anywhere |
| `re.find(str, pattern)` | First match with `groups`, `named`, and `index` (or `nil`) |
| `re.findAll(str, pattern)` | Array of all matches, each with `groups` and `named` |
| `re.replace(str, pattern, repl)` | Replace all matches (`$1`, `$2` back-references work) |
| `re.split(str, pattern)` | Split string by regex pattern |
| `re.escape(str)` | Escape special regex characters for literal matching |

### Named groups

Use `(?<name>...)` syntax. Named groups appear in `m.named` as a map:

```
let m = re.find("2026-04-22", "(?<year>\\d{4})-(?<month>\\d{2})-(?<day>\\d{2})")
print(m.named.year)    // "2026"
print(m.named.month)   // "04"
print(m.named.day)     // "22"
print(m.groups)         // ["2026", "04", "22"]
```

Named and unnamed groups can be mixed. Unnamed groups get `nil` in the name list and don't appear in `named`.

### matchAll with named groups

```
let text = "name=Alice age=30 city=London"
let matches = re.findAll(text, "(?<key>\\w+)=(?<val>\\w+)")
for (m in matches) {
    print(m.named.key, "=>", m.named.val)
}
// name => Alice
// age => 30
// city => London
```

### split

```
re.split("one,,two,,,three", ",+")       // ["one", "two", "three"]
re.split("hello world  foo", "\\s+")     // ["hello", "world", "foo"]
re.split("a1b2c3", "\\d")                // ["a", "b", "c", ""]
```

### escape

Escape user input for safe inclusion in a regex pattern:

```
let literal = re.escape("file (1).txt")
print(literal)                             // "file \\(1\\)\\.txt"
re.test("file (1).txt", literal)          // true
```

### Practical: parsing log lines

```
use "re"

let log = "2026-04-22 ERROR [auth] Login failed user=admin"
let m = re.find(log, "(?<date>\\S+) (?<level>\\w+) \\[(?<mod>\\w+)\\] (?<msg>.*)")
print(m.named.level)    // "ERROR"

let pairs = re.findAll(m.named.msg, "(?<key>\\w+)=(?<val>\\S+)")
for (p in pairs) {
    print(p.named.key, "=", p.named.val)   // user = admin
}
```

---

## Array Methods

Methods are called with dot notation on array values.

| Method | Description |
|--------|-------------|
| `.push(value)` | Append an element |
| `.pop()` | Remove and return the last element |
| `.contains(value)` | Check if value is in the array |
| `.join(separator)` | Join elements into a string |
| `.reverse()` | Reverse the array in place |
| `.shift()` | Remove and return the first element |
| `.unshift(val)` | Add element to the beginning |
| `.slice(start, end?)` | Extract subarray (negative indices supported) |
| `.indexOf(val)` | Find index of element (-1 if not found) |
| `.find(fn)` | First element where fn returns truthy (nil if not found) |
| `.lastIndexOf(val)` | Find last index of element (-1 if not found) |
| `.sort(comparator?)` | Return sorted copy. Optional: `lam{ a, b in a - b }` |

```
let arr = [1, 2, 3]
arr.push(4)                     // [1, 2, 3, 4]
arr.pop()                       // returns 4
arr.contains(2)                 // true
["a", "b", "c"].join(", ")     // "a, b, c"
arr.reverse()                   // [3, 2, 1]
```

---

## The sys Namespace

`sys` is a built-in map providing OS-level operations.

### File I/O

Filesystem operations live on the `fs` namespace. (They used to live on `sys`; the old `sys.read` / `sys.write` / etc. still work but print a one-shot deprecation warning to stderr. Rename to `fs.*` at your convenience — the aliases will be removed at 1.0.)

```
// Write a file
fs.write("output.txt", "hello from praia")

// Read a file
let content = fs.read("output.txt")
print(content)          // hello from praia

// Append to a file
fs.append("output.txt", "\nmore text")
```

### File System

```
fs.mkdir("my/nested/dir")          // creates all parent dirs
print(fs.exists("output.txt"))     // true
fs.remove("output.txt")            // delete a file
fs.remove("my/nested/dir")         // delete a directory (recursive)
fs.copy("src.txt", "dst.txt")      // copy a file
fs.copy("srcdir", "dstdir")        // copy a directory (recursive)
fs.move("old.txt", "new.txt")      // move / rename a file or directory
let files = fs.readDir("my/dir")   // returns array of filenames in directory
let tmp = fs.tempDir("myapp")      // race-free mkdtemp under the system tmp
```

### Running Commands

`sys.exec(cmd, timeout?)` (also available as `sys.run(cmd, timeout?)`) runs a command and returns a map with `stdout`, `stderr`, and `exitCode`. Optional timeout in milliseconds — if the process exceeds the timeout, it's killed and `timedOut: true` is added to the result. Pass a string for shell execution, or an array for safe argument passing (no shell injection):

```
// String form — runs through /bin/sh (supports pipes, redirects, etc.)
let r = sys.exec("ls -la")
print(r.stdout)
print(r.exitCode)          // 0

// Array form — runs directly, no shell (safe from injection)
let r2 = sys.exec(["git", "clone", "--quiet", url, dest])
print(r2.exitCode)

// Check for errors
let r3 = sys.exec("cat nonexistent.txt")
if (r3.exitCode != 0) {
    print("Error:", r3.stderr)
}
```

### Process Spawning

`sys.spawn(cmd)` launches a child process with piped stdin/stdout/stderr, returning a process handle for interactive communication. Pass a string for shell execution or an array for safe argument passing (no shell, no injection):

```
// String form — runs via /bin/sh -c
let proc = sys.spawn("cat -n")
proc.write("hello\n")
proc.write("world\n")
proc.closeStdin()         // signal EOF to child
print(proc.read())        // read all stdout
print("exit:", proc.wait())

// Array form — runs via execvp; each element is an exact argv entry
let proc2 = sys.spawn(["ffmpeg", "-i", userPath, "-f", "mp3", "out.mp3"])
```

#### Process handle methods

| Method | Description |
|--------|-------------|
| `proc.write(data)` | Write a string to the child's stdin |
| `proc.closeStdin()` | Close stdin (signals EOF to the child) |
| `proc.read()` | Read all of stdout (blocks until EOF) |
| `proc.readErr()` | Read all of stderr (blocks until EOF) |
| `proc.readLine()` | Read one line from stdout (returns nil on EOF) |
| `proc.wait()` | Wait for exit, returns exit code |
| `proc.kill(signal?)` | Send a signal (default SIGTERM). Accepts name or number |
| `proc.pid` | The child's process ID |

#### Line-by-line reading

```
let proc = sys.spawn("grep -i error")
proc.write("INFO all good\n")
proc.write("ERROR disk full\n")
proc.write("ERROR timeout\n")
proc.closeStdin()

let line = proc.readLine()
while (line != nil) {
    print("match:", line)
    line = proc.readLine()
}
proc.wait()
// match: ERROR disk full
// match: ERROR timeout
```

Use `sys.exec` for fire-and-forget commands. Use `sys.spawn` when you need to pipe input, read output line-by-line, or interact with a long-running process.

### Command-Line Arguments

Arguments passed after the script name are available in `sys.args`:

```sh
./praia script.praia hello world
```

```
// Inside script.praia:
print(sys.args)         // ["hello", "world"]
for (arg in sys.args) {
    print(arg)
}
```

### Exiting

```
sys.exit(0)             // exit with code 0
sys.exit(1)             // exit with code 1
```

### Reading from stdin

`sys.input(prompt?)` reads one line from standard input and returns it as a
string. The optional prompt is printed without a trailing newline before
reading. Returns `nil` on EOF (Ctrl-D or a closed pipe).

```
let name = sys.input("What's your name? ")
if (name == nil) { sys.exit(0) }        // user pressed Ctrl-D

let ans = sys.input("Continue? [y/N] ")
if (ans != nil && ans.lower() == "y") {
    print("Onwards.")
}
```

The line is returned without its trailing newline. Use `.strip()` if you
want to also drop leading/trailing whitespace.

### Signal handling

Register callbacks for OS signals. The signal handler sets a flag; callbacks run when you call `sys.checkSignals()`.

| Function | Description |
|----------|-------------|
| `sys.onSignal(name, fn)` | Register a handler for a signal. Pass `nil` to restore default. |
| `sys.checkSignals()` | Process pending signals by calling registered handlers. Returns `true` if any fired. |
| `sys.signal(name)` | Send a signal to the current process (for testing). |

Supported signals: `SIGINT`, `SIGTERM`, `SIGHUP`, `SIGUSR1`, `SIGUSR2`. Short names (`INT`, `TERM`, etc.) also work.

```
let running = true
sys.onSignal("SIGINT", lam{ sig in
    print("shutting down...")
    running = false
})

while (running) {
    // ... do work ...
    sys.checkSignals()
}
print("cleanup done")
```

Call `sys.checkSignals()` in long-running loops so signal callbacks get a chance to run. Signals of the same type coalesce (matching POSIX semantics) — if SIGINT arrives twice before `checkSignals()`, the handler fires once.

To remove a handler and restore default behavior:

```
sys.onSignal("SIGINT", nil)
```

---

## Terminal I/O

Praia supports raw terminal mode for building TUI applications.

### Raw mode

```
sys.rawMode(true)      // disable line buffering, echo
// ... read keypresses ...
sys.rawMode(false)     // restore normal terminal
```

### Reading keys

```
sys.rawMode(true)
let key = sys.readKey()     // blocks until a key is pressed
sys.rawMode(false)
print("You pressed:", key)
```

Arrow keys return escape sequences: `"\x1b[A"` (up), `"\x1b[B"` (down), `"\x1b[C"` (right), `"\x1b[D"` (left).

### Terminal size

```
let size = sys.termSize()
print(size.rows, size.cols)     // e.g. 24 80
```

### ANSI escape codes

Use `\x1b` (hex escape) to send ANSI codes:

```
let ESC = "\x1b"
print(ESC + "[2J")           // clear screen
print(ESC + "[10;20H")       // move cursor to row 10, col 20
print(ESC + "[31mRed" + ESC + "[0m")    // colored text
print(ESC + "[?25l")         // hide cursor
print(ESC + "[?25h")         // show cursor
```

---

## Pipe Operator

The pipe operator `|>` passes the left side as the first argument to the right side. It turns nested calls into readable top-to-bottom chains.

### Basic usage

```
// Without pipe: nested, reads inside-out
print(sort(filter(nums, lam{ it > 5 })))

// With pipe: linear, reads top-to-bottom
nums
    |> filter(lam{ it > 5 })
    |> sort
    |> print
```

`a |> f` becomes `f(a)`. `a |> f(x)` becomes `f(a, x)` — the left side is prepended as the first argument.

### Chaining

```
let result = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    |> filter(lam{ it % 2 == 0 })
    |> map(lam{ it * it })
    |> sort
print(result)   // [4, 16, 36, 64, 100]
```

### With any function

```
func double(x) { return x * 2 }
func add10(x) { return x + 10 }

let val = 5 |> double |> add10 |> double
print(val)      // 40
```

### Implicit `it` parameter

Lambdas without an `in` keyword automatically get a single parameter named `it`:

```
[1, 2, 3] |> filter(lam{ it > 1 }) |> map(lam{ it * 2 })
// equivalent to:
[1, 2, 3] |> filter(lam{ x in x > 1 }) |> map(lam{ x in x * 2 })
```

This works anywhere, not just in pipes:

```
let double = lam{ it * 2 }
print(double(5))    // 10
```

Use `lam{ in ... }` (explicit empty `in`) for zero-parameter lambdas.

### Error pipeline (`|?>`)

The `|?>` operator catches errors in pipe chains. If the left side throws, the error is passed to the handler on the right. If no error, the value passes through.

```
// Catch parse errors with a fallback
let data = inputStr |> json.parse |?> lam{ nil }

// Chain with error recovery
let result = rawInput
    |> validate
    |> transform
    |> save
    |?> lam{ err in print("failed: " + err); defaultValue }
```

`|?>` is equivalent to wrapping the left side in `try/catch`:

```
// These are equivalent:
let r1 = expr |?> handler
let r2 = nil
try { r2 = expr } catch (e) { r2 = handler(e) }
```

### Data processing pipeline

```
let adults = fs.read("users.json")
    |> json.parse
    |> filter(lam{ it.age >= 18 })
    |> map(lam{ it.name })
    |> sort
```

### Functional built-ins

These functions are designed to work with `|>`:

| Function | Description |
|----------|-------------|
| `filter(arr, predicate)` | Keep elements where predicate returns truthy |
| `map(arr, transform)` | Transform each element |
| `flatMap(arr, fn)` | Map then flatten — if fn returns an array, its elements are spread into the result |
| `reduce(arr, fn, initial?)` | Fold array to a single value. fn receives `(accumulator, element)` |
| `each(arr, fn)` | Call fn on each element (side effects), returns the array |
| `sort(arr, comparator?)` | Return sorted copy. Optional comparator: `lam{ a, b in a.field - b.field }` |
| `any(arr, predicate)` | Returns `true` if any element matches |
| `all(arr, predicate)` | Returns `true` if all elements match |
| `unique(arr)` | Remove duplicates, preserving order |
| `zip(a, b)` | Combine two arrays into `[[a0, b0], [a1, b1], ...]` (truncates to shorter) |
| `enumerate(arr)` | Returns `[[0, elem0], [1, elem1], ...]` |
| `groupBy(arr, fn)` | Group elements by key function, returns map of key → array |
| `findIndex(arr, fn)` | Index of first matching element, or -1 |
| `flatten(arr)` | Flatten one level of nested arrays |
| `keys(map)` | Return array of map keys |
| `values(map)` | Return array of map values |

```
// reduce
[1, 2, 3, 4] |> reduce(lam{ acc, x in acc + x }, 0)  // 10

// sort by field
users |> sort(lam{ a, b in a.age - b.age })

// groupBy
[{t: "a", v: 1}, {t: "b", v: 2}, {t: "a", v: 3}]
    |> groupBy(lam{ it.t })  // {a: [...], b: [...]}

// flatMap
[[1, 2], [3, 4]] |> flatMap(lam{ it })  // [1, 2, 3, 4]

// enumerate with pipe
["x", "y"] |> enumerate |> each(lam{ p in print("%{p[0]}: %{p[1]}") })
```

---

## JSON

The `json` namespace provides fast built-in JSON parsing and serialization.

### json.parse(string)

Converts a JSON string into Praia values:

| JSON | Praia |
|------|-------|
| `{}` | map |
| `[]` | array |
| `"string"` | string |
| `123` | number |
| `true/false` | bool |
| `null` | nil |

```
let data = json.parse("{\"name\": \"Ada\", \"age\": 36}")
print(data.name)        // Ada
print(data.age)         // 36

let list = json.parse("[1, 2, 3]")
print(list)             // [1, 2, 3]
```

### json.stringify(value, indent?)

Converts a Praia value to a JSON string. Optional `indent` for pretty-printing.

```
let obj = {name: "Ada", scores: [100, 95]}

json.stringify(obj)         // {"name":"Ada","scores":[100,95]}
json.stringify(obj, 2)      // pretty-printed with 2-space indent
```

### Round-tripping

```
let original = {users: [{name: "Alice"}, {name: "Bob"}]}
let str = json.stringify(original)
let restored = json.parse(str)
print(restored.users[0].name)   // Alice
```

---

## YAML

The `yaml` namespace provides built-in YAML parsing and serialization.

### yaml.parse(string)

Parses a YAML string into Praia values. Supports mappings, sequences, nested structures, comments, flow sequences, and quoted strings.

```
let config = yaml.parse("host: localhost\nport: 8080\ndebug: true")
print(config.host)      // localhost
print(config.port)      // 8080
print(config.debug)     // true
```

Nested:

```
let yaml_str = "database:\n  host: localhost\n  port: 5432"
let conf = yaml.parse(yaml_str)
print(conf.database.host)   // localhost
```

Sequences:

```
let list = yaml.parse("- apple\n- banana\n- cherry")
print(list)                 // ["apple", "banana", "cherry"]
```

Flow sequences:

```
let data = yaml.parse("tags: [web, api, fast]")
print(data.tags)            // ["web", "api", "fast"]
```

Comments are stripped:

```
let data = yaml.parse("name: Ada  # the inventor")
print(data.name)            // Ada
```

### yaml.stringify(value)

Converts a Praia value to a YAML string.

```
let obj = {name: "Praia", features: ["fast", "simple"]}
print(yaml.stringify(obj))
// name: Praia
// features:
//   - fast
//   - simple
```

### Practical: reading config files

```
let config = yaml.parse(fs.read("config.yaml"))
print("Listening on port %{config.server.port}")
```

---

## Base64

| Function | Description |
|----------|-------------|
| `base64.encode(str)` | Encode string to standard base64 |
| `base64.decode(str)` | Decode standard base64 to string |
| `base64.encodeURL(str)` | URL-safe base64 (RFC 4648 §5): `-_` instead of `+/`, no padding |
| `base64.decodeURL(str)` | Decode URL-safe base64 |

```
base64.encode("hello")         // "aGVsbG8="
base64.decode("aGVsbG8=")     // "hello"

// URL-safe variant — safe for URLs, filenames, JWT tokens
base64.encodeURL("hello")     // "aGVsbG8"  (no padding)
base64.decodeURL("aGVsbG8")   // "hello"
```

---

## Path

Path manipulation using `<filesystem>`. All functions work with strings.

| Function | Description |
|----------|-------------|
| `path.join(parts...)` | Join path segments |
| `path.dirname(p)` | Parent directory |
| `path.basename(p)` | Filename component |
| `path.ext(p)` | File extension (including dot) |
| `path.resolve(p)` | Absolute path |
| `path.isFile(p)` | Returns `true` if path is a regular file |
| `path.isDir(p)` | Returns `true` if path is a directory |
| `path.size(p)` | File size in bytes |
| `path.mtime(p)` | Last modification time (seconds since the Unix epoch) |
| `path.glob(dir, pattern)` | Match files by pattern (e.g. `"*.praia"`) |

```
path.join("src", "main.cpp")       // "src/main.cpp"
path.dirname("/usr/local/bin/p")   // "/usr/local/bin"
path.basename("/usr/local/bin/p")  // "p"
path.ext("script.praia")          // ".praia"
path.resolve("src")               // "/full/path/to/src"
```

---

## URL

| Function | Description |
|----------|-------------|
| `url.parse(str)` | Parse a URL into RFC 3986 components |

`url.parse` returns a map with seven fields: `scheme`, `userinfo`,
`host`, `port`, `path`, `query`, `fragment`. The scheme is lowercased.
`port` is `nil` when the URL omits a port (so it doesn't collide with
an explicit `0`). IPv6 literals are accepted in the bracketed form
`[..]` and `host` returns the address unbracketed.

```
let u = url.parse("https://user:pass@example.com:8080/api?key=val#section")
print(u.scheme)    // "https"
print(u.userinfo)  // "user:pass"
print(u.host)      // "example.com"
print(u.port)      // 8080
print(u.path)      // "/api"
print(u.query)     // "key=val"
print(u.fragment)  // "section"

let v = url.parse("http://[::1]:8080/")
print(v.host)      // "::1"     (brackets stripped)
print(v.port)      // 8080
```

Malformed input throws — non-numeric ports, unterminated `[..]`
literals, bare IPv6 without brackets (ambiguous), or CR/LF/NUL
anywhere in the input.

---

## Concurrency

### How the HTTP server works

The HTTP server is **single-threaded** — handlers run one at a time, serially. The next request is not accepted until the current handler returns. This means there are no race conditions by default.

You can still use `async` inside a handler for fire-and-forget background work or to parallelise I/O — just remember the cross-task isolation rules: each async task gets a deep copy of globals/upvalues/args, so Praia maps are NOT shared. To communicate, use `Queue`, `await`, or external resources (sqlite, files).

### Lock

`Lock()` is a mutex for serializing concurrent access to **external resources** — files, sqlite handles, sockets, native plugin state. It does not, and cannot, make a Praia value shared across tasks; for that, use a Queue.

```
let lock = Lock()
let db = sqlite.open("counts.db")

func increment() {
    lock.withLock(lam{ in
        let row = db.query("SELECT n FROM c WHERE id = 1")
        db.run("UPDATE c SET n = ? WHERE id = 1", [row[0].n + 1])
    })
}

let f1 = async increment()
let f2 = async increment()
await f1
await f2
// db is shared; the lock prevents the two reads/writes from racing
```

| Method | Description |
|--------|-------------|
| `lock.acquire()` | Acquire the lock (blocks if held by another thread) |
| `lock.release()` | Release the lock |
| `lock.withLock(fn)` | Acquire, call fn, release — even if fn throws. Returns fn's return value. |

**Always prefer `withLock`** — it's impossible to forget to release, and it handles errors correctly.

The lock is re-entrant (recursive) — the same thread can acquire it multiple times without deadlocking.

#### What happens if `fn` throws

`withLock` releases the mutex on throw, but does **not** roll back any state `fn` mutated before throwing. For compound state that needs to be all-or-nothing on failure, build the new state locally inside `fn` and only commit it on the last line, after everything that could throw:

```praia
lock.withLock(lam{ in
    let staged = computeNextState()   // may throw — nothing committed yet
    db.run("UPDATE ...", [staged])    // commit
})
```

---

## Async / Await

`async` runs a function call in a background thread and returns a **future**. `await` blocks until the future has a result. Both native and Praia functions run in true parallel.

### Basic usage

```
func compute(n) {
    let sum = 0
    for (i in 0..n) { sum += i }
    return sum
}

let f1 = async compute(10000)
let f2 = async compute(20000)
let f3 = async compute(30000)

print(await f1, await f2, await f3)
```

### Parallel shell commands

```
let f1 = async sys.exec("sleep 1 && echo done1")
let f2 = async sys.exec("sleep 1 && echo done2")
let f3 = async sys.exec("sleep 1 && echo done3")

print(await f1, await f2, await f3)
// Total time: ~1 second (not 3)
```

### futures.all and futures.race

```
// Wait for all futures, returns an array of results
let fs = map([1,2,3,4,5], lam{ n in async compute(n) })
let results = futures.all(fs)

// Wait for the first future to finish
let winner = futures.race([async slowTask(), async fastTask()])
```

| Function | Description |
|----------|-------------|
| `futures.all(arr)` | Await all futures in the array, return results as an array |
| `futures.race(arr)` | Return the result of the first future to complete |

### Error handling

If an async task throws an error, `await` re-throws it:

```
let f = async http.get("http://invalid-host")
try {
    let r = await f
} catch (err) {
    print("request failed:", err)
}
```

### How it works

- `async funcCall(args)` evaluates the function and arguments on the current thread, then spawns the actual call in a new OS thread
- Returns a **future** value immediately
- `await future` blocks until the background thread finishes
- Each async Praia function gets its own VM with a **deep copy** of globals, captured upvalues, and arguments. Tasks are fully isolated — no shared mutable state, no data races
- Native functions (http.get, sys.exec, etc.) also run in true parallel
- A bare `async fn()` (no `let f =`) is fire-and-forget — it returns immediately and the task runs to completion in the background

### Sharing state across async tasks

Because globals/upvalues/arguments are deep-copied, **you cannot share a Praia map, array, or class instance between an async task and its caller** — each side has its own independent copy. This is the price of "no data races" in user code.

```
let m = {}
func writer(k, v) { m[k] = v }
let f = async writer("a", 1)
await f
print(m)              // {} — the task mutated its own copy
```

To communicate, use:

- **`SharedMap`** — cross-task key-value store. Use this when async tasks need shared state by key (progress trackers, job state, caches).
- **`Queue`** — built for cross-task messaging; the queue itself isn't deep-copied.
- **`CancellationToken`** — cooperative cancel signal. The caller flips the flag; long-running tasks poll and bail.
- **External resources** — files, SQLite, sockets, native plugin state. These live outside the Praia heap, so all tasks see the same underlying resource. Use `Lock()` to coordinate concurrent access.
- **`await`** — collect results back from the task. The future's return value is moved across the boundary.

### SharedMap

`SharedMap()` is a thread-safe key-value store that survives the `async` deep-copy. Unlike a regular `{}` map, mutations from inside an async task are visible to the caller.

```
let jobs = SharedMap()
jobs.set("abc", {progress: 0})
let f = async lam{ in
    jobs.update("abc", lam{ s in s.progress = 50; return s })
}()
await f
print(jobs.get("abc"))     // {progress: 50}
```

| Method | Description |
|--------|-------------|
| `m.set(k, v)` | Insert or replace. |
| `m.get(k)` / `m.get(k, default)` | Read with optional default. |
| `m.has(k)` | Presence check. |
| `m.delete(k)` | Remove; returns true if was present. |
| `m.update(k, fn)` | Atomic read-modify-write. `fn(current)` returns the new value. Lock held during `fn`. |
| `m.keys()` / `m.values()` | Snapshot arrays. |
| `m.size()` / `m.clear()` | Count / empty. |

Use `update` for compound mutations (increments, conditional writes); the lock guarantees no other task observes a half-update. Don't do I/O inside `update`. The lock is recursive — `fn` can call other `SharedMap` methods on the same map without deadlocking.

#### What happens if `fn` throws

The slot itself is all-or-nothing: if `fn` throws, the SharedMap entry for `k` is **not** overwritten and the throw propagates out of `update`.

But the value `fn` receives is the *same map/array/instance* the SharedMap is holding (Praia maps and arrays are reference types). If `fn` mutates that value in place before throwing, those partial mutations stick — the original ends up half-modified even though the slot wasn't reassigned:

```praia
m.set("k", {a: 1, b: 2})
try {
    m.update("k", lam{ s in
        s.a = 99       // mutates the shared map directly
        throw "oops"   // slot write is skipped, but s.a = 99 already happened
        return s
    })
} catch (e) {}
print(m.get("k"))   // {a: 99, b: 2} — half-modified
```

If you need transactional semantics, build a fresh value inside `fn` and only return it on success:

```praia
m.update("k", lam{ s in
    let next = {a: s.a, b: s.b}    // snapshot
    next.a = 99
    if (somethingBad) { throw "oops" }
    return next                    // only stored on success
})
```

### CancellationToken

`CancellationToken()` is a flag that any task can flip to "cancelled". Pass it to long-running async tasks and have them poll `cancelled()` to bail out cleanly. This is the cooperative alternative to killing a task by signal — the task gets to clean up its own subprocess.

```
let token = CancellationToken()

func work(tok) {
    let proc = sys.spawn(["ffmpeg", "-i", "in.mp4", "out.webm"])
    while (true) {
        if (tok.cancelled()) { proc.kill(); return "cancelled" }
        let line = proc.readLine()
        if (line == nil) { break }
    }
    proc.wait()
    return "done"
}

let f = async work(token)
// ... later, from any thread:
token.cancel()
print(await f)        // "cancelled"
```

| Method | Description |
|--------|-------------|
| `token.cancel()` | Set the cancelled flag. Idempotent. |
| `token.cancelled()` | Returns `true` if `cancel()` has been called. |
| `token.throwIfCancelled()` | Throws `"cancelled"` if cancelled, otherwise returns nil. |

The flag is `std::atomic<bool>` — both methods are lock-free. Cancellation is a one-way transition. Make a fresh `CancellationToken()` per logical operation.

### Queues

Queues are thread-safe FIFO queues for communication between async tasks.

```
let q = Queue()      // unbounded queue — send() never blocks (until closed)
let q = Queue(10)    // bounded queue — send() blocks while 10 items are pending
```

> **Renamed in this release.** Previously `Channel()`. The name `Channel`
> still works as a deprecated alias and prints a one-line warning to
> stderr on first use; rename to `Queue` to silence it. Method names
> (`send` / `recv` / `tryRecv` / `close` / `isClosed` / `isEmpty` /
> `closed`) are unchanged.

| Method | Description |
|--------|-------------|
| `q.send(val)` | Send a value. Blocks when bounded and full; never blocks when unbounded. Throws if the queue is closed. |
| `q.recv()` | Receive a value. Blocks until one is available; returns nil once the queue is closed AND drained. |
| `q.tryRecv()` | Non-blocking receive. Returns nil immediately if no value is pending. |
| `q.close()` | Close the queue — no more sends allowed. |
| `q.isClosed()` | True once `close()` has been called. The "can I still send?" check for producers. |
| `q.isEmpty()` | True when the buffer has no pending values. The "is there anything to read?" check for consumers. |
| `q.closed()` | True when **closed AND drained** (= `isClosed() && isEmpty()`). The "is this queue done forever?" check for shutdown logic. |

The three flags answer different questions. A producer wanting to bail out cleanly should consult `isClosed()` — `closed()` stays false until the buffer drains, so a producer checking it can race past a `close()` call and only see it once their own buffered values have already been consumed.

> **Not a rendezvous channel.** `Queue()` is a queue, not Go-style hand-to-hand
> coordination. Senders don't wait for receivers — values pile up in the buffer
> until consumed. Use `Queue(N)` if you want backpressure.

#### Producer-consumer pattern

```
let q = Queue()

func producer(q) {
    for (i in 0..5) {
        q.send(i)
    }
    q.close()
}

async producer(q)

while (true) {
    let val = q.recv()
    if (val == nil) { break }
    print(val)
}
```

#### Fan-out: multiple workers

```
let results = Queue()

func scan(target, results) {
    let r = sys.exec("ping -c1 -W1 " + target)
    if (r.exitCode == 0) {
        results.send(target + " is up")
    } else {
        results.send(target + " is down")
    }
}

let targets = ["10.0.0.1", "10.0.0.2", "10.0.0.3"]
for (t in targets) {
    async scan(t, results)
}

for (i in 0..len(targets)) {
    print(results.recv())
}
```

---

## HTTP Networking

The `http` namespace provides an HTTP/HTTPS client and server. HTTPS requires OpenSSL (auto-detected at build time).

### HTTP Client

#### GET request

```
let res = http.get("http://example.com/api")
print(res.status)       // 200
print(res.body)         // response body string
print(res.headers)      // map of lowercase header names
```

#### POST request

```
// Simple string body
let res = http.post("http://example.com/api", "hello")

// With headers
let res = http.post("http://example.com/api", {
    body: "{\"name\": \"Ada\"}",
    headers: {"Content-Type": "application/json"}
})
```

#### General request

```
let res = http.request({
    method: "PUT",
    url: "http://example.com/api/1",
    body: "updated data",
    headers: {"Content-Type": "text/plain"}
})
```

#### Response format

All client methods return a map:

```
{
    status: 200,
    body: "...",
    headers: {"content-type": "text/html", ...}
}
```

Header names are lowercased for consistent access.

### HTTP Server

#### Creating a server

Pass a handler function to `http.createServer`. The handler receives a request map and returns a response map:

```
let server = http.createServer(lam{ req in
    if (req.path == "/") {
        return {
            status: 200,
            body: "<h1>Hello!</h1>",
            headers: {"Content-Type": "text/html"}
        }
    }
    return {status: 404, body: "Not Found"}
})

server.listen(8080)     // blocks, prints "Server listening on port 8080"
```

#### Request object

The handler receives a map with:

| Field | Description |
|-------|-------------|
| `method` | `"GET"`, `"POST"`, etc. |
| `path` | URL path (e.g. `"/hello"`) |
| `query` | Parsed query parameters as a map (e.g. `{name: "Ada", age: "36"}`) |
| `headers` | Map of lowercase header names |
| `body` | Request body string |

#### Response format

Return a map with:

| Field | Default | Description |
|-------|---------|-------------|
| `status` | `200` | HTTP status code |
| `body` | `""` | Response body |
| `headers` | `{"Content-Type": "text/plain"}` | Response headers |

You can also return a plain string — it becomes a 200 text/plain response.

#### Example: JSON API

```
let server = http.createServer(lam{ req in
    if (req.method == "GET" && req.path == "/api/time") {
        return {
            status: 200,
            body: "{\"time\": \"%{sys.exec("date").stdout}\"}",
            headers: {"Content-Type": "application/json"}
        }
    }

    if (req.method == "POST" && req.path == "/api/echo") {
        return {
            status: 200,
            body: req.body,
            headers: {"Content-Type": req.headers["content-type"]}
        }
    }

    return {status: 404, body: "Not Found"}
})

server.listen(3000)
```

#### Error handling

If the handler throws an error, the server returns a 500 response and continues running.

#### Request body size

The server has no built-in cap on request body size — it reads up to `Content-Length` bytes into `req.body`. This matches the philosophy of `net/http`, Flask, and Express: the stdlib stays out of policy. If you need a limit, opt in:

- **Application-level** — use `middleware.bodyLimit(n)` from the middleware grain to reject requests with `Content-Length` over `n` bytes (returns 413).
- **DoS-grade** — put a reverse proxy (nginx, caddy) in front. A determined attacker can lie about `Content-Length` or stream forever; only the proxy can short-circuit before the body is buffered.

Headers are capped at 64 KB regardless.

#### Graceful shutdown

The server handles `SIGINT` (Ctrl-C) and `SIGTERM` (container stop) gracefully — it finishes the current request, closes the socket, and returns from `listen()`. Code after `listen()` runs normally:

```
server.listen(8080)
// This runs after Ctrl-C or SIGTERM:
print("Shutting down...")
db.close()
```

#### Server-Sent Events (SSE)

`http.sse(req, callback)` keeps the connection open for real-time streaming. The callback receives a `send` function.

```
server.get("/events", lam{ req, params in
    return http.sse(req, lam{ send in
        for (i in 0..10) {
            send(json.stringify({count: i}), "update")   // send(data, event?)
            time.sleep(1000)
        }
        send("done", "close")
    })
})
```

The client receives standard SSE format:

```
event: update
data: {"count":0}

event: update
data: {"count":1}
```

`send(data)` sends a plain `data:` message. `send(data, eventName)` adds an `event:` field. The connection closes when the callback returns.

**Browser client:**

```javascript
const events = new EventSource('/events');
events.addEventListener('update', e => {
    console.log(JSON.parse(e.data));
});
```

#### Query parameters

Query strings are automatically parsed into a map. Values are URL-decoded.

```
// Request: GET /search?q=hello+world&page=2

server.get("/search", lam{ req, params in
    print(req.query.q)      // "hello world"
    print(req.query.page)   // "2"
})
```

### Response Helpers

Instead of manually building response maps, use these helpers:

| Helper | Description |
|--------|-------------|
| `http.json(obj, status?)` | JSON response with `application/json` |
| `http.text(str, status?)` | Plain text response |
| `http.html(str, status?)` | HTML response with `charset=utf-8` |
| `http.redirect(url, status?)` | Redirect (302 by default) |
| `http.file(path, status?, opts?)` | Serve a file with auto-detected MIME type |

```
// Before
return {status: 200, body: json.stringify(data), headers: {"Content-Type": "application/json"}}

// After
return http.json(data)
return http.json({error: "not found"}, 404)
return http.text("hello")
return http.html("<h1>Hi</h1>")
return http.redirect("/login")
return http.redirect("/new-url", 301)    // permanent redirect
return http.file("public/style.css")     // auto-detects text/css
```

`http.file()` detects MIME types for: html, css, js, json, xml, txt, csv, svg, png, jpg, gif, ico, webp, woff, woff2, pdf, zip, mp3, mp4, wasm.

#### Static file serving

```
server.get("/static/:filename", lam{ req, params in
    // Always pair user-controlled paths with withinDir — see below.
    return http.file("public/" + params.filename, {withinDir: "public"})
})
```

#### Path traversal — withinDir

`http.file` and `http.fileStream` will serve whatever path you give them — including `/etc/passwd`. By design: if you're serving a hard-coded asset (`http.file("public/index.html")`), you don't pay any path-resolution overhead. **But if any part of the path comes from a request** (URL params, query strings, headers, form fields), you must constrain where it can resolve to, or an attacker passing `../../etc/passwd` will read whatever the server process can read.

Use the `withinDir` option. It resolves both the path and the dir to their canonical absolute forms (handling `..`, relative components, and symlinks) and throws if the path escapes the dir. Symlinks targeting outside the jail are blocked.

```
server.get("/files/:name", lam{ req, params in
    try {
        return http.file("uploads/" + params.name, {withinDir: "uploads"})
    } catch (e) {
        return http.text("not found", 404)
    }
})
```

### URL Encoding

| Function | Description |
|----------|-------------|
| `http.encodeURI(str)` | Percent-encode a string (RFC 3986) |
| `http.decodeURI(str)` | Decode percent-encoded sequences |

```
http.encodeURI("hello world")       // "hello%20world"
http.encodeURI("a=1&b=2")          // "a%3D1%26b%3D2"
http.decodeURI("hello%20world")    // "hello world"
```

`encodeURI` leaves unreserved characters (`A-Z a-z 0-9 - _ . ~`) as-is and percent-encodes everything else. `decodeURI` reverses `%XX` sequences. Use these when building URLs with user input to prevent injection.

---

## Router

The `router` grain provides Express-style HTTP routing with path parameters.

### Setup

```
use "router"

let server = router.create()

server.get("/", lam{ req, params in
    return {status: 200, body: "Home"}
})

server.listen(8080)
```

### Path parameters

Use `:name` segments to capture parts of the URL. Captured values are passed as the second argument to the handler.

```
server.get("/users/:id", lam{ req, params in
    return {status: 200, body: "User %{params.id}"}
})

server.post("/api/game/:id/guess", lam{ req, params in
    let gameId = params.id
    let guess = json.parse(req.body)
    // ...
})

// Multiple params
server.get("/users/:userId/posts/:postId", lam{ req, params in
    print(params.userId, params.postId)
})
```

### HTTP methods

| Method | Description |
|--------|-------------|
| `server.get(path, handler)` | GET |
| `server.post(path, handler)` | POST |
| `server.put(path, handler)` | PUT |
| `server.delete(path, handler)` | DELETE |
| `server.patch(path, handler)` | PATCH |
| `server.options(path, handler)` | OPTIONS |
| `server.all(path, handler)` | Match any method |

### Custom 404

```
server.notFound(lam{ req, params in
    return {
        status: 404,
        body: json.stringify({error: "Not found", path: req.path}),
        headers: {"Content-Type": "application/json"}
    }
})
```

### Handler signature

All handlers receive two arguments: `(req, params)`.

- `req` — the request map with `method`, `path`, `query`, `headers`, `body`
- `params` — map of captured path parameters (empty `{}` for the 404 handler)

### Using without a server

Use `.handle(req)` to test routing without starting a server:

```
let result = server.handle({method: "GET", path: "/users/42", query: {}, body: ""})
print(result.body)      // "User 42"
```

---

## Middleware

The `middleware` grain provides common middleware functions for the router. Middleware runs on every request, in registration order.

### Using middleware

```
use "router"
use "middleware"

let server = router.create()
server.use(middleware.requestId())
server.use(middleware.cors())
server.use(middleware.jsonBody())
```

### How middleware works

Each middleware is a function that receives `(req, next)`. Call `next(req)` to pass the request to the next middleware (or the route handler). Return a response map to short-circuit.

```
// Custom middleware
server.use(lam{ req, next in
    let start = time.now()
    let res = next(req)      // call next middleware / handler
    let ms = time.now() - start
    print("%{req.method} %{req.path} took %{ms}ms")
    return res
})
```

### Built-in middleware

| Middleware | Description |
|-----------|-------------|
| `middleware.cors()` | Adds CORS headers, handles OPTIONS preflight |
| `middleware.cors({origin: "...", methods: "...", headers: "..."})` | CORS with custom options |
| `middleware.jsonBody()` | Parses JSON request bodies into `req.json` |
| `middleware.requestId()` | Adds unique `req.id` and `X-Request-Id` response header |
| `middleware.auth(verifier)` | Bearer token auth — calls `verifier(token)`, sets `req.user` |
| `middleware.headers(map)` | Adds fixed headers to every response |
| `middleware.bodyLimit(maxBytes)` | Rejects requests with `Content-Length` above `maxBytes` with 413 (app-level only — see HTTP server notes) |

### CORS

```
server.use(middleware.cors())                              // allow all origins
server.use(middleware.cors({origin: "https://myapp.com"})) // specific origin
```

### JSON body parsing

```
server.use(middleware.jsonBody())

server.post("/api/data", lam{ req, params in
    print(req.json.name)    // parsed from {"name": "Ada"}
})
```

Returns 400 automatically if the JSON is malformed.

### Authentication

```
server.use(middleware.auth(lam{ token in
    if (token == "secret123") {
        return {id: 1, name: "Ada"}   // user object
    }
    return nil                         // reject
}))

server.get("/profile", lam{ req, params in
    return {status: 200, body: "Hello %{req.user.name}"}
})
```

Returns 401 if no `Authorization: Bearer ...` header, 403 if the verifier returns nil.

---

## Logger

The `logger` grain provides structured logging with levels.

### Creating a logger

```
use "logger"

let log = logger.create("MyApp")
log.info("server started")
log.warn("disk space low")
log.error("connection failed")
log.debug("verbose output")
```

Output: `[2026-04-19 13:00:00] INFO [MyApp] server started`

### Log levels

| Level | Priority |
|-------|----------|
| `debug` | 0 (lowest) |
| `info` | 1 (default) |
| `warn` | 2 |
| `error` | 3 |
| `none` | 4 (disables all) |

```
log.setLevel("debug")    // show everything
log.setLevel("warn")     // only warn and error
log.setLevel("none")     // silence all
```

### As router middleware

```
use "logger"
use "router"

let log = logger.create("API")
let server = router.create()

server.use(logger.middleware(log))
// Logs: [timestamp] INFO [API] GET /users/42 200 12ms
```

---

## Cookies

The `cookie` grain parses and builds HTTP cookie headers.

### Parsing cookies

```
use "cookie"

// Parse a Cookie header into a map
let cookies = cookie.parse(req.headers["cookie"])
print(cookies.sessionId)
print(cookies.theme)
```

### Building Set-Cookie headers

```
// Simple cookie with secure defaults (HttpOnly, SameSite=Lax, Path=/)
let header = cookie.build("theme", "dark")

// With options
let header = cookie.build("session", token, {
    httpOnly: true,
    secure: true,
    sameSite: "Strict",
    maxAge: 3600,        // 1 hour in seconds
    path: "/",
    domain: "example.com"
})
```

### Clearing cookies

```
let header = cookie.clear("session")   // sets Max-Age=0
```

### Default options

If no options are passed, `cookie.build` uses safe defaults:
- `HttpOnly` — not accessible from JavaScript
- `SameSite=Lax` — prevents CSRF
- `Path=/` — available on all paths

---

## Sessions

The `session` grain provides server-side session management as router middleware.

### Setup

```
use "router"
use "session"

let server = router.create()
let sessions = session.create()
server.use(sessions.middleware())
```

### Using sessions in handlers

```
// Store data
server.post("/login", lam{ req, params in
    req.session.set("user", {name: "Ada", role: "admin"})
    return http.json({message: "logged in"})
})

// Read data
server.get("/profile", lam{ req, params in
    let user = req.session.get("user")
    if (!user) {
        return http.json({error: "not logged in"}, 401)
    }
    return http.json({user: user})
})

// Destroy session (logout)
server.post("/logout", lam{ req, params in
    req.session.destroy()
    return http.json({message: "logged out"})
})
```

### Session API

| Method | Description |
|--------|-------------|
| `req.session.get(key)` | Get a value (returns nil if not set) |
| `req.session.set(key, value)` | Store a value |
| `req.session.has(key)` | Check if key exists |
| `req.session.delete(key)` | Remove a key |
| `req.session.destroy()` | Delete the entire session |
| `req.session.id` | The session ID string |

### Options

```
let sessions = session.create({
    cookieName: "myapp.sid",    // default: "praia.sid"
    maxAge: 7200,               // 2 hours (default: 86400 = 1 day)
    secure: true,               // HTTPS only
    sameSite: "Strict"          // default: "Lax"
})
```

### How it works

1. On each request, the middleware reads the session cookie
2. If valid, loads session data from an in-memory store
3. If missing or invalid, creates a new session
4. Attaches `req.session` with get/set/destroy methods
5. After the handler runs, sets the `Set-Cookie` header on the response

Sessions are stored in memory — they're lost when the server restarts. For persistent sessions, store session data in SQLite.

---

## SQLite

Built-in SQLite database support. `sqlite.open()` returns a database object with `query`, `run`, and `close` methods. Available when built on a system with libsqlite3.

### Opening a database

```
let db = sqlite.open("myapp.db")       // file-based
let db = sqlite.open(":memory:")       // in-memory
```

### Queries

`db.query(sql, params?)` executes a SELECT and returns an array of maps (one map per row):

```
let users = db.query("SELECT * FROM users WHERE age > ?", [18])
for (user in users) {
    print(user.name, user.age)
}
```

### Executing statements

`db.run(sql, params?)` executes INSERT/UPDATE/DELETE and returns `{changes, lastId}`:

```
let result = db.run("INSERT INTO users (name, age) VALUES (?, ?)", ["Ada", 36])
print(result.lastId)      // auto-increment id
print(result.changes)     // rows affected
```

### Parameterized queries

Always use `?` placeholders — they prevent SQL injection:

```
// Safe
db.query("SELECT * FROM users WHERE name = ?", [name])

// Unsafe — never do this
db.query("SELECT * FROM users WHERE name = '" + name + "'")
```

Parameters are bound by type: strings, numbers, bools, and nil are all handled automatically.

### Closing

```
db.close()
```

### Example: REST API with SQLite

```
let db = sqlite.open(":memory:")
db.run("CREATE TABLE todos (id INTEGER PRIMARY KEY, title TEXT, done INT)")

let server = http.createServer(lam{ req in
    if (req.method == "GET" && req.path == "/todos") {
        let todos = db.query("SELECT * FROM todos")
        return {
            status: 200,
            body: json.stringify(todos),
            headers: {"Content-Type": "application/json"}
        }
    }
    if (req.method == "POST" && req.path == "/todos") {
        let todo = json.parse(req.body)
        db.run("INSERT INTO todos (title, done) VALUES (?, ?)", [todo.title, 0])
        return {status: 201, body: json.stringify({ok: true})}
    }
    return {status: 404, body: "Not Found"}
})

server.listen(8080)
```

---

## Math

The `math` namespace provides mathematical constants and functions.

### Constants

| Name | Value |
|------|-------|
| `math.PI` | 3.14159265358979 |
| `math.E` | 2.71828182845905 |
| `math.INF` | Infinity |

### Functions

| Function | Description |
|----------|-------------|
| `math.sqrt(x)` | Square root |
| `math.pow(x, y)` | x raised to power y |
| `math.abs(x)` | Absolute value |
| `math.floor(x)` | Round down |
| `math.ceil(x)` | Round up |
| `math.round(x)` | Round to nearest |
| `math.trunc(x)` | Truncate to integer (toward zero) |
| `math.idiv(a, b)` | Integer division (truncated toward zero) |
| `math.min(a, b)` | Minimum |
| `math.max(a, b)` | Maximum |
| `math.clamp(x, lo, hi)` | Clamp x between lo and hi |
| `math.approx(a, b, epsilon?)` | Approximate equality (default epsilon: 1e-9) |
| `math.sin(x)`, `cos`, `tan` | Trigonometry (radians) |
| `math.asin(x)`, `acos`, `atan` | Inverse trig |
| `math.atan2(y, x)` | Two-argument arctangent |
| `math.log(x)` | Natural log |
| `math.log2(x)`, `log10(x)` | Base-2 and base-10 log |
| `math.exp(x)` | e^x |
| `math.isNan(x)` | Returns `true` if x is NaN (false for non-numbers) |
| `math.isInf(x)` | Returns `true` if x is ±Infinity (false for non-numbers) |

```
print(math.sqrt(144))              // 12
print(math.pow(2, 10))             // 1024
print(math.sin(math.PI / 2))       // 1
print(math.clamp(150, 0, 100))     // 100
```

---

## Random

The `random` namespace provides random number generation using a Mersenne Twister engine.

| Function | Description |
|----------|-------------|
| `random.int(min, max)` | Random integer between min and max (inclusive) |
| `random.float()` | Random float between 0.0 and 1.0 |
| `random.choice(arr)` | Random element from an array |
| `random.shuffle(arr)` | Shuffle an array in place |
| `random.seed(n)` | Set the seed for reproducible results |

```
print(random.int(1, 100))          // e.g. 42
print(random.float())              // e.g. 0.7312
print(random.choice(["a", "b"]))   // "a" or "b"

let deck = [1, 2, 3, 4, 5]
random.shuffle(deck)
print(deck)

// Reproducible
random.seed(42)
print(random.int(0, 100))          // always 51
```

---

## Time

The `time` namespace provides timestamps, formatting, and sleep.

| Function | Description |
|----------|-------------|
| `time.now()` | Current time as Unix milliseconds |
| `time.epoch()` | Current time as Unix seconds |
| `time.sleep(ms)` | Pause execution for ms milliseconds |
| `time.format(fmt?, timestamp?, utc?)` | Format time as string. Pass `true` for UTC |
| `time.parse(str, fmt?, utc?)` | Parse date string to ms timestamp. Pass `true` for UTC |
| `time.year(ts)`, `month`, `day` | Extract date components from ms timestamp |
| `time.hour(ts)`, `minute`, `second` | Extract time components |
| `time.weekday(ts)` | Day of week (0=Sunday, 6=Saturday) |
| `time.addDays(ts, n)` | Add/subtract days, returns new timestamp |
| `time.addHours(ts, n)` | Add/subtract hours |
| `time.addMinutes(ts, n)` | Add/subtract minutes |
| `time.addSeconds(ts, n)` | Add/subtract seconds |
| `time.components(ts, utc?)` | Returns `{year, month, day, hour, minute, second, weekday}`. Pass `true` for UTC |

```
let start = time.now()
time.sleep(100)
print(time.now() - start)          // ~100

print(time.format())               // "2026-04-18 13:00:00"
print(time.format("%H:%M"))        // "13:00"
print(time.epoch())                // 1776510000

// Parsing — auto-detects "YYYY-MM-DD" and "YYYY-MM-DD HH:MM:SS"
let ts = time.parse("2024-06-15")
let ts2 = time.parse("15/06/2024", "%d/%m/%Y")  // custom format
print(time.format("%Y-%m-%d", ts))               // "2024-06-15"
```

### Benchmarking

```
let start = time.now()
// ... code to measure ...
print("took", time.now() - start, "ms")
```

---

## Filesystem (fs)

`fs` is the canonical home for filesystem I/O.

### Content & directory ops

| Function | Description |
|----------|-------------|
| `fs.read(path)` | Read entire file as string |
| `fs.write(path, str)` | Write string to file (creates/truncates) |
| `fs.append(path, str)` | Append string to file |
| `fs.readLines(path)` | Read file as array of lines (no trailing newlines) |
| `fs.exists(path)` | `true` if the path exists |
| `fs.mkdir(path)` | Create directory, including parents |
| `fs.remove(path)` | Delete file or directory (recursive) |
| `fs.readDir(path)` | Array of entry names in a directory |
| `fs.copy(src, dst)` | Copy file or directory (recursive) |
| `fs.move(src, dst)` | Move / rename a file or directory |

### Metadata, permissions, and links

| Function | Description |
|----------|-------------|
| `fs.stat(path)` | Full metadata, follows symlinks. Returns `{type, size, mode, uid, gid, mtime, atime, ctime, nlink, ino, dev}` |
| `fs.lstat(path)` | Same as `stat` but does NOT follow symlinks — use when you need to distinguish links from regular files |
| `fs.chmod(path, mode)` | Set permission bits. `mode` is an int (e.g. `420` for `0o644`, `384` for `0o600`); high bits are masked off |
| `fs.symlink(target, linkpath)` | Create a symlink at `linkpath` pointing to `target`. Target string stored verbatim; dangling links are legal |
| `fs.readlink(path)` | Return the stored target of a symlink. Throws on a non-link |

The `.type` field returned by `stat`/`lstat` is one of `"file"`, `"dir"`, `"symlink"`, `"socket"`, `"fifo"`, `"block"`, `"char"`, or `"unknown"`. Mode is masked to the low 12 bits (permission + setuid/setgid/sticky).

### Atomic and temp-file primitives

| Function | Description |
|----------|-------------|
| `fs.atomicWrite(path, content)` | Write to a sibling temp file, fsync, then `rename(2)` onto `path`. Crash-safe: readers see old-or-new, never a half-written file. Same-filesystem only |
| `fs.tempDir(prefix?)` | Race-free `mkdtemp(3)` under the system temp dir (mode 0700) |
| `fs.mktemp(prefix?)` | Race-free `mkstemp(3)` temp FILE under the system temp dir (mode 0600). Returns the path of an empty file you own |

Use `fs.atomicWrite` for config files, lock files, and anything else where a partial write would corrupt downstream readers. The temp file goes in the target's directory (same filesystem) so the rename is genuinely atomic.

### File handles (streaming I/O)

`fs.open(path, mode)` returns a file handle for streaming reads and writes — preferred over `fs.read`/`fs.write` when the file is large enough that loading it whole would be wasteful, or when you need random access via `seek`.

| Mode | Behavior |
|------|----------|
| `"r"`  | Read only; file must exist |
| `"w"`  | Write only; truncates or creates (mode 0644) |
| `"a"`  | Write only, append; creates if missing |
| `"r+"` | Read + write; file must exist |
| `"w+"` | Read + write; truncates or creates |
| `"a+"` | Read + write; writes always append |

| Method | Description |
|--------|-------------|
| `h.read(n)` | Read up to `n` bytes; returns `""` at EOF |
| `h.readLine()` | Read one line (without trailing `\n`); returns `nil` at EOF |
| `h.write(data)` | Write string/bytes; returns the number of bytes written |
| `h.seek(offset, whence?)` | Reposition. `whence` is `"start"` (default), `"current"`, or `"end"` |
| `h.tell()` | Current logical position (accounts for read-buffered bytes) |
| `h.flush()` | `fsync(2)` — push the kernel page cache to disk |
| `h.close()` | Close the fd. Idempotent. Subsequent reads/writes throw |
| `h.path` | The path the handle was opened on |
| `h.mode` | The mode string the handle was opened with |

Streaming pattern — line-by-line processing of an arbitrarily large file:

```
let h = fs.open("big.log", "r")
defer h.close()
let line = h.readLine()
while (line != nil) {
    process(line)
    line = h.readLine()
}
```

Read and `readLine` can be mixed on the same handle. Writes on `r+`/`w+` handles correctly land at the user's logical position even after intervening reads — the handle resyncs the kernel position before writing.

The old `sys.read` / `sys.write` / etc. names still work but emit a one-shot deprecation warning on first use per process. They'll be removed at 1.0 — rename `sys.<op>` to `fs.<op>` whenever you touch the file.

## OS extras (sys)

`sys` covers process-level concerns: environment, working directory, process metadata, signals, exec, terminal control.

| Field/Function | Description |
|----------------|-------------|
| `sys.env(name)` | Read environment variable (returns nil if not set) |
| `sys.envAll()` | Returns all environment variables as a map |
| `sys.setenv(name, value)` | Set an environment variable |
| `sys.cwd()` | Current working directory |
| `sys.chdir(path)` | Change working directory |
| `sys.uid()` | Effective user ID (`geteuid()`) |
| `sys.isRoot()` | `true` if running as root (uid 0) |
| `sys.getpid()` | Current process ID |
| `sys.platform` | `"darwin"`, `"linux"`, or `"windows"` |
| `sys.stdout(str)` | Write to stdout without a trailing newline |
| `sys.rawMode(bool)` | Enable/disable raw terminal mode (no line buffering) |
| `sys.readKey()` | Read a single keypress (returns string, handles escape sequences) |
| `sys.termSize()` | Returns `{rows, cols}` of the terminal |

```
print(sys.env("HOME"))             // "/Users/ada"
print(sys.cwd())                   // "/path/to/project"
print(sys.platform)                // "darwin"

let dbUrl = sys.env("DATABASE_URL")
ensure (dbUrl) else { throw "DATABASE_URL not set" }
```

---

## Bitwise Operators

| Operator | Description |
|----------|-------------|
| `&` | Bitwise AND |
| `\|` | Bitwise OR |
| `^` | Bitwise XOR |
| `~` | Bitwise NOT (unary) |
| `<<` | Left shift |
| `>>` | Right shift |

```
255 & 15        // 15
240 | 15        // 255
255 ^ 15        // 240
~0              // -1
1 << 8          // 256
256 >> 4        // 16
```

All values are converted to 64-bit integers for bitwise operations.

Note: `|` (single pipe) is bitwise OR. `|>` is the pipe operator. `||` is logical OR. The lexer distinguishes them by what follows the `|`.

---

## Bytes

The `bytes` namespace provides binary data packing and unpacking for working with binary protocols.

### Struct format strings

`bytes.pack` and `bytes.unpack` accept Python-style struct format strings. The format starts with an endianness prefix, followed by type characters with optional repeat counts.

**Endian prefix** (required for struct format):

| Prefix | Byte order |
|--------|------------|
| `>` or `!` | Big-endian (network) |
| `<` or `=` | Little-endian |

**Type characters:**

| Char | Size | Description |
|------|------|-------------|
| `B` | 1 | Unsigned 8-bit |
| `b` | 1 | Signed 8-bit |
| `H` | 2 | Unsigned 16-bit |
| `h` | 2 | Signed 16-bit |
| `I` | 4 | Unsigned 32-bit |
| `i` | 4 | Signed 32-bit |
| `Q` | 8 | Unsigned 64-bit |
| `q` | 8 | Signed 64-bit |
| `f` | 4 | 32-bit float |
| `d` | 8 | 64-bit double |
| `x` | 1 | Pad byte (no value consumed) |

Repeat counts: `3B` means three unsigned bytes, `4x` means four pad bytes.

### bytes.pack(format, values)

```
// Struct format: big-endian u8 + u16 + u32
let data = bytes.pack(">BHI", [255, 1234, 100000])

// Little-endian two u16s
let data2 = bytes.pack("<2H", [1, 256])

// Float and double
let data3 = bytes.pack(">fd", [3.14, 2.718])

// With padding
let header = bytes.pack(">BxxH", [1, 1000])  // 1 byte, 2 pad, 2 bytes
```

### bytes.unpack(format, data)

```
let vals = bytes.unpack(">BHI", data)     // [255, 1234, 100000]
let floats = bytes.unpack(">fd", data3)   // [3.14, 2.718]
```

### bytes.calcsize(format)

Returns the total byte size of a struct format string:

```
bytes.calcsize(">BHI")    // 7
bytes.calcsize(">3B2Hd")  // 15
```

### Practical: DNS query header

```
// Pack: ID, flags, qdcount, ancount, nscount, arcount
let header = bytes.pack(">6H", [4660, 256, 1, 0, 0, 0])
let parsed = bytes.unpack(">6H", header)
print(parsed[0], parsed[1], parsed[2])   // 4660 256 1
```

When no endian prefix is given, big-endian is the default:

```
bytes.pack("2H", [1234, 5678])    // same as ">2H"
```

### Byte conversion

```
// Array of byte values ↔ string
let raw = bytes.from([72, 101, 108, 108, 111])    // "Hello"
let arr = bytes.toArray("Hello")                    // [72, 101, 108, 108, 111]

// Hex encoding
bytes.hex("ABC")                // "414243"
bytes.fromHex("414243")         // "ABC"

// Byte length
bytes.len(data)                 // same as len() but clear intent for binary
```

### Byte-indexed search and slice

The string method versions of `slice`/`indexOf` are grapheme-indexed and corrupt arbitrary binary data. The `bytes.*` versions operate on raw bytes — use these when a string holds non-text content (file uploads, network frames, etc.):

```
bytes.slice(s, start, end?)            // byte-indexed substring
bytes.indexOf(s, sub, startByte?)      // byte offset of first match (-1 if none)
```

Negative indices count from the end. `bytes.slice` clamps out-of-range indices to the string boundary.

```
let body = req.body                    // raw HTTP body, may be binary
let pos = bytes.indexOf(body, "\r\n\r\n")
let headers = bytes.slice(body, 0, pos)
let payload = bytes.slice(body, pos + 4)
```

### Character codes

`.charCode(index?)` returns the Unicode codepoint of the grapheme at the given index (default: 0). `fromCharCode(codepoint)` creates a string from a Unicode codepoint (0-0x10FFFF).

```
"A".charCode()              // 65
"hello".charCode(1)         // 101 (character 'e')
"😀".charCode()             // 128512
fromCharCode(65)            // "A"
fromCharCode(0x1F600)       // "😀"
```

---

## Crypto

The `crypto` namespace provides hashing, HMAC, encryption, password hashing, and secure random bytes.

### Hashing

| Function | Description |
|----------|-------------|
| `crypto.md5(string)` | MD5 hash (32-char hex string) |
| `crypto.sha1(string)` | SHA-1 hash (40-char hex string) |
| `crypto.sha256(string)` | SHA-256 hash (64-char hex string) |
| `crypto.sha512(string)` | SHA-512 hash (128-char hex string) |

```
crypto.md5("hello")     // "5d41402abc4b2a76b9719d911017c592"
crypto.sha1("hello")    // "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d"
crypto.sha256("hello")  // "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
```

### HMAC

`crypto.hmac(key, message, algorithm)` computes a keyed hash. Supported algorithms: `"sha256"`, `"sha1"`, `"sha512"`, `"md5"`.

```
crypto.hmac("secret-key", "message", "sha256")
// "4b393abced1c497f8048860ba1ede46a23f1ff5209b18e9c428bddfbb690aad8"
```

### Random bytes

`crypto.randomBytes(count)` returns cryptographically secure random bytes as a raw string. Use `bytes.hex()` to convert to hex.

```
let key = crypto.randomBytes(32)     // 32 random bytes (256-bit key)
let iv = crypto.randomBytes(16)      // 16 random bytes (128-bit IV)
let token = bytes.hex(crypto.randomBytes(16))  // hex string token
```

### Authenticated encryption — `seal` / `open` (requires OpenSSL)

`crypto.seal` and `crypto.open` are the recommended symmetric encryption API. They use AES-256-GCM, an authenticated (AEAD) cipher: a successful `open` proves both that the ciphertext was produced by someone holding the key AND that no bit of it has been altered. Use this for anything new — cookies, file encryption, request payloads, transport over an untrusted channel.

```
let key = crypto.randomBytes(32)          // 32-byte (256-bit) key

let sealed = crypto.seal("secret data", key)
let plain  = crypto.open(sealed, key)     // throws if tampered or wrong key
print(plain)                              // "secret data"
```

The sealed blob is a single binary string laid out as `nonce(12) ‖ ciphertext ‖ tag(16)`. The nonce is generated freshly per call from the CSPRNG and bundled in; the caller never manages it. **Don't reuse a key for an indefinite number of seals** — birthday-collision probability for the 96-bit nonce becomes non-negligible after ~2^48 messages with the same key. Re-key well before that (rotate per day, per session, per ~10^12 messages, whatever fits).

#### Additional Authenticated Data (AAD)

An optional third argument binds context to the tag without encrypting it. The same value must be passed to `open`, or authentication fails. Use it for context that must match (user ID, timestamp, protocol version) but doesn't need to be secret:

```
let token = crypto.seal(user_data, key, "user-id:42|v=1")
// ... later ...
let data = crypto.open(token, key, "user-id:42|v=1")   // ok
crypto.open(token, key, "user-id:43|v=1")              // throws: AAD mismatch
```

#### Errors

`open` throws on **any** authentication failure — tampered ciphertext, wrong key, or AAD mismatch all surface as the same `authentication failed` error. By design: leaking which specific check failed gives an attacker a usable oracle.

### AES-256-CBC — low-level, unauthenticated (requires OpenSSL)

> **⚠ Read `seal` / `open` above first.** These primitives provide **confidentiality only**. An attacker who can modify the ciphertext bits causes controlled changes to the decrypted plaintext that `decrypt()` cannot detect (padding-oracle and bit-flipping attacks are classic exploits). Use AES-CBC only for interop with legacy systems that demand it; new code should use `crypto.seal` / `crypto.open`.

AES-256-CBC. Key must be 32 bytes, IV must be 16 bytes and must never be reused with the same key.

```
let key = crypto.randomBytes(32)
let iv  = crypto.randomBytes(16)

let encrypted = crypto.encrypt("secret data", key, iv)
let decrypted = crypto.decrypt(encrypted, key, iv)
print(decrypted)   // "secret data"
```

If you must use CBC, also compute an HMAC over `iv || ciphertext` with a separate key and verify it before decrypting — that's the encrypt-then-MAC construction that makes CBC safe. Or just use `seal`/`open`.

### Password hashing (requires OpenSSL)

PBKDF2-SHA256 for secure password storage. Generates a random salt automatically.

```
// Hash a password
let result = crypto.hashPassword("mypassword")
print(result.hash)        // hex hash
print(result.salt)        // hex salt
print(result.iterations)  // 100000

// Verify a password
crypto.verifyPassword("mypassword", result.hash, result.salt)  // true
crypto.verifyPassword("wrong", result.hash, result.salt)        // false
```

Custom iterations: `crypto.hashPassword("pass", nil, 200000)`

### Digital signatures

Sign and verify data with RSA or EC keys.

| Function | Description |
|----------|-------------|
| `crypto.sign(data, privateKeyPEM, algorithm?)` | Sign data, returns base64 signature. Default: `"sha256"` |
| `crypto.verify(data, signature, publicKeyPEM, algorithm?)` | Verify signature, returns boolean |
| `crypto.generateKeyPair(type?, bits?)` | Generate key pair. Returns `{privateKey, publicKey}` as PEM. Type: `"rsa"` (default) or `"ec"` |

```
let keys = crypto.generateKeyPair("rsa", 2048)
let sig = crypto.sign("hello world", keys.privateKey)
crypto.verify("hello world", sig, keys.publicKey)  // true
crypto.verify("tampered", sig, keys.publicKey)      // false

// EC keys (P-256)
let ecKeys = crypto.generateKeyPair("ec")
let ecSig = crypto.sign("data", ecKeys.privateKey, "sha256")
crypto.verify("data", ecSig, ecKeys.publicKey, "sha256")  // true
```

---

## WebSocket (socket grain)

The `socket` grain provides WebSocket server support for Praia's HTTP server. Each WebSocket connection runs in its own background thread.

```
use "socket"

let server = http.createServer(lam{ req in
    if (req.path == "/ws") {
        return socket.upgrade(req, lam{ ws in
            print("Connected: " + ws.id)

            ws.onMessage(lam{ msg in
                ws.send("echo: " + msg)
            })

            ws.onClose(lam{ in
                print("Disconnected")
            })

            ws.send("Welcome!")
        })
    }
    return {body: "Hello"}
})
server.listen(8080)
```

### API

| Function | Description |
|----------|-------------|
| `socket.upgrade(req, handler)` | Upgrade HTTP request to WebSocket. Call inside an HTTP handler |

The `handler` receives a `ws` object:

| Method | Description |
|--------|-------------|
| `ws.send(msg)` | Send a text message |
| `ws.sendBinary(data)` | Send binary data |
| `ws.close()` | Close the connection |
| `ws.onMessage(fn)` | Set message handler — `fn` receives the message string |
| `ws.onClose(fn)` | Set close handler — called when connection ends |
| `ws.id` | Unique connection identifier (e.g. `"ws-1"`) |

### With the router grain

```
use "router"
use "socket"

let app = router.create()

app.get("/ws", lam{ req in
    return socket.upgrade(req, lam{ ws in
        ws.onMessage(lam{ msg in ws.send("echo: " + msg) })
    })
})

app.listen(8080)
```

### Notes

- Each connection runs in its own `async` thread — the server can handle multiple concurrent WebSocket connections alongside normal HTTP requests
- The grain handles the WebSocket handshake (SHA-1 + base64), frame parsing/writing, masking, ping/pong, close handshake, and message fragmentation
- Requires OpenSSL for the SHA-1 handshake hash (same dependency as HTTPS)

---

## hex Grain

The `hex` grain provides hex encoding/decoding utilities for working with binary data, network protocols, and debugging.

```
use "hex"
```

### Functions

| Function | Description |
|----------|-------------|
| `hex.encode(str)` | Encode a raw string to hex (`"AB"` → `"4142"`) |
| `hex.decode(hexStr)` | Decode a hex string to raw bytes (`"4142"` → `"AB"`) |
| `hex.fromInt(n, width?)` | Integer to hex string, optional zero-padded width |
| `hex.toInt(hexStr)` | Hex string to integer (accepts optional `0x` prefix) |
| `hex.dump(data, cols?)` | xxd-style hex dump (default 16 columns) |

### encode / decode

```
hex.encode("Hello")          // "48656c6c6f"
hex.decode("48656c6c6f")     // "Hello"
```

### fromInt / toInt

```
hex.fromInt(255)             // "ff"
hex.fromInt(0xCAFE)          // "cafe"
hex.fromInt(255, 4)          // "00ff"  (zero-padded to 4 chars)
hex.fromInt(0, 2)            // "00"

hex.toInt("ff")              // 255
hex.toInt("0xDEADBEEF")     // 3735928559
hex.toInt(hex.fromInt(42))   // 42  (round-trip)
```

### dump

Produces an xxd-style hex dump with address, hex bytes, and ASCII sidebar:

```
print(hex.dump("Hello, World!\n"))
// 00000000  48 65 6c 6c 6f 2c 20 57 6f 72 6c 64 21 0a     |Hello, World!. |
```

Non-printable bytes show as `.` in the ASCII column. Optional second argument sets columns (default 16).

---

## colors Grain

The `colors` grain provides ANSI color and style helpers for terminal output.

```
use "colors"
```

### Foreground colors

```
print(colors.red("error:") + " something went wrong")
print(colors.green("ok"))
print(colors.yellow("warning"))
print(colors.blue("info"))
print(colors.gray("debug output"))
```

Available: `black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`, `gray`, `brightRed`, `brightGreen`, `brightYellow`, `brightBlue`, `brightMagenta`, `brightCyan`, `brightWhite`

### Background colors

```
print(colors.bgRed(colors.white(" FAIL ")))
print(colors.bgGreen(colors.black(" PASS ")))
```

Available: `bgBlack`, `bgRed`, `bgGreen`, `bgYellow`, `bgBlue`, `bgMagenta`, `bgCyan`, `bgWhite`

### Styles

```
print(colors.bold("important"))
print(colors.underline("link"))
print(colors.dim("muted"))
print(colors.italic("emphasis"))
print(colors.strike("deleted"))
print(colors.inverse("inverted"))
```

### 256-color and RGB

```
print(colors.fg256("orange-ish", 208))
print(colors.bg256("highlighted", 226))
print(colors.rgb("any color", 255, 128, 0))
print(colors.bgRgb("bg color", 50, 50, 50))
```

### Composing styles

Functions nest naturally:

```
print(colors.bold(colors.red("bold red error")))
print(colors.bgYellow(colors.black(" WARNING ")))
```

### Stripping colors

Remove all ANSI escape sequences from a string (useful for logging to files):

```
let colored = colors.red("error")
let plain = colors.strip(colored)    // "error" (no escapes)
```

---

## progress Grain

The `progress` grain provides terminal progress bars and spinners.

```
use "progress"
```

### Progress bar

```
let p = progress.bar({width: 30, showCount: true})
p.total(100)

for (i in 0..101) {
    p.update(i)
    time.sleep(10)
}
p.done()
```

Options (all optional):

| Option | Default | Description |
|--------|---------|-------------|
| `width` | 30 | Bar width in characters |
| `fill` | `"#"` | Fill character |
| `empty` | `"."` | Empty character |
| `showPercent` | true | Show percentage |
| `showCount` | false | Show current/total |
| `color` | nil | ANSI color code (e.g. `"32"` for green) |

Methods:

| Method | Description |
|--------|-------------|
| `p.total(n)` | Set total count |
| `p.update(n)` | Set current value |
| `p.tick(step?)` | Increment by step (default 1) |
| `p.done()` | Fill to 100% and print newline |

### Spinner

```
let s = progress.spinner({message: "Loading..."})
for (i in 0..20) {
    s.tick()
    time.sleep(50)
}
s.done("Complete!")
```

| Method | Description |
|--------|-------------|
| `s.tick(msg?)` | Advance one frame, optionally update message |
| `s.done(msg?)` | Clear spinner, optionally print final message |

Options: `frames` (array of frame strings), `message`, `color`.

---

## table Grain

The `table` grain renders formatted text tables.

```
use "table"
```

### Rendering rows

Pass an array of maps:

```
let users = [
    {name: "Alice", age: 30, role: "admin"},
    {name: "Bob", age: 25, role: "user"}
]
print(table.render(users))
// +-----+-------+------+
// | age | role  | name |
// +-----+-------+------+
// | 30  | admin | Alice|
// | 25  | user  | Bob  |
// +-----+-------+------+
```

### Options

```
table.render(rows, {
    columns: ["name", "age", "role"],         // column order
    headers: {name: "Name", age: "Age"},      // display names
    align: {age: "right", role: "center"},    // alignment per column
    border: false                              // borderless mode
})
```

| Option | Default | Description |
|--------|---------|-------------|
| `columns` | all keys from first row | Array of key names in order |
| `headers` | key names | Map of key to display name |
| `align` | `"left"` | Map of key to `"left"`, `"right"`, or `"center"` |
| `border` | true | Box-drawing borders vs plain aligned columns |

### Key-value display

For vertical key-value output:

```
print(table.kv({Host: "10.0.0.1", Port: 443, Status: "open"}))
// Host    : 10.0.0.1
// Port    : 443
// Status  : open
```

Options: `separator` (default `": "`), `keyWidth` (fixed key column width).

---

## Networking (net)

The `net` namespace provides TCP and UDP socket operations, DNS resolution, and socket timeouts. All functions support both IPv4 and IPv6. Sockets are represented as numbers (file descriptors).

### TCP Client

```
let sock = net.connect("localhost", 5432)
net.send(sock, "hello")
let response = net.recv(sock)
print(response)
net.close(sock)
```

### TCP Server

```
let server = net.listen(9000)
print("listening on 9000")

while (true) {
    let client = net.accept(server)
    let data = net.recv(client)
    net.send(client, "echo: " + data)
    net.close(client)
}
```

### UDP

```
// Send a UDP datagram
let sock = net.udp()
net.sendTo(sock, "127.0.0.1", 9999, "hello udp")
net.close(sock)

// Listen for UDP datagrams
let server = net.udpBind(9999)
let msg = net.recvFrom(server)
print(msg.data, "from", msg.host, msg.port)
net.close(server)
```

### DNS Resolution

```
let ips = net.resolve("example.com")
print(ips)    // ["93.184.216.34", "2606:2800:21f:cb07:6820:80da:af6b:8b2c"]
```

### Socket Timeouts

```
let sock = net.connect("localhost", 8080)
net.setTimeout(sock, 5000)     // 5 second timeout for send/recv
```

### Functions

| Function | Description |
|----------|-------------|
| **TCP** | |
| `net.connect(host, port, timeout?)` | Connect to a TCP server, returns socket. Optional timeout in ms |
| `net.connectAll(targets, timeout)` | Concurrent TCP connect scan. targets: array of `{host, port}` or `[host, port]`. Returns `[{host, port, open}]` |
| `net.listen(port)` | Bind and listen on a port, returns server socket |
| `net.accept(server)` | Wait for and accept a connection, returns client socket |
| `net.send(sock, data)` | Send a string, returns bytes sent. Works with TLS handles |
| `net.recv(sock, maxBytes?)` | Receive data (default 4096 bytes). Returns `""` on timeout or close. Works with TLS handles |
| `net.recvAll(sock)` | Read until the connection closes, returns string. Works with TLS handles |
| **TLS** | |
| `net.tls(sock, hostname?)` | Wrap a TCP socket with TLS, returns TLS handle. Hostname enables SNI + cert verification |
| **UDP** | |
| `net.udp()` | Create an IPv4 UDP socket |
| `net.udp6()` | Create an IPv6 UDP socket |
| `net.udpBind(port)` | Create and bind a UDP socket to a port |
| `net.sendTo(sock, host, port, data)` | Send a UDP datagram |
| `net.recvFrom(sock, maxBytes?)` | Receive a datagram, returns `{data, host, port}` |
| **ICMP** | |
| `net.ping(host, timeout?)` | ICMP echo, returns `{alive, rtt}`. Timeout defaults to 1500ms |
| `net.pingAll(hosts, timeout?)` | Concurrent ping sweep, returns `[{host, alive, rtt?}]` |
| **Raw sockets** | |
| `net.rawSocket(protocol)` | Create a raw socket. Protocol: `"icmp"`, `"icmp6"`, `"tcp"`, `"udp"`, `"raw"`, or a number |
| `net.rawSend(sock, host, data)` | Send raw data to a host |
| `net.rawRecv(sock, maxBytes?)` | Receive raw data, returns `{data, host}` |
| **DNS** | |
| `net.resolve(host)` | DNS lookup, returns array of IP strings (IPv4 and IPv6) |
| `net.query(name, type)` | Raw DNS query. Types: `"A"`, `"AAAA"`, `"MX"`, `"TXT"`, `"NS"`, `"CNAME"`, `"SOA"`, `"PTR"`, `"SRV"` |
| **Interface** | |
| `net.interfaces()` | List network interfaces, returns array of `{name, addresses}` |
| `net.bindInterface(sock, name)` | Bind a socket to a network interface (e.g. `"en0"`) |
| **General** | |
| `net.setTimeout(sock, ms)` | Set send/recv timeout in milliseconds |
| `net.close(sock)` | Close a socket |

### ICMP Ping

`net.ping(host, timeout?)` sends an ICMP echo request and returns `{alive, rtt}`. Works unprivileged on macOS. Requires root or `CAP_NET_RAW` on Linux.

```
let r = net.ping("8.8.8.8")
if (r.alive) { print("up, rtt=" + str(r.rtt) + "ms") }
```

`net.pingAll(hosts, timeout?)` pings multiple hosts concurrently using a single ICMP socket. All echo requests are sent first, then replies are collected via poll(). Much faster than sequential pinging.

```
// Sweep a /24 subnet
let hosts = []
for (i in 1..255) { push(hosts, "192.168.1." + str(i)) }

let results = net.pingAll(hosts, 500)
for (r in results) {
    if (r.alive) { print(r.host + " up (" + str(r.rtt) + "ms)") }
}
```

### Raw Sockets

Raw sockets allow sending and receiving custom protocol packets (ICMP, etc.). Requires root or `CAP_NET_RAW` on Linux. On macOS, unprivileged ICMP echo is supported via a `SOCK_DGRAM` fallback.

```
if (!sys.isRoot()) { print("warning: raw sockets may need root") }

let sock = net.rawSocket("icmp")
net.setTimeout(sock, 2000)

// Build ICMP echo request with bytes.pack
let packet = bytes.pack(">BBHHh", [8, 0, 0, 1, 1])  // type, code, checksum, id, seq
// ... compute checksum, send, receive reply ...
net.rawSend(sock, "127.0.0.1", packet)
let reply = net.rawRecv(sock)
print("reply from:", reply.host)
net.close(sock)
```

With `SOCK_RAW`, received packets include the IP header (first 20 bytes for IPv4). Parse with `bytes.unpack`.

Use `sys.isRoot()` to check privileges before attempting raw socket operations:

```
if (!sys.isRoot()) {
    print("This tool requires root. Run with sudo.")
    sys.exit(1)
}
```

### DNS Queries

`net.query(name, type)` performs raw DNS record lookups. Returns an array of maps, each with `name`, `type`, and `ttl` plus type-specific fields.

For PTR lookups, pass a plain IP address — it's automatically converted to the reverse `.in-addr.arpa` / `.ip6.arpa` form.

Returns an empty array for non-existent domains (NXDOMAIN) or no records of that type (NODATA). Only throws on network-level failures.

```
// A records
let a = net.query("example.com", "A")
// [{name: "example.com", type: "A", ttl: 300, address: "93.184.216.34"}]

// MX records
let mx = net.query("google.com", "MX")
// [{name: "google.com", type: "MX", ttl: 600, priority: 10, exchange: "smtp.google.com"}]

// TXT records (SPF, DKIM, etc.)
let txt = net.query("google.com", "TXT")
// [{name: "google.com", type: "TXT", ttl: 300, text: "v=spf1 include:_spf.google.com ~all"}]

// Reverse DNS (PTR) — pass IP directly
let ptr = net.query("8.8.8.8", "PTR")
// [{name: "8.8.8.8.in-addr.arpa", type: "PTR", ttl: 3600, hostname: "dns.google"}]

// SOA record
let soa = net.query("example.com", "SOA")
// [{..., mname: "ns1.example.com", rname: "admin.example.com",
//   serial: 2024010100, refresh: 3600, retry: 900, expire: 604800, minimum: 86400}]

// SRV record
let srv = net.query("_sip._tcp.example.com", "SRV")
// [{..., priority: 10, weight: 60, port: 5060, target: "sip.example.com"}]

// NS, CNAME, AAAA also supported
let ns = net.query("example.com", "NS")       // target field
let cn = net.query("www.github.com", "CNAME")  // target field
let v6 = net.query("example.com", "AAAA")      // address field
```

### TLS

`net.tls(sock, hostname?)` wraps an existing TCP socket with TLS. Returns a TLS handle that works transparently with `net.send()`, `net.recv()`, `net.recvAll()`, `net.setTimeout()`, and `net.close()`.

When `hostname` is provided, SNI is sent and the server certificate is verified against that hostname. Without it, TLS is established but the certificate is not verified against a specific hostname (useful for pentesting).

```
// HTTPS banner grab
let sock = net.connect("example.com", 443, 3000)
let tls = net.tls(sock, "example.com")
net.setTimeout(tls, 2000)
net.send(tls, "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n")
let response = net.recv(tls)
print(response.split("\r\n")[0])  // "HTTP/1.1 200 OK"
net.close(tls)

// TLS without hostname verification
let sock2 = net.connect("192.168.1.1", 443, 1000)
let tls2 = net.tls(sock2)  // no cert check
net.setTimeout(tls2, 1000)
net.send(tls2, "GET / HTTP/1.1\r\nHost: target\r\nConnection: close\r\n\r\n")
print(net.recv(tls2))
net.close(tls2)
```

Requires OpenSSL (optional build dependency). Throws if OpenSSL is not available.

### Connection Timeouts

`net.connect()` accepts an optional third argument for a timeout in milliseconds. Without a timeout, connect blocks until the OS gives up (often 75+ seconds). With a timeout, it returns quickly — essential for port scanning.

```
// Port scan with 500ms timeout
for (port in [22, 80, 443, 3306, 5432, 8080]) {
    try {
        let sock = net.connect("192.168.1.1", port, 500)
        net.close(sock)
        print("port " + str(port) + " open")
    } catch (e) {
        // closed or filtered
    }
}
```

### Concurrent Connect Scanning

`net.connectAll(targets, timeout)` scans many host/port pairs concurrently using poll-based multiplexing (no threads). Much faster than sequential `net.connect()` calls.

```
// Scan 100 ports in parallel
let targets = []
for (port in 1..1025) {
    push(targets, {host: "192.168.1.1", port: port})
}

let results = net.connectAll(targets, 500)
for (r in results) {
    if (r.open) { print("port " + str(r.port) + " open") }
}
```

Targets can be `{host, port}` maps or `[host, port]` arrays. Results are `{host, port, open}` maps. Internally batches connections to respect file descriptor limits and checks for Ctrl+C between batches.

### Network Interfaces

```
// List all interfaces
let ifaces = net.interfaces()
for (iface in ifaces) {
    print(iface.name + ": " + str(iface.addresses))
}

// Bind a socket to a specific interface
let sock = net.udp()
net.bindInterface(sock, "en0")
```

---

## Grains (Modules)

Praia's module system uses **grains** (like sand grains). A grain can be a single `.praia` file or a directory with multiple files.

### Creating a grain

A grain is any `.praia` file that ends with an `export` statement. Pick
a name that doesn't collide with a builtin namespace (`math`, `http`,
`json`, etc.) — `use` binds the grain into the current scope under
its filename, which would shadow the builtin entirely.

```
// grains/geometry.praia
func circleArea(r) { return math.PI * math.square(r) }
func cubeVolume(s) { return math.cube(s) }

export { circleArea, cubeVolume }
```

### Multi-file grains

A grain can also be a directory with a `grain.yaml` manifest:

```
ext_grains/
  mylib/
    grain.yaml        <- specifies entry point
    main.praia        <- main file
    helpers.praia     <- internal module
```

The `grain.yaml` specifies the entry file:

```yaml
name: mylib
version: 0.1.0
main: main.praia
```

Files within a grain directory can import each other with relative paths:

```
// ext_grains/mylib/main.praia
use "./helpers"

func process(x) { return helpers.double(x) }
export { process }
```

### Importing a grain

Use `use` to import a grain. The grain is bound to a variable named after the last path segment:

```
use "geometry"

print(geometry.circleArea(3))   // ≈ 28.274
print(geometry.cubeVolume(4))   // 64
```

### Custom alias

Use `as` to bind a grain to a different name:

```
use "logger" as log
use "collections" as col

let l = log.create("App")
let s = col.Stack()
```

This is required for grain names that contain hyphens:

```
use "my-grain" as myGrain

myGrain.doSomething()
```

### Relative imports

Paths starting with `./` or `../` are resolved relative to the importing file:

```
use "./helpers/greeter"

greeter.hello("world")
```

### Resolution order

When you write `use "geometry"`, Praia looks for the grain in this order:

1. **`ext_grains/`** — local dependencies (installed by sand), walks up from the current file
2. **`grains/`** — project-bundled grains, walks up from the current file
3. **`~/.praia/ext_grains/`** — user-global grains (sand --global)
4. **`<libdir>/ext_grains/`** — system-global grains (sudo sand --global)

At each location, Praia checks for:
- `<name>.praia` (single-file grain)
- `<name>/` directory with `grain.yaml` → reads `main` field for the entry file
- `<name>/main.praia` (fallback if no `grain.yaml`)

### Grains importing other grains

Grains can import other grains. The builtin `math` namespace is
already in scope inside a grain — no `use` needed for it.

```
// grains/shapes.praia
use "geometry"

func boxedCircleRatio(side) {
    return geometry.circleArea(side / 2) / math.square(side)
}

export { boxedCircleRatio }
```

### Rules

- **No duplicate imports** — importing the same grain twice in one file is an error (enforces clean code)
- **Grains run once** — if multiple files import the same grain, it is only executed the first time; subsequent imports get the cached exports
- **Isolated scope** — grains cannot access the importer's variables; they only see globals and their own definitions
- **Explicit exports** — only names listed in `export { ... }` are visible to the importer

```
use "geometry"
use "geometry"  // Error: Grain 'geometry' is already imported in this file
```

### Package manager (sand)

[sand](https://github.com/praia-lang/sand) is the package manager for Praia. It installs grains from Git repositories.

```bash
sand init                          # create grain.yaml
sand install github.com/user/lib   # install locally to ext_grains/
sand install --global github.com/user/lib  # install globally
sand remove lib                    # uninstall
sand list                          # show installed grains
```

See the [sand documentation](https://github.com/praia-lang/sand) for details.

### Project structure

A typical Praia project might look like:

```
my-project/
├── ext_grains/              <- installed by sand
│   └── router/
│       ├── grain.yaml
│       └── main.praia
├── grains/                  <- project-bundled grains
│   ├── math.praia
│   └── geometry.praia
├── grain.yaml               <- project manifest
├── sand-lock.yaml           <- lock file (auto-generated)
└── main.praia
```

---

## Comments

```
// This is a single-line comment

/* This is a
   multi-line comment */

/* Block comments /* can nest */ like this */
```

---

## Operator Precedence

From highest to lowest:

| Precedence | Operators | Description |
|-----------|-----------|-------------|
| 1 | `()` `[]` `.` | Call, index, field access |
| 2 | `++` `--` | Postfix increment/decrement |
| 3 | `-` `!` | Unary negation, logical NOT |
| 4 | `*` `/` `%` | Multiplication, division, modulo |
| 5 | `+` `-` | Addition, subtraction |
| 6 | `<<` `>>` | Bitwise shift |
| 7 | `&` | Bitwise AND |
| 8 | `^` | Bitwise XOR |
| 9 | `\|` | Bitwise OR |
| 10 | `<` `>` `<=` `>=` | Comparison |
| 11 | `==` `!=` | Equality |
| 12 | `&&` | Logical AND |
| 13 | `\|\|` | Logical OR |
| 14 | `=` | Assignment (right-associative) |

Parentheses can override precedence:

```
print(2 + 3 * 4)       // 14
print((2 + 3) * 4)     // 20
```

---

## Error Stack Traces

When an error occurs inside nested function calls, Praia prints the full call stack:

```
[line 3] Runtime error: Division by zero
  at divide() line 7
  at calculate() line 11
  at main() line 14
```

Stack traces work for all error types: runtime errors, `throw`, and uncaught exceptions. Caught errors (via `try/catch`) do not print a trace — only uncaught errors that terminate execution.

---

## REPL

Run `./praia` with no arguments to start the interactive REPL.

```
$ ./praia
Praia REPL (type 'exit' to quit)
>> 2 + 3
5
>> let x = 10
>> x * 2
20
>> "hello".upper()
HELLO
```

Features:
- **Arrow keys** for command history (up/down) and line editing (left/right)
- **Auto-print** expression results (nil results are hidden)
- **Multi-line input** detected automatically when braces are unbalanced
- **Persistent state** across inputs (variables, functions survive between lines)
- **Ctrl-D** or `exit` to quit

```
>> func greet(name) {
..   print("hello %{name}")
.. }
>> greet("world")
hello world
```

---

## Memory Management

Praia uses **reference counting** (`shared_ptr`) for automatic memory management. Most objects are freed immediately when they go out of scope.

### Cycle collection

Circular references (e.g., two objects pointing at each other) cannot be freed by reference counting alone. Praia includes a **mark-and-sweep cycle collector** that detects and breaks these cycles automatically.

```
// This would leak without cycle collection:
let a = []
push(a, a)      // a references itself
a = nil         // refcount stays 1 due to cycle — GC breaks it
```

The collector runs automatically in the background. It tracks container objects (arrays, maps, instances, classes, generators, environments) and periodically:

1. **Marks** all objects reachable from roots (stack, globals, upvalues)
2. **Sweeps** any tracked object that is alive but not reachable — these are in cycles
3. **Breaks** the cycle by clearing the object's internal references, allowing normal refcount cleanup

### What you need to know

- GC is automatic — no manual intervention needed
- It only targets **cycles**. Non-cyclic objects are freed immediately by refcounting
- Each thread has its own collector (async tasks are isolated)
- The collector auto-tunes its frequency based on how much garbage it finds

### Common cycle patterns (all handled)

```
// Self-referencing collections
let m = {}; m.self = m

// Mutual references
let a = Node(1); let b = Node(2)
a.next = b; b.next = a

// Instance capturing this in a closure
class Foo {
    func init() { this.cb = lam{ in this } }
}

// Function stored in its own closure environment
func make() {
    func inner() { return inner }
    return inner
}
```

---

## Unicode

Praia strings are UTF-8 encoded. When built with [utf8proc](https://github.com/JuliaStrings/utf8proc), all user-facing string operations work on **grapheme clusters** — the visible characters a user perceives, regardless of how many bytes or codepoints make them up.

### Grapheme clusters

A grapheme cluster is a single user-perceived character. It may consist of multiple Unicode codepoints (which may be multiple bytes each in UTF-8):

| String | Graphemes | Codepoints | UTF-8 bytes |
|--------|-----------|------------|-------------|
| `"hello"` | 5 | 5 | 5 |
| `"cafe\u{301}"` | 4 | 5 | 6 |
| `"\u{1F468}\u{200D}\u{1F469}\u{200D}\u{1F467}\u{200D}\u{1F466}"` | 1 | 7 | 25 |
| `"\u{1F1F5}\u{1F1F9}"` | 1 | 2 | 8 |

### What uses grapheme clusters

These operations count and index by grapheme cluster:

- `len(str)` — number of grapheme clusters
- `str[i]` — i-th grapheme cluster
- `for (c in str)` — iterates grapheme clusters
- `.slice(start, end)` — grapheme indices
- `.split("")` — split into grapheme clusters
- `.indexOf()` / `.lastIndexOf()` — returns grapheme index
- `.padStart(n)` / `.padEnd(n)` — counts graphemes for target length
- `.reverse()` — reverses grapheme clusters (so accents stay on their base, ZWJ sequences stay intact)
- `.charCode(i)` — first codepoint of i-th grapheme

### What stays byte-based

These operations work on raw UTF-8 bytes, which is correct and intentional:

- `.contains()`, `.startsWith()`, `.endsWith()` — byte-sequence matching (safe for UTF-8)
- `.replace()` — byte-sequence replacement (safe for UTF-8)
- `.strip()`, `.trimStart()`, `.trimEnd()` — ASCII whitespace is single-byte
- `.test()`, `.replacePattern()` — regex pattern matching operates on bytes internally
- `.match()`, `.matchAll()` — regex matching on bytes, but returned `index` is a grapheme index (consistent with `slice` and `indexOf`)
- `.repeat()`, `.join()` — concatenate whole strings
- `bytes.len(str)` — byte length

### Inspecting string internals

Three methods give access to every level:

```
"A\u{1F600}".graphemes()     // ["A", "\u{1F600}"]    — visible characters
"A\u{1F600}".codepoints()    // [65, 128512]           — Unicode codepoints
"A\u{1F600}".bytes()         // [65, 240, 159, 152, 128]  — raw UTF-8 bytes
```

### Normalization, display width, and collation (`unicode.*`)

The `unicode` namespace exposes operations that need to look at the codepoint stream as a whole, not just walk it.

| Function | Description |
|----------|-------------|
| `unicode.normalize(s, form)` | Canonicalize. `form` is `"NFC"`, `"NFD"`, `"NFKC"`, or `"NFKD"` |
| `unicode.displayWidth(s)` | Monospace cell count: ASCII = 1, CJK / emoji = 2, combining marks = 0. Useful for TUI layout |
| `unicode.collateKey(s)` | Sortable, case-insensitive key. NFD + casefold; accented variants sort to the end of their letter's range |
| `unicode.foldKey(s)` | Like `collateKey` but ALSO strips combining marks — diacritic-INSENSITIVE comparison |

```
// Storage / comparison: normalize before storing so equality works
// regardless of which form the input came in as.
let cleaned = unicode.normalize(userInput, "NFC")

// Terminal layout — wrap or pad based on visual cells, not byte count.
let cellsUsed = unicode.displayWidth(label)
let padding = " ".repeat(20 - cellsUsed)

// Case-insensitive alphabetical sort.
let sorted = sort(names, lam{ a, b in
    unicode.collateKey(a) < unicode.collateKey(b)
})

// Accent-tolerant search ("Élan" finds "elan", "naïve" finds "naive").
if (unicode.foldKey(query) == unicode.foldKey(candidate)) {
    print("match")
}
```

`collateKey` is good enough for case-insensitive alphabetical sort of user-visible names but is not a UCA-grade locale collator. It uses Unicode-default ordering: accented variants of a letter sort to the END of that letter's section rather than interleaving with un-accented variants (real UCA would put "élan" right after "elan"). Applications that need real locale-specific tailoring — Spanish "ll", Swedish ä-after-z, Turkish dotless-i — should link ICU and call `ucol_*` directly.

### Encoding conversions

`s.encode(encoding)` converts a UTF-8 string into bytes in the named encoding; `bytes.decode(b, encoding)` is the inverse.

| Encoding | Notes |
|----------|-------|
| `"utf-8"` | Identity with validation — invalid UTF-8 throws rather than passing through |
| `"utf-16le"` / `"utf-16be"` | Surrogate pairs for codepoints ≥ U+10000; odd byte counts and lone surrogates rejected on decode |
| `"latin-1"` (alias `"iso-8859-1"`) | Single-byte per codepoint; codepoints > U+00FF throw on encode |
| `"ascii"` | Codepoints/bytes must be < U+0080 / < 0x80 |

Encoding names are case-insensitive and ignore `-` / `_`, so `"UTF-8"`, `"utf8"`, and `"Utf_8"` all resolve to the same encoder.

```
"café".encode("latin-1")           // bytes 63 61 66 e9 (4 bytes, "é" as a single byte 0xE9)
"café".encode("utf-8")             // bytes 63 61 66 c3 a9 (5 bytes, "é" as two)
"\u{1F600}".encode("utf-16le")     // bytes 3d d8 00 de (surrogate pair, little-endian units)

let raw = fs.read("legacy.txt")    // unknown — treat as bytes
let text = bytes.decode(raw, "latin-1")  // most "binary safe" interpretation

// Unencodable codepoints throw rather than silently corrupting:
"\u{1F600}".encode("latin-1")      // throws: "codepoint U+01F600 is not encodable in latin-1"
"\u{E9}".encode("ascii")           // throws: "codepoint U+00E9 is not encodable in ASCII"
```

Asian legacy encodings (Shift-JIS, GBK, EUC-KR, Big5, Windows-125x) are out of scope and not bundled — they need either table-based decoders or libiconv. Open an issue if you have a concrete interop need.

### Grapheme-aware reverse

`.reverse()` on strings reverses grapheme clusters, not bytes or codepoints. This is the only sane choice for visually correct reversal — naive byte-reverse would place combining marks before their base letter and shred emoji ZWJ sequences.

```
"hello".reverse()         // "olleh"
"héllo".reverse()         // "olléh" — accent stays attached to e
"\u{1F1F8}\u{1F1EA}\u{1F1FA}\u{1F1F8}".reverse()
                          // "🇺🇸🇸🇪" — whole flags swap; codepoints inside each flag stay paired
```

### Without utf8proc

If Praia is built without utf8proc, all string operations fall back to byte-based behavior (each byte is treated as a character). `unicode.*` throws — there's no useful fallback for normalization or grapheme-aware width. Install utf8proc for proper Unicode support:

```sh
# macOS
brew install utf8proc

# Ubuntu / Debian
sudo apt install libutf8proc-dev

# Fedora / RHEL
sudo dnf install utf8proc-devel
```

---

## Compression (zlib)

`zlib` is one-shot gzip and raw-deflate. Two pairs of inverses:

| Function | Format | Use when |
|----------|--------|----------|
| `zlib.gzip(bytes, level?)` / `zlib.gunzip(bytes)` | RFC 1952 (10-byte header + crc32 + isize) | `.gz` files, HTTP `Content-Encoding: gzip`, log rotation — any payload that needs to be self-framing |
| `zlib.deflate(bytes, level?)` / `zlib.inflate(bytes)` | RFC 1951 raw deflate (no header, no checksum) | Inner layer of a protocol that already frames the compressed body |

`level` is an integer 0..9: 0 = stored (no compression), 1 = fastest, 9 = best. Omit it for zlib's default (currently 6) — a good ratio/CPU balance for typical text.

```
let body = fs.read("server.log")
let gz = zlib.gzip(body)                 // ~10x smaller for typical text
fs.write("server.log.gz", gz)

let raw = fs.read("server.log.gz")
print(zlib.gunzip(raw))                  // recover the original

// HTTP body inflate — the server says Content-Encoding: gzip
let body = zlib.gunzip(resp.body)
```

Invalid input (bytes that aren't gzip-framed, truncated streams, wrong format for the inverse) throws `zlib: inflate failed: <detail>`. Mixing formats — for instance calling `gunzip` on raw-deflate output — is an immediate failure rather than silent corruption.

Streaming variants (process a multi-gigabyte file without holding the whole thing in memory) are intentionally not included; the one-shot API covers nearly all practical use cases. Pair `fs.open` chunked reads with manual chunking if you really need it; open an issue for a real `zlib.deflater()` stateful builder if you have one of the rare workloads where this matters.

---

## Formatting (fmt)

`fmt` is a Go-style formatter for building strings with width, precision, sign control, and typed verbs. Reach for it when string interpolation gets too noisy (`"%-10s | %6.2f"` is clearer than concatenating padded substrings) or when you need precise control over numeric output.

| Function | Description |
|----------|-------------|
| `fmt.sprintf(format, args...)` | Format and return the resulting string |
| `fmt.printf(format, args...)` | Format and write to stdout (no trailing newline) |
| `fmt.println(args...)` | Print args separated by spaces, with a trailing newline (no format string) |
| `fmt.errorf(format, args...)` | Same body as `sprintf`. Named so `throw fmt.errorf(...)` reads naturally |

### Verbs

| Verb | Argument | Output |
|------|----------|--------|
| `%d` | integer (or integer-valued float) | base 10 |
| `%b` `%o` `%x` `%X` | integer | binary / octal / lower-hex / upper-hex |
| `%f` `%F` | number | fixed-point decimal (default 6 fractional digits) |
| `%e` `%E` | number | scientific (default 6 fractional digits) |
| `%g` `%G` | number | shortest of `%e`/`%f` |
| `%s` | any | `toString()` of the value |
| `%q` | any | Go-style quoted string (escapes `"`, `\`, control chars) |
| `%t` | bool | `"true"` / `"false"` |
| `%c` | integer codepoint | UTF-8 character (e.g. `%c` of `0x1F600` is `😀`) |
| `%v` | any | default formatting (same as `%s`) |
| `%T` | any | type name (`"int"`, `"string"`, `"map"`, …) |
| `%%` | — | literal `%` |

### Flags

| Flag | Effect |
|------|--------|
| `-` | left-align inside `width` |
| `+` | force sign on positives |
| ` ` (space) | leading space for positives (sign placeholder) |
| `0` | zero-pad numeric verbs (ignored with `-`) |
| `#` | alternate form: `0b` / `0o` / `0x` prefix for `%b`/`%o`/`%x` |

### Width and precision

`%[flags][width][.precision]verb`. `width` is the minimum output size; `.precision` truncates strings or sets decimal digits for floats. Width is counted in grapheme clusters for non-numeric verbs, matching Praia's `len()`.

```
fmt.sprintf("%10s",   "hi")          // "        hi"
fmt.sprintf("%-10s",  "hi")          // "hi        "
fmt.sprintf("%05d",   42)            // "00042"
fmt.sprintf("%05d",  -42)            // "-0042"     (zero-pad goes AFTER the sign)
fmt.sprintf("%.3f",   3.14159)       // "3.142"
fmt.sprintf("%010.2f", -3.5)         // "-000003.50"
fmt.sprintf("%.3s",   "hello world") // "hel"
fmt.sprintf("%#x",    255)           // "0xff"
fmt.sprintf("%+d",    42)            // "+42"
fmt.sprintf("%c",     0x1F600)       // "😀"
fmt.sprintf("%q",     "say \"hi\"")  // "\"say \\\"hi\\\"\""
fmt.sprintf("%T",     [1, 2])        // "array"
```

### Errors

Format-or-argument mismatches throw — `fmt.sprintf` deliberately fails loudly rather than producing Go's `%!d(string=foo)` "fail-open" output. Throw cases:

- Wrong arg type for a verb (`"%d"` with a string, `"%t"` with a number).
- Too few or too many arguments for the format string.
- Unknown verb (`"%z"`).
- Incomplete spec (`"%5.2"` with no verb).
- Non-integer or out-of-range codepoint for `%c`.

### `throw fmt.errorf(...)` pattern

Praia throws strings; `errorf` makes formatted error messages a one-liner:

```
if (port < 0 || port > 65535) {
    throw fmt.errorf("port %d out of range (expected 0..65535)", port)
}
```

---

## Native Plugins

Praia can load native C++ modules at runtime:

```
let mathext = loadNative("./mathext")
print(mathext.gcd(48, 18))  // 6
```

`loadNative(path)` opens a shared library (`.dylib`/`.so`) and returns a map of native functions. The extension is auto-detected if omitted. Results are cached — loading the same path twice returns the same module.

See [PLUGINS.md](PLUGINS.md) for the full plugin authoring guide.

---

## Command-Line Usage

```
./praia                             # REPL
./praia script.praia                # run a script
./praia script.praia arg1 arg2      # run with arguments (sys.args)
./praia -c 'print("hello")'        # run a one-liner
./praia -c 'print(sys.args)' a b   # one-liner with arguments
./praia test                        # run test suite in tests/
./praia -v                          # print version
./praia --tree script.praia         # run with tree-walker interpreter
./praia --tokens script.praia       # show lexer tokens
./praia --ast script.praia          # show parse tree
```

Semicolons can be used as statement separators, which is useful for one-liners:

```bash
./praia -c 'let x = 1; let y = 2; print(x + y)'
```

### Bytecode VM

Praia uses a bytecode compiler and stack-based VM by default. A tree-walking interpreter is available as a fallback with the `--tree` flag:

```bash
./praia script.praia              # runs with the VM (default)
./praia --tree script.praia       # runs with the tree-walker
./praia --tree test               # test suite with tree-walker
```

Both engines support the full language.
