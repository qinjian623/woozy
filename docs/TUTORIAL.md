# Woozy Language Tutorial

[简体中文](TUTORIAL.cn.md)

Woozy is statically typed and supports local type inference, generics, structs, enums, and declared errors. The rules for functions, values, scopes, and collections are the same across backends.

Start with a complete program, then work through the syntax one feature at a time. If you already know another programming language and want a shorter overview, see the [quick start](QUICKSTART.md). Examples use `.wz` files. For setup instructions, see the [build guide](../development/docs/BUILD.md). For library APIs, see the [standard library guide](STDLIB.md).

## 1. Read a program

This program prints one line of text.

```wz
package main;
import "std/io" as io;
import "std/os" as os;

func main() throws os.Error {
    io.println("Hello, world!");
}
```

`import "std/io" as io;` makes the output functions available as `io`. `io.println` writes the string followed by a newline. `throws os.Error` declares that output can fail. The error handling section shows how to handle that case. The native program prints `Hello, world!`.

The next program adds a list of scores. Save it separately from the first example.

```wz
package main;

func sum(scores Vec[i32]) i32 {
    var total i32 = 0;
    for score in scores {
        total += score;
    }
    return total;
}

func main() i32 {
    let scores Vec[i32] = [12, 18, 20];
    let total = sum(scores);  // 50
    return 0;
}
```

`package main;` declares which package the file belongs to. `sum` takes a list of `i32` integers, loops over them, and returns their sum. `main` is the program's entry point. Returning `0` signals a successful exit.

- Put the type after the name, as in `scores Vec[i32]` or `total i32`.
- Declare functions with `func` and put the return type after the parameter list.
- End simple statements with a semicolon and enclose blocks in braces.
- Use `let` for a binding you cannot reassign and `var` for a binding you can reassign.
- Use `return` to return a value. The final expression in a function body is not returned automatically.

Treat the examples in each section separately. Combine nearby declarations and calls where indicated. Short examples say whether they belong inside a function or at the top level, so they do not each need their own `main`. Comments show expected values. To print strings or bytes, see [std/os](OS.md).

## 2. Comments, bindings, and scope

A single-line comment starts with `//`. A block comment starts with `/*` and ends with `*/`. Block comments can span lines, and nesting is invalid.

```wz
// Inside a function body
let initial i32 = 10;
var count = initial;
count += 2;  // count is 12
/* Both initial and count are i32.
   The type of count is inferred from its initializer. */
```

Reassigning `initial` is an error. Both `let` and `var` need an initializer, and later assignments must preserve the binding's type. You can omit the type of a local binding. Function parameters and struct fields require a type.

A `const` needs an initializer known at compile time. You can declare it at the top level or in a local scope.

```wz
const PassingScore i32 = 60;
const Greeting string = "你好，Woozy";
```

Ordinary function calls, Vec values, and foreign objects are invalid as constant initializers. You can construct constant structs and enums recursively and read fields of constant values.

Braces introduce a local scope. Names must be unique within a scope. An inner scope can shadow an outer name.

```wz
// Inside a function body
let x i32 = 1;
{
    let x = x + 1;  // The right side reads the outer x. The inner x is 2.
}
let original = x;   // Still 1
```

A new binding becomes visible after its initializer finishes. Branches, loop bodies, switch arms, and catch blocks also have their own local scopes.

## 3. Primitive types and literals

| Type | Meaning |
| --- | --- |
| `bool` | `true` or `false` |
| `i8`, `i16`, `i32`, `i64` | Signed integers of the specified width |
| `u8`, `u16`, `u32`, `u64` | Unsigned integers of the specified width |
| `isize`, `usize` | Integers whose width depends on the target |
| `f32`, `f64` | IEEE 754 floating-point numbers |
| `string` | An immutable UTF-8 string |
| `Unit` | A value type with no data, often used for functions that return no data |

Here are several ways to write numbers.

```wz
// Inside a function body
let decimal = 1_000;  // Defaults to i64
let binary = 0b1010;
let octal = 0o755;
let hex = 0xFF;
let ratio = 0.25;     // Defaults to f64
let scale f32 = 1e3;  // The context supplies f32
let small u8 = 10;
```

Without a type context, integer literals default to `i64` and floating-point literals to `f64`. Context can determine a literal's type. Variables that already have a concrete type keep it. Type suffixes such as `123i32` are invalid, as are `.5`, `1.`, and `007`.

