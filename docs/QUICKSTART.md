# Woozy Quick Start

[简体中文](QUICKSTART.cn.md)

This guide is for programmers who already know another language. For installation, see the [0.1.0 release notes](RELEASE-0.1.0.md); to build from source, see the [build guide](../development/docs/BUILD.md).

Start with a complete Woozy program:

```wz
package main;

func add(left i32, right i32) i32 {
    return left + right;
}

func main() i32 {
    let values Vec[i32] = [1, 2, 3];
    var total i32 = 0;
    for value in values {
        total += value;
    }
    let answer = add(right: 1, left: total); // answer is 7
    return 0;
}
```

A few Woozy rules are worth remembering first:

| Item | Syntax and behavior |
| --- | --- |
| Declarations | Types follow names; simple statements require a semicolon, and a newline does not replace one |
| Mutability | `let` cannot rebind or modify value fields; `var` can; elements of a shared Vec can still be modified |
| Calls | Ordinary parameters are read-only and do not consume arguments; write `move` and `inout` explicitly |
| Errors | `throws E` propagates uncaught errors automatically; do not write `?` for every call |
| Cleanup | `defer` runs when the current lexical block exits; the last registered action runs first |
| Constraints | Generics use `[T]`, and traits require an explicit `impl`; use static calls or `dyn Trait` |

Examples below are marked as top-level declarations or function-body fragments. Put top-level declarations after `package`; put function-body fragments inside `main` or another function. Declarations needed by a fragment appear in the adjacent code block.

## Standard output

This complete program prints text on Linux x86_64 and macOS arm64.

```wz
package main;
import "std/io" as io;
import "std/os" as os;

func main() throws os.Error {
    io.println("Hello, world!");
}
```

`std/io.println` writes a string followed by a newline and throws `os.Error` on failure. File and system interfaces use C-style names and argument order; see [std/os](OS.md) for file I/O, short writes, interruptions, and File lifetime, and the [standard library](STDLIB.md) for the module list. Modules that make system calls work only for native programs on Linux x86_64 and macOS arm64, not on the Python backend.

## Bindings and types

Function-body fragment:

```wz
let count i32 = 3;
var total = count + 1;      // i32 inferred from count
const step i32 = 2;
total += step;             // total is 6
let default_integer = 10;  // i64 when unconstrained
let ratio = 0.5;           // f64 when unconstrained
let widened = i64(count);  // explicit numeric conversion
let empty Vec[i32] = [];   // an empty list needs an element type
let title string = "Woozy";
```

Parameters and return types must be explicit (omitting the return type means `Unit`), while local bindings can infer their types. Ordinary parameter bindings are read-only; copy one into a local `var` if you need to reassign it. A `const` initializer must be known at compile time and cannot call ordinary functions or create a Vec or foreign object.

Integer types are `i8/i16/i32/i64`, `u8/u16/u32/u64`, and the target-width `isize/usize`; floating-point types are `f32/f64`. Different concrete numeric types do not mix implicitly; convert with `T(value)`. Literals can take their type from context.

Fixed-width integer addition, subtraction, and multiplication wrap, and integer division truncates toward zero. Integer division by zero, out-of-range dynamic integer conversions, and invalid shifts panic; conversions known to fail at compile time are compile errors. Floating-point division by zero produces infinity or NaN under IEEE rules. `bool` is not interchangeable with integers.

A `string` is an immutable UTF-8 value. Indexing, slicing, and `text.len` (from `std/text`) count Unicode code points, and indexing returns a `u32`. Strings support `+`, `+=` on a whole `var` binding, `==`, and ordering comparisons, comparing UTF-8 bytes without Unicode normalization. `std/text` provides `bytes` and `from_bytes` to convert explicitly to and from `Vec[u8]`; `from_bytes` validates UTF-8 strictly and throws `text.Error`. String interpolation is still unsupported, and `const` strings accept only values known at compile time.

Comments start with `// ...` or `/* ... */`; block comments do not nest. Common operators are `+ - * / %`, `== != < <= > >=`, `! && ||`, and integer bitwise operators; `%` applies only to integers.

## Functions, argument labels, and function values

Top-level declarations:

```wz
func combine(first i32, second i32) i32 {
    return first * 10 + second;
}

func apply(operation func(i32, i32) i32) i32 {
    return operation(2, 3);
}
```

Function-body fragment using the declarations above:

```wz
let direct = combine(second: 3, first: 2); // 23
let operation func(i32, i32) i32 = combine;
let indirect = operation(2, 3);           // 23
let applied = apply(operation);          // 23
let twice = func(value i32) i32 {
    return value * 2;
};
let doubled = twice(4);                  // 8
```

Direct calls may use parameter names, and positional arguments must come before named arguments. Arguments are always evaluated in written order, so reordering named arguments does not reorder side effects. Function types do not retain parameter labels; calls through a function value use positional arguments only.

Function literals can capture outer local bindings when created. Captured bindings are read-only, though shared objects such as Vec and Box can still be modified through their APIs. A plain `func` copies captured values, while `move func(...) { ... }` moves the owned values it actually captures; direct values such as numbers are still copied.

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

A method value `value.method` keeps a logical copy of the receiver made at creation. Rebinding the original variable or replacing ordinary by-value fields later does not change the saved function value, while reference fields such as Vec still share their contents. A bound move-Self method can be copied and called repeatedly; each call copies the saved receiver and passes that copy to the method. Methods with an `inout Self` receiver cannot be stored as function values.

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

## Control flow

Function-body fragment:

```wz
var total i32 = 0;
for var index i32 = 0; index < 3; index += 1 {
    total += index;
}
let values Vec[i32] = [1, 2, 3];
for value in values {
    total += value;
}
for total < 10 {
    total += 1;
}
if total == 10 {
    total += 1;
} else {
    total = 0;
} // total is 11
```

Conditions must be `bool`, and `&&` and `||` short-circuit; assignment is not an expression. All loops use `for`, supporting the three-clause form, a condition-only form, `for-in`, and an unconditional loop. `break` and `continue` run `defer` actions in the lexical blocks they actually leave.