Double-quoted strings support newline, tab, and Unicode escapes. Backticks enclose raw strings, which treat backslashes literally and can span lines.

```wz
// Inside a function body
let greeting = "你好";
let lines = "第一行\n第二行";
let same_greeting = "\u{4F60}\u{597D}";
let pattern = `\d+\.\d+`;
let message = `第一行
第二行`;
```

Single-quoted character literals, string interpolation, and `\xHH` escapes are unavailable. A `string` is an immutable UTF-8 value. You can store it, copy it, pass it as an argument, and put it inside an aggregate. String operations include concatenation, equality, and ordering. Comparisons use UTF-8 bytes without Unicode normalization.

`std/text` counts Unicode code points with `len`. String indexing returns a `u32` code point, and slice endpoints also use code point positions. Use `text.bytes` to get the bytes and `text.from_bytes` to build a string. Invalid UTF-8 makes `text.from_bytes` throw `text.Error`. Import `std/text` first.

```wz
import "std/text" as text;
```

Put the following code inside a function body.

```wz
let sample = "a你😀";
let count = text.len(sample);            // 3 code points
let byte_count = text.byte_len(sample);  // 8 bytes
let second = sample[1];                  // 20320, a u32
let tail = sample[1:3];                  // "你😀"
let raw = text.bytes(sample);            // Vec[u8]
raw[0] = 98;                             // Editing raw leaves sample intact
try {
    let restored = text.from_bytes(text.bytes(sample));
} catch error text.Error {
    // The input is not valid UTF-8.
}
```

Concatenate strings with `+` and compare them with `==`, `!=`, or the ordering operators. You can use `+=` to update a whole `var` binding.

```wz
var greeting = "你好";
greeting += "，Woozy";  // Creates a new string
```

String `+=` applies to whole `var` bindings only. For an assignable field or indexed element, write `target = target + suffix` instead. A string constant needs a value that can be determined at compile time. For example, `const C = "a" + "b";` is an error.

## 4. Operators and conversions

Use `+ - * / %` for arithmetic and `== != < <= > >=` for comparisons. The remainder operator `%` requires integers. The type checker determines which operations a concrete type supports. For example, integer equality does not imply Vec equality.

The logical operators are `!`, `&&`, and `||`. The last two short-circuit. The bitwise operators are `~`, `&`, `|`, `^`, `<<`, and `>>`. Assignment and compound assignment are statements.

To combine different concrete numeric types, convert explicitly with `Type(value)`.

```wz
// Inside a function body
let count i32 = 7;
let wide i64 = i64(count);
let average f64 = f64(count) / 2.0;  // 3.5
let truncated = i32(3.9);           // 3
let negative = i32(-3.9);           // -3
```

Integer-to-integer conversions check the target range. A float-to-integer conversion truncates toward zero before checking the range. Conversions known to fail are compile errors. At runtime, converting an out-of-range value, NaN, or infinity to an integer panics.

Fixed-width integer addition, subtraction, and multiplication wrap.

```wz
// Inside a function body
let highest i8 = 127;
let wrapped = highest + 1;  // -128
let quotient i32 = -7 / 3;  // -2, truncated toward zero
let remainder i32 = -7 % 3; // -1, with the sign of the dividend
```

Integer division by zero and invalid shifts panic. Floating-point division by zero gives Inf or NaN according to IEEE 754. Floating-point operations use `f32` or `f64` precision, with relaxed-precision rules disabled by default. For details, see the [numeric rules](SEMANTICS.md#4-数值运行语义).

## 5. Functions and calls

Declare a top-level function with `func`.

```wz
func add(left i32, right i32) i32 {
    return left + right;
}
```

Omit the return type for a `Unit` function, which can run to the end of its body. A function with any other return type must return a value on every path that completes normally.

After declaring `add`, you can call it from inside a function body.

```wz
let positional = add(2, 3);              // 5
let named = add(right: 3, left: 2);       // 5
let mixed = add(2, right: 3);            // 5
```

Positional arguments come before named arguments. The callee and arguments are evaluated from left to right as written, regardless of the parameter names. Functions have no default arguments, variadic parameters, or overloading by name.

Ordinary parameter bindings are read-only. To change a numeric value, assign it to a local `var` first. A later section shows how to use `inout` to modify a caller's variable.

Functions can also be passed as values. This example declares a function type and creates a function literal.

```wz
func apply(operation func(i32) i32, value i32) i32 {
    return operation(value);
}

func calculate() i32 {
    let twice = func(value i32) i32 {
        return value * 2;
    };
    return apply(twice, 21);  // 42
}
```

A function type records its parameter types and modes, return type, and error type. Parameter labels are omitted, so calls through function values use positional arguments only.

A function literal can capture local variables from the function that creates it. Capture happens when the literal is evaluated. Values such as numbers are copied at that point. Capturing a Vec or Box shares the same object. Captured bindings are read-only within the function value, and shared objects remain mutable through their APIs. The reader returned below keeps working after the scope that created it has exited.

```wz
func make_reader(seed i32) (func() i32) {
    let values Vec[i32] = [seed, seed + 1];
    return func() i32 { return values[0] + values[1]; };
}

func read_value() i32 {
    let read = make_reader(20);
    return read();  // 41
}
```

A plain `func` copies its captured values. A `move func(...) { ... }` transfers the owned values it captures, such as a Vec. Values such as numbers still copy normally. Reinitialize a binding before reading it after a move.

```wz
func moved_capture() i32 {
    var values Vec[i32] = [41];
    var scalar i32 = 1;
    let read = move func() i32 { return values[0] + scalar; };
    scalar = 2;
    return read();  // 42
}
```

A method can also be stored as a function value. Evaluating `value.method` saves a logical copy of the receiver. Rebinding the original variable or replacing its value fields leaves the saved copy intact. Reference fields, such as Vec fields, still share an object, and updates to its contents are visible through the saved receiver. A method with a `move Self` receiver can also be bound. Its function value can be copied and called repeatedly. Each call copies the saved receiver and moves the new copy into the method.

```wz
struct Counter {
    value i32;
}

impl Counter {
    func read(self move Counter) i32 { return self.value; }
}

func repeat_bound() i32 {
    let counter = Counter(value: 7);
    let read = (move counter).read;
    let copied = read;
    return read() + copied();  // 14
}
```

Methods with an `inout Self` receiver cannot be stored as function values. Captured values are copied and released along with the function value.

## 6. Branches and loops

Conditions require a `bool`. Zero and empty containers do not count as false.

```wz
// In a function body
let score i32 = 72;
var passed = false;
if score >= 60 {
    passed = true;
} else {
    passed = false;
}
```

Woozy uses `for` for three-clause loops, conditional loops, and infinite loops.

```wz
// In a function body
var total i32 = 0;
for var i i32 = 0; i < 4; i += 1 {
    total += i;
}
// total is 6. i is out of scope after the loop.

var remaining i32 = 3;
for remaining > 0 {
    remaining -= 1;
}

var attempts i32 = 0;
for {
    attempts += 1;
    if attempts == 2 {
        break;
    }
}
```

`break` exits the innermost loop or switch. `continue` skips to the next loop iteration. To iterate over a Vec, use `for element in values`, as shown in the next section.

## 7. Vec literals, indexing, and slicing

`Vec[T]` holds elements of type `T`. A list literal creates a Vec.

```wz
// In a function body
let values Vec[i32] = [12, 18, 20];
let first = values[0];       // 12
let first_two = values[0:2]; // [12, 18]
let empty Vec[i32] = [];
var total i32 = 0;
for value in values {
    total += value;
}
// total is 50
```

All elements in a list must resolve to the same type. Type inference gives the same result regardless of element order. An empty list needs a type from context. A slice includes the start index and excludes the end index. You can omit either endpoint. The new Vec copies the selected elements.

Assigning a Vec shares the same container.

```wz
// In a function body
let scores Vec[i32] = [12, 18, 20];
let alias = scores;
alias[0] = 15;               // scores[0] is now 15 too
let copy = scores[0:2];
copy[0] = 99;                // scores[0] is still 15
```

`let` prevents reassignment and changes to the fields of a Woozy value. You can still change elements of a shared Vec. A slice copies each element by value. If an element is itself a Vec, the copy shares that inner container.

`std/vec` provides operations to inspect the length and modify the container. Add `import "std/vec" as vec;` before using the following code in a function body.

```wz
let items = vec.empty[i32]();
vec.reserve(items, 8);
vec.push(items, 12);
vec.push(items, 18);
let last = vec.pop(items); // option.Option[i32]
vec.resize(items, 4, 0);   // Length is now 4, with new elements set to 0
let capacity = vec.capacity(items);
vec.clear(items);          // Length is 0 and capacity is unchanged
```

`vec.pop` returns `None` on an empty Vec. When `resize` grows a Vec, it fills the new elements with the given value. When it shrinks a Vec, it truncates the elements. `clear` keeps the allocated capacity. Since Vec assignment shares the container, changes are visible through every alias.

Vec indices must be nonnegative. Out-of-bounds indices and invalid slices cause a Panic. You can update existing elements while iterating. Keep the length and storage of the Vec fixed during the loop, so avoid calling `push`, `pop`, `resize`, `clear`, or `reserve` on it.

### Shared cells and weak references

`std/box.Box[T]` stores a mutable value. Copying a Box gives you a handle to the same cell. `box.get` reads the current value and `box.set` replaces it.

```wz
import "std/box" as box;

func shared_cell() i32 {
    let cell = box.new[i32](5);
    let alias = cell;
    box.set(alias, 8);
    return box.get(cell);  // 8
}
```

`Weak[T]` is a built-in type for storing a weak reference without extending the target's lifetime. `std/weak` provides three operations. Valid targets include Vec, function values, `Box[T]`, and reference-counted shared handles. Plain struct and enum values, strings, file and socket descriptors, and foreign Python or C objects cannot be used directly as targets. `weak.empty[T]()` creates an empty weak reference. `weak.downgrade[T](value)` creates one from an existing strong reference. `weak.upgrade(w)` returns an `option.Option[T]`. If the target has no strong references left, it returns `None`.

```wz
import "std/option" as option;
import "std/weak" as weak;

func observe_values() i32 {
    var saved Weak[Vec[i32]] = weak.empty[Vec[i32]]();
    {
        let values Vec[i32] = [7];
        saved = weak.downgrade[Vec[i32]](values);
        switch weak.upgrade(saved) {
        case option.Option.Some(strong):
            if strong[0] != 7 { return 1; }
        case option.Option.None:
            return 2;
        }
    }
    switch weak.upgrade(saved) {
    case option.Option.Some(strong):
        return 3;
    case option.Option.None:
        return 0;
    }
}
```

Weak references can be stored in structs, enums, Vecs, and generic values. Plain value aggregates cannot serve as weak reference targets. See the [standard library guide](STDLIB.md) for the API.

Woozy automatically collects cycles made up of strong references among pure Woozy Vecs, Boxes, and closures. Support for cycles involving tasks or coroutine frames is still incomplete. See the [development notes](../development/docs/LANGUAGE_DOC_FACTS.md) for details.

## 8. Structs and methods

Declare the struct and its method at the top level.

```wz
struct Point {
    x i32;
    y i32;
}

impl Point {
    func squared_length(self Self) i32 {
        return self.x * self.x + self.y * self.y;
    }
}
```

Declare the type of every struct field. Initialize all fields when constructing a struct, since fields have no default values. `Self` is the type being implemented. The receiver is the first parameter of a method.

Once you have declared the type above, use it in a function body.

```wz
var point = Point(x: 3, y: 4);
let length = point.squared_length();  // 25
let saved = point;
point.x = 6;
// saved.x is still 3. point.x is 6.
```

Structs copy field by field. An integer field gets an independent copy of its value. A Vec field continues to share the container. A struct bound with `let` does not allow direct field updates.

## 9. Enums and pattern matching

An enum describes several mutually exclusive cases. Each variant can carry different data. Declare the enum and functions at the top level.

```wz
enum Verdict {
    Pass(total i32);
    Fail;
}

func evaluate(total i32) Verdict {
    if total >= 60 {
        return Verdict.Pass(total);
    }
    return Verdict.Fail;
}

func accepted(verdict Verdict) bool {
    switch verdict {
        case Verdict.Pass(total):
            return total >= 60;
        case Verdict.Fail:
            return false;
    }
}
```

Use `Verdict.Fail` for a variant without data. Call the constructor for a variant that carries data. `case Verdict.Pass(total):` extracts the score. `total` is visible only in that branch.

`switch` cases do not fall through and must cover every possible value. The example above covers both variants. A switch on `bool` can list `true` and `false`. If you cannot list all values, add `default:`, `case _:`, or another catch-all branch.

```wz
func category(code i32) i32 {
    switch code {
        case 0:
            return 10;
        case 1:
            return 20;
        default:
            return -1;
    }
}
```

The subject expression is evaluated once. A bare identifier pattern always creates a new binding. For example, `case limit:` binds the subject to a new name instead of comparing it with an outer `limit`. `_` matches any value without introducing a name you can read. Range patterns, guards, and switch expressions are currently unsupported.

## 10. Ordinary parameters, move, and inout

Ordinary parameters leave the caller's value available. Numeric values are copied and Vec values continue to share the container. The parameter binding itself is read-only. The following function changes a Vec element.

```wz
func replace_first(values Vec[i32], value i32) {
    values[0] = value;
}
```

Use `inout` in both the declaration and the call to modify the caller's storage.

```wz
func increment(value inout i32) {
    value += 1;
}

func changed() i32 {
    var count i32 = 10;
    increment(inout count);
    return count;  // 11
}
```

`inout` accepts writable `var` bindings, existing `inout` parameters, their nested fields, and container elements. Examples include `swap(inout state.left, inout state.right)`, `bump(inout values[0])`, and `grow(inout items[1].count)`. Eligible locations can hold numbers, bools, finite structs or enums containing only trivial values recursively, Vec or Slice containers themselves, strings, function values, Weak or dyn values, and aggregates with owned fields. The callee can replace the entire value, releasing the old value exactly once. A single call can borrow distinct named fields or provably different literal indices separately. The compiler rejects overlapping access paths, conflicting accesses to the whole container, and dynamic index conflicts. Using `inout` with `Task` or a foreign C resource reports `E_CAPABILITY_UNAVAILABLE`. Vec and Slice handles can share an object through copied bindings, structs or enums, function return values, or closures. While an element is borrowed with `inout`, the compiler conservatively rejects other accesses that might alias it.

Use `move` to transfer ownership of a value.

```wz
func take_first(values move Vec[i32]) i32 {
    return values[0];
}

func transferred() i32 {
    let values Vec[i32] = [12, 18];
    let kept = values;
    let first = take_first(move values);
    // values is no longer readable. kept can still use the shared container.
    return first + kept[1];  // 30
}
```

After a move, reinitialize the binding before reading it again. Other aliases remain intact and the container is not deep-copied. To move an ordinary parameter, first make a local copy.

Method receivers follow the same rules. An ordinary `value.method()` call neither moves the receiver nor grants writable access. If the receiver declaration uses `move` or `inout`, write the call as `(move value).take()` or `(inout value).change()`.

## 11. Declared errors

`throws E` means the function may throw an error of type `E`. On success, it still returns its declared result type.

```wz
enum ScoreError {
    Negative(value i32);
}

func require_score(value i32) i32 throws ScoreError {
    if value < 0 {
        throw ScoreError.Negative(value);
    }
    return value;
}

func checked_sum(scores Vec[i32]) i32 throws ScoreError {
    var total i32 = 0;
    for score in scores {
        total += require_score(score);
    }
    return total;
}
```

Errors from `require_score` propagate automatically through `checked_sum`, so the call needs no `?` marker. The caller must declare a compatible `throws` or handle the error with `try/catch`. With both functions declared, you can write the following.

```wz
func handled() i32 {
    try {
        return checked_sum([12, -1, 20]);
    } catch error ScoreError {
        switch error {
            case ScoreError.Negative(value):
                return value;  // Returns -1 in this example
        }
    }
}
```

An ordinary enum can serve as an error type without implementing a special Error trait. A custom `Result[T, E]` is an ordinary value and does not trigger error propagation. The standard library's `std/result.Result[T,E]` is also an ordinary enum with `result.Result.Ok` and `result.Result.Err` variants. Write out the type arguments when constructing a generic enum, as in `result.Result[i32,i32].Ok(7)`. When matching it from another package, write `result.Result.Ok(value)`. Catching an error leaves any container changes or external operations already performed in effect.

Runtime failures such as out-of-bounds access and integer division by zero cause a Panic, which a typed catch cannot recover from. Use `throws` for expected errors that callers need to handle.

## 12. defer and leaving a block

A `defer` runs when control leaves the current block. The last registered action runs first.

```wz
// Inside a function body
var order i32 = 0;
{
    defer { order = order * 10 + 1; }
    defer { order = order * 10 + 2; }
}
// After this block, order is 21
```

A `defer` belongs to the lexical block where it is declared. Cleanup runs when control reaches the end of the block or leaves it through `return`, `break`, `continue`, or a typed throw. A Panic can skip cleanup.

A `defer` reads the variable values in effect when it runs. A return expression is evaluated and saved before cleanup.

```wz
func before_cleanup(value inout i32) i32 {
    defer { value += 1; }
    return value;
}

func demonstrate() i32 {
    var value i32 = 10;
    let returned = before_cleanup(inout value);
    // returned is 10 and value is 11
    return returned + value;  // 21
}
```

Create a new binding first if a `defer` needs a snapshot. A deferred block can handle errors internally. It cannot `return`, `break`, or `continue` out of the deferred block or propagate a typed error from it.

## 13. Packages, files, and visibility

The `.wz` files in one directory belong to the same package and must declare the same package name. Top-level declarations are visible across files in that package without imports.

To access a declaration from another package, give the import an alias and mark the declaration `pub`. Suppose `app` names the project's source root. Then `app/stats` names the package in the `stats/` directory, where `stats.wz` can contain the following.

```wz
package stats;

pub func passed(total i32) bool {
    return total >= 60;
}
```

A file in another package imports it by its logical module name.

```wz
package main;
import "app/stats" as stats;

func main() i32 {
    let passed = stats.passed(72);  // true
    return 0;
}
```

An import alias applies only in the file that declares it. Import names omit the `.wz` extension. Package imports must be acyclic, and functions can call each other recursively. Top-level declarations can define constants, types, functions, traits, and implementations. Top-level `let` and `var` bindings with dynamic initialization are currently unsupported.

## 14. Generics and traits

Type parameters go in square brackets. Here is a generic struct and a generic function.

```wz
struct Pair[T] {
    first T;
    second T;
}

func identity[T](value T) T {
    return value;
}
```

Use the declarations in a function body as follows.

```wz
let pair = Pair[i32](first: 12, second: 18);
let explicit = identity[i32](pair.first);  // 12
let inferred = identity(pair.second);     // The argument gives T = i32 and the result is 18
```

A generic function can use only the operations declared for its type parameter. To call a method on a type parameter, add a trait constraint and supply an implementation.

```wz
trait Ordered {
    func less(self Self, other Self) bool;
}

impl Ordered for i32 {
    func less(self Self, other Self) bool {
        return self < other;
    }
}

func smaller[T Ordered](left T, right T) T {
    if left.less(right) {
        return left;
    }
    return right;
}
```

`smaller[i32](12, 18)` returns `12`. When constructing a generic enum, supply all type arguments, as in `option.Option[i32].Some(1)`. From another package, match the variant with `option.Option.Some(value)`. Both `std/option.Option` and `std/result.Result` are ordinary generic enums. The shorter pattern `Option.Some(value)` is available inside the `std/option` package. The type must explicitly implement the trait, even if it already has a method with the same name. A type parameter can have at most one named constraint, and traits can themselves take type parameters. One type can implement the same trait with different type arguments. An ambiguous call is a compile error, and the expected return type does not resolve it.

C++-style template specialization, const generics, and associated types are currently unsupported. For runtime polymorphism, convert a concrete value to a `dyn Trait` value and call a trait method on it.

```wz
trait Area {
    func area(self Self) i32;
}

struct Square {
    side i32;
}

impl Area for Square {
    func area(self Square) i32 { return self.side * self.side; }
}

func area_of_shape() i32 {
    let square = Square(side: 3);
    let shape dyn Area = square;
    return shape.area();  // 9
}
```

A method on `dyn Trait` must take an ordinary `self Self` receiver and ordinary by-value parameters. That excludes both `move` and `inout` parameters, including `move i32`. The method cannot declare its own type parameters, return `Self`, throw errors, or use `Self` in a non-receiver parameter. Each concrete value needs exactly one complete implementation of the trait. A `Vec[dyn Trait]` can hold values of different concrete types. See [generics and traits](SEMANTICS.md#6-泛型和-trait) for the full rules.

### Custom iterators

Implement `std/iter.Iterator[T]` to use a type in a for-in loop. The `next` method updates the iterator through `inout`. It returns `Some` to yield an element or `None` to end iteration.

```wz
package app;
import "std/iter" as iter;
import "std/option" as option;

struct Counter { value i32; limit i32; }

impl iter.Iterator[i32] for Counter {
    func next(cursor inout Self) option.Option[i32] {
        if cursor.value >= cursor.limit {
            return option.Option[i32].None;
        }
        let item = cursor.value;
        cursor.value += 1;
        return option.Option[i32].Some(item);
    }
}

func main() i32 {
    let source = Counter(value: 0, limit: 4);
    var total i32 = 0;
    for item in source { total += item; }
    if source.value != 0 { return 1; }
    return total - 6;
}
```

The loop evaluates its source once, saves a copy of the iterator, and advances that copy. In this example, `source.value` stays at 0. Reference fields such as Vec still share the same objects after copying. To advance your own mutable iterator variable, call `(inout cursor).next()`.

The `next` method cannot declare `throws`. Use a Result as the element type `T` to report an error for an individual element. If a type implements Iterator with several different type arguments, a direct for-in loop is ambiguous. A generic function with a specific Iterator constraint can select the corresponding implementation. The loop requires an implementation of the standard trait, even if the type already has a method named `next`.

## 15. Threads and shared data

A thread entry point is a function without captures, and it receives its data through an argument. `thread.spawn[A](function, argument)` starts a thread, and `thread.join(job)` waits for it and returns an `i64` result. Both calls can throw `os.Error`.

Passing a Vec to a thread shares the same container. When multiple threads access shared mutable data, protect it with a Mutex from `std/sync`. Use Condvar to wait for a shared condition and AtomicI64 for an atomic counter. Atomic reference counting protects the reference count itself. See [threads and synchronization](CONCURRENCY.md) for full examples and APIs.

### Async functions

An `async func` returns `Task[R,E]` instead of its declared result, and calling it does not run the body. Inside another async function, `await task` starts the task if it was never spawned and returns its result; a task can be awaited more than once. `async func main` is a valid entry point. `std/async` provides `sleep`, descriptor waits, `spawn`/`run`, cancellation, and `shutdown`. The executor runs on one thread, and async is available on the Native backend for Linux x86_64 and macOS arm64. See [threads and async](CONCURRENCY.md#async-与-await) for the full rules.

## 16. Common standard library modules

Import standard library modules using `std/...`. This complete program splits input with `std/text`, parses integers with `std/strconv`, and prints their sum with `std/io`.

```wz
package main;
import "std/io" as io;
import "std/os" as os;
import "std/strconv" as strconv;
import "std/text" as text;

func main() throws os.Error {
    let pieces = text.split("12,18,20", ",");
    var total i64 = 0;
    for piece in pieces {
        try {
            total += strconv.parse_i64(piece);
        } catch error strconv.Error {
            io.eprintln("invalid number: " + piece);
            return;
        }
    }
    io.println(total);
}
```

`std/io.println` adds a newline, and `std/io.eprintln` writes to standard error. Use `std/fs` to read and write files, and `std/os` and `std/io` for file and buffered I/O. For time, randomness, math, networking, threads, and synchronization, see the [standard library guide](STDLIB.md), [system interfaces](OS.md), and [threads and synchronization](CONCURRENCY.md).

Functions such as `std/fs.read_text` and `std/fs.write_text` open and close files for you. When you work directly with `os.File`, `io.Reader`, or `io.Writer`, you are responsible for closing and calling `flush`. See the [standard library guide](STDLIB.md) for details.

## Further reading

The [quick start](QUICKSTART.md) focuses on the differences experienced programmers are likely to encounter. [SEMANTICS.md](SEMANTICS.md) defines the exact behavior of numbers, copying, errors, and cleanup. The [lexical and syntax reference](../baseline/LEXICAL_SYNTAX.md) covers literals and declarations. [System interfaces](OS.md) covers output, file I/O, and process operations. The [standard library guide](STDLIB.md) lists APIs and resource rules by module.

The chapter structure draws on the [Rust Book](https://github.com/rust-lang/book/blob/main/src/SUMMARY.md) and the [basics section of the Tour of Go](https://github.com/golang/website/blob/master/_content/tour/basics.article). The examples and language rules are specific to Woozy.