Custom for-in types implement `std/iter.Iterator[T]`, whose `next(cursor inout Self)` returns `std/option.Option[T]` and does not throw. The loop evaluates the source expression once and advances a private copy of the iterator; reference fields still share their objects. `Some` yields an element, and `None` ends the loop. See the [tutorial example](TUTORIAL.md#custom-iterators).

`switch` has no fallthrough and must cover every possible value; write `default` or `_` when the cases cannot be exhaustive. Each arm has its own scope. A bare identifier pattern always introduces a new binding—even if an outer `const` shares the name, it does not match that constant.

## structs, enums, and methods

Top-level declarations:

```wz
struct Point {
    x i32;
    y i32;
}

impl Point {
    func shifted(self Self, distance i32) Self {
        return Point(x: self.x + distance, y: self.y);
    }
}

enum Reading {
    Missing;
    Value(value i32);
}

func unpack(reading Reading) i32 {
    switch reading {
        case Reading.Missing: return 0;
        case Reading.Value(value): return value;
    }
}
```

Function-body fragment using the declarations above:

```wz
let original = Point(x: 1, y: 2);
var copy = original;
copy.x = 9;                       // original.x is still 1
let shifted = original.shifted(3); // shifted.x is 4
let reading = Reading.Value(value: shifted.x);
let value = unpack(reading);       // 4
```

Struct construction uses named fields and must provide every field; enum variants may carry payloads. Structs and enums copy field by field recursively, and reference-valued fields still follow their own sharing rules.

`Self` refers to the type of the current impl or trait. Calling a method on a value supplies the leading Self parameter automatically, whereas `Point.shifted(original, 3)` passes every argument explicitly. `Point.shifted` produces a function value with an unbound receiver, while `original.shifted` produces a bound method value holding a copy of the receiver made at creation.

## Vec sharing and parameter modes

Function-body fragment:

```wz
let left Vec[i32] = [1, 2, 3];
let right = left;
right[0] = 9;              // left[0] also becomes 9
let slice = left[0:2];
slice[0] = 7;              // left[0] is still 9
```

Assigning a Vec shares the same container with no implicit deep copy; `let` does not prevent modifying elements. Slicing creates a new Vec with logical copies of the elements, so reference-valued elements can still be shared. Indices must be non-negative; out-of-bounds access panics, and Python-style negative indices are rejected.

`std/vec` provides `empty`, `make`, `len`, `capacity`, `reserve`, `push`, `pop`, `resize`, and `clear`. `pop` returns `Option[T]`; `resize` fills with the given value when growing and truncates when shrinking; `clear` keeps the capacity. Assigning a Vec shares the container, so these calls affect every alias. During iteration, do not push, pop, resize, clear, or reserve; modify only existing elements.

`std/box.Box[T]` is a shared mutable value cell; copying a Box yields the same cell, `box.get` reads it, and `box.set` replaces it.

`Weak[T]` is a built-in weak reference type that does not extend its target's lifetime; `std/weak` provides only `empty`, `downgrade`, and `upgrade`. Valid targets are Vec, function values, `Box[T]`, and reference-counted shared handles; ordinary struct/enum values, string, file/socket descriptors, and foreign Python/C objects cannot be direct targets. `weak.empty[T]()` creates an empty weak reference, `weak.downgrade[T](value)` downgrades an existing strong reference, and `weak.upgrade(w)` returns `option.Option[T]`, yielding `None` once no strong references remain. Weak references can be stored in structs, enums, Vec, and generic values. See the [standard library](STDLIB.md) for the interface.

Top-level declarations:

```wz
func take(values move Vec[i32]) i32 {
    return values[0];
}

func increment(value inout i32) {
    value += 1;
}
```

Function-body fragment using the declarations above:

```wz
let values Vec[i32] = [9];
var answer = take(move values); // values is no longer readable
increment(inout answer);       // answer is 10
```

Ordinary parameters do not consume the caller's value; returning or saving a parameter copies it or retains a shared reference according to its type. `move` transfers the entire owned value, and the original binding is unreadable until reinitialized; an ordinary parameter cannot be moved directly, so create a local copy first.

`inout` accepts a writable `var`, an existing `inout` parameter, its field chain, or a bindable container element—for example `bump(inout obj.nested.value)`, `bump(inout items[0])`, or `replace(inout holder.items)`. Eligible locations include numbers, bool, finite structs/enums containing only trivial values, and pure Woozy owned values: Vec, Slice, string, function values, Weak, dyn trait objects, thread/synchronization runtime handles, and structs/enums containing these fields. The callee can replace them as a whole, by field, or by element, releasing the old value exactly once; `inout` on `Task` and foreign C resources still reports `E_CAPABILITY_UNAVAILABLE`.

Within a single call, distinct named fields or provably different literal indices can be borrowed at the same time. Overlapping paths, whole-container access, dynamic index conflicts, temporary locations that do not belong to a local or parameter, moving out of an `inout` parameter, and capturing an `inout` parameter in a closure are all rejected. Copies of a Vec/Slice handle in another binding, a struct or enum, a function result, or a closure can still point to the same object. While an element `inout` borrow is active, the compiler conservatively rejects other arguments that might carry that container and unknown indirect callees.

Methods do not automatically move or gain writable access. When the first Self parameter is declared `move` or `inout`, value calls are written `(move value).take()` and `(inout value).change()` respectively; type-name calls continue to pass all arguments explicitly.

## throws and defer

Top-level declarations:

```wz
enum ReadError { Missing; }

func read_value(found bool) i32 throws ReadError {
    if !found { throw ReadError.Missing; }
    return 7;
}

func load(found bool) i32 throws ReadError {
    return read_value(found); // error propagates outward automatically
}
```

Function-body fragment using the declarations above:

```wz
var cleaned i32 = 0;
var result i32 = 0;
try {
    defer { cleaned += 1; }
    result = load(false);
} catch error ReadError {
    switch error {
        case ReadError.Missing: result = -1;
    }
} // cleaned is 1, result is -1
```

`throws E` is part of the function type. An uncaught call error must be covered by a compatible `throws` on the current function; no per-call `?` is needed. A custom `Result` enum is ordinary data and does not propagate automatically. The standard library's `std/result.Result[T,E]` is also an ordinary enum; write full type arguments when constructing it, for example `result.Result[i32,i32].Ok(7)`.

`defer` runs when its lexical block exits, with the most recently registered action first, and reads bindings as they stand when it executes. `return value` saves the return value before running cleanup. A defer cannot `return`, `break`, or `continue` out of the deferred block or propagate an error. Panics such as integer division by zero and out-of-bounds access cannot be caught by typed `catch` and are not guaranteed to run language cleanup.

## Generics and traits

Top-level declarations:

```wz
func identity[T](value T) T { return value; }

struct Counter { value i32; }

trait Measure {
    func measure(self Self) i32;
}

impl Measure for Counter {
    func measure(self Self) i32 { return self.value; }
}

func measure[T Measure](value T) i32 {
    return value.measure();
}
```

Function-body fragment using the declarations above:

```wz
let first = identity[i32](7);       // explicit type argument
let second = identity(first);       // infers T as i32
let counter = Counter(value: second);
let measured = measure(counter);    // 7
```

Type parameters use `[T]`, and constraints are written `[T Measure]`; generic types, generic methods, and traits with type arguments also use square brackets. Function bodies are checked against the declared constraints, and code is generated for the concrete types in use.

Traits are nominal constraints and require explicit implementations; having a matching method is not enough. Static dispatch is the default; for runtime polymorphism write `dyn Trait` or `dyn Trait[Args]`, where object-safe methods dispatch dynamically to the concrete implementation and concrete values convert automatically when there is a unique implementation. Associated types, specialization, and non-type or variadic template parameters are not supported. See [generics and traits](SEMANTICS.md#6-泛型和-trait) for type parameter and constraint rules.

## Threads and synchronization

Native programs on Linux x86_64 and macOS arm64 can use `spawn[A](func(A) i64, A)` and `join(Thread) i64` from `std/thread`; both declare `throws os.Error`. Thread functions do not capture local variables, and the entry function handles typed errors itself. Thread values are shared by reference; cleaning up a handle without joining does not cancel the thread, and returning from main ends the whole process.

`std/sync` provides non-recursive Mutex, Condvar, and sequentially consistent AtomicI64. Normal locking does not throw; misuse such as unlocking incorrectly panics. Sharing objects such as Vec between threads is allowed, but the programmer must synchronize reads and writes; atomic ARC only makes the reference count safe. See [threads and synchronization](CONCURRENCY.md).

## Async functions

An `async func` call returns `Task[R,E]`, and the body does not run until `await` or the executor runs it. `await` may appear only in an async function; it waits for a Task and propagates declared errors. An unspawned task starts directly, and the same Task can be awaited repeatedly. `async func main` is a valid entry point. `std/async` provides `sleep`, `readable`/`writable`, `spawn`/`run`, `cancel`, `shutdown`, `finished`, and `join`/`join_all`. The executor is single-threaded and available on the Native backend for Linux x86_64 and macOS arm64. See [async and await](CONCURRENCY.md#async-与-await) for the rules.

## Packages and imports

Each file declares a `package`, and top-level declarations in the same package are visible to one another. `pub` controls cross-package access, and an import alias is valid only in the current file. The two files below form a small project, where `app` is the project source root.

`src/geometry/point.wz` (complete file):

```wz
package geometry;

pub struct Point { pub x i32; pub y i32; }

pub func horizontal(point Point) i32 {
    return point.x;
}
```

`src/main.wz` (complete file):

```wz
package main;

import "app/geometry" as geometry;

func main() i32 {
    let point = geometry.Point(x: 3, y: 4);
    let value = geometry.horizontal(point); // 3
    return 0;
}
```

Modules consist of `.wz` files in directories; `app/geometry` maps to `geometry/` under the source root, and package dependencies cannot form cycles. The first path segment of any other import must be declared in the project manifest. Importing Python libraries also requires a matching binding declaration, and source checking does not execute foreign libraries.

See [BUILD.md](../development/docs/BUILD.md) for running examples. The [tutorial](TUTORIAL.md) explains each step, the [standard library](STDLIB.md) covers APIs and resource rules, and [SEMANTICS.md](SEMANTICS.md) defines numbers, sharing, errors, and cleanup.

This guide's organization draws on the [basics section of the Tour of Go](https://github.com/golang/website/blob/master/_content/tour/basics.article), [Julia Getting Started](https://github.com/JuliaLang/julia/blob/master/doc/src/manual/getting-started.md), and [Effective Go](https://go.dev/doc/effective_go). The language rules in this repository take precedence.
