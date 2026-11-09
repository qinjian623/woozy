"""Independent T09 sources. Original ownership fixtures remain unchanged."""
CASES = {
    'reverse-vec': ('''func main() i32 {let v Vec[Vec[i32]] = [[11], [22]]; return 0;}''', {'frees': [1, 0, 2]}),
    'reverse-struct': ('''struct S {a Vec[i32]; b Vec[i32];}
func main() i32 {let v = S(b: [22], a: [11]); return 0;}''', {'frees': [0, 1]}),
    'reverse-enum': ('''enum E {A(a Vec[i32], b Vec[i32]); B;}
func main() i32 {let v = E.A(b: [22], a: [11]); return 0;}''', {'frees': [0, 1]}),
    'self-assign': ('''func main() i32 {var v Vec[Vec[i32]] = [[1]]; v = v; v[0] = v[0];
if v[0][0] != 1 {return 1;} return 0;}''', {}),
    'owned-field-replace': ('''struct S {a Vec[i32]; b Vec[i32];}
func main() i32 {var v = S(a: [1], b: [2]); v.a = v.b;
v.a[0] = 7; if v.b[0] != 7 {return 1;} return 0;}''', {}),
    'owned-pattern': ('''enum E {A(a Vec[i32], b Vec[i32]); B;}
func main() i32 {let v = E.A(a: [1], b: [2]); switch v {case E.A(a, b):
a[0] = 9; if a[0] != 9 || b[0] != 2 {return 1;} case E.B: return 2;} return 0;}''', {}),
    'generic-owned': ('''struct Box[T] {value T;}
func keep[T](x T) T {let saved = x; return saved;}
func main() i32 {let a = Box[Vec[i32]](value: [3]); let b = keep(a); b.value[0] = 7;
if a.value[0] != 7 {return 1;} return 0;}''', {}),
    'computed-move': ('''func make() Vec[i32] {return [8];}
func take(v move Vec[i32]) i32 {return v[0];}
func main() i32 {if take(move make()) != 8 {return 1;} return 0;}''', {}),
    'owned-error-caught': ('''enum E {Bad(v Vec[i32]);}
func fail() Vec[i32] throws E {let v Vec[i32] = [7]; throw E.Bad(v: v);}
func main() i32 {try {let v = fail(); return 1;} catch e E {
switch e {case E.Bad(v): if v[0] != 7 {return 2;}}} return 0;}''', {}),
    'owned-error-escape': ('''enum E {Bad(v Vec[i32]);}
func main() i32 throws E {throw E.Bad(v: [7]);}''', {'typed': True}),
    'partial-struct': ('''enum E {Bad;}
struct S {a Vec[i32]; b Vec[i32]; c Vec[i32];}
func bad() Vec[i32] throws E {throw E.Bad;}
func main() i32 {try {let s = S(c: [3], a: [1], b: bad()); return 1;}
catch e E {} return 0;}''', {'frees': [1, 0]}),
    'partial-vec': ('''enum E {Bad;}
func bad() Vec[i32] throws E {throw E.Bad;}
func main() i32 {try {let v Vec[Vec[i32]] = [[1], [2], bad()]; return 1;}
catch e E {} return 0;}''', {'frees': [1, 0]}),
    'rhs-error-target': ('''enum E {Bad;}
func bad() Vec[i32] throws E {throw E.Bad;}
func main() i32 {var v Vec[Vec[i32]] = [[4]]; try {v[0] = bad(); return 1;}
catch e E {} if v[0][0] != 4 {return 2;} return 0;}''', {}),
    'loop-control': ('''enum E {Bad;}
func scan(mode i32) Vec[i32] throws E {let v Vec[Vec[i32]] = [[1], [2], [3]];
for x in v {if x[0] == 1 {continue;} if mode == 0 {break;}
if mode == 1 {return x;} throw E.Bad;} return [8];}
func main() i32 throws E {let a = scan(0); let b = scan(1); if a[0] != 8 || b[0] != 2 {return 1;}
try {let c = scan(2); return 2;} catch e E {} return 0;}''', {}),
    'multi-defer-output': ('''enum E {Bad(v Vec[i32]);}
func mark(log inout i64, n i64) {log = log * 10 + n;}
func work(log inout i64, fail bool) Vec[i32] throws E {let a Vec[i32] = [1];
defer {mark(inout log, 4);} {let b Vec[i32] = [2]; defer {mark(inout log, 3);}
if fail {throw E.Bad(v: b);} return b;}}
func main() i32 throws E {var log i64 = 0; let v = work(inout log, false);
try {let w = work(inout log, true); return 1;} catch e E {}
if v[0] != 2 || log != 3434 {return 2;} return 0;}''', {'events': [3, 4, 3, 4]}),
    'negative-index': ('''func main() i32 {let v Vec[i32] = [1]; var i i64 = -1; return v[i];}''', {'panic': True}),
    'large-index': ('''func main() i32 {let v Vec[i32] = [1]; var i u64 = 18446744073709551615; return v[i];}''', {'panic': True}),
    'bad-slice': ('''func main() i32 {let v Vec[i32] = [1, 2]; let w = v[2:1]; return 0;}''', {'panic': True}),
    'negative-slice': ('''func main() i32 {let v Vec[i32] = [1]; var i i64 = -1; let w = v[i:]; return 0;}''', {'panic': True}),
    'partial-enum': ('''enum E {Bad;} enum Box {A(a Vec[i32], b Vec[i32], c Vec[i32]);}
func bad() Vec[i32] throws E {throw E.Bad;}
func main() i32 {try {let v = Box.A(c: [3], a: [1], b: bad()); return 1;}
catch e E {} return 0;}''', {'frees': [1, 0]}),
    'iteration-owned-element': ('''func main() i32 {let v Vec[Vec[i32]] = [[1], [2]];
var i i32 = 0; for x in v {v[i] = [9]; if x[0] != i + 1 {return 1;} i += 1;}
if i != 2 {return 2;} return 0;}''', {}),
    'collection-once': ('''func make(calls inout i32) Vec[i32] {calls += 1; return [1, 2];}
func main() i32 {var calls i32 = 0; var sum i32 = 0;
for x in make(inout calls) {sum += x;} if calls != 1 || sum != 3 {return 1;} return 0;}''', {}),
    'condition-full-expression': ('''func test(v Vec[i32]) bool {return v[0] == 1;}
func mark(log inout i64, n i64) {log = n;}
func main() i32 {var log i64 = 0; if test([1]) {mark(inout log, 7);}
if log != 7 {return 1;} return 0;}''', {'sequence': [['free', 0], ['event', 7]]}),
    'defer-infinite-build-only': ('''func main() i32 {let v Vec[i32] = [1]; defer {for ;; {}} return v[0];}''', {'build_only': True}),
    'ordinary-param-readonly-positive': ('''func bump(slot inout i32) {slot += 1;}
func edit(values Vec[i32]) {values[0] = 7; let alias = values; alias[1] = 8;}
func main() i32 {var local i32 = 1; local = local + 2; local += 3;
if local != 6 {return 1;} bump(inout local); if local != 7 {return 2;}
let values Vec[i32] = [1, 2]; edit(values);
if values[0] != 7 || values[1] != 8 {return 3;} return 0;}''', {}),
    'owned-callbacks': ('struct S {v Vec[i32];}\nfunc read_vec(v Vec[i32]) i32 {let saved=v;return saved[0];}\nfunc read_struct(s S) i32 {let saved=s;return saved.v[0];}\nfunc apply_vec(cb func(Vec[i32]) i32,v Vec[i32]) i32 {return cb(v);}\nfunc apply_struct(cb func(S) i32,s S) i32 {return cb(s);}\nfunc main() i32 {if apply_vec(read_vec,[5])!=5 || apply_struct(read_struct,S(v:[6]))!=6 {return 1;}return 0;}\n', {}),
    'computed-owning-once-defer-order': ('''enum Data { Empty; Items(value Vec[i32]); }
func mark(log inout i64, digit i64) { log = log * 10 + digit; }
func make(log inout i64, digit i64, value i32) Data {
    log = log * 10 + digit;
    return Data.Items(value: [value]);
}
func probe(log inout i64) i32 {
    switch make(inout log, 1, 7) {
    case Data.Empty: return 1;
    case Data.Items(items):
        defer { mark(inout log, 2); }
        if items[0] != 7 { return 2; }
    }
    mark(inout log, 3);
    return 0;
}
func main() i32 {
    var log i64 = 0;
    if probe(inout log) != 0 { return 1; }
    if log != 123 { return 2; }
    return 0;
}''', {'sequence': [['event', 2], ['free', 0], ['event', 3]]}),
    'computed-owning-construction-move-empty': ('''enum Data { Empty; Items(value Vec[i32]); }
func main() i32 {
    var x = Data.Items(value: [9]);
    switch Data.Items(value: [7]) {
    case Data.Empty: return 1;
    case Data.Items(items):
        if items[0] != 7 { return 2; }
    }
    switch move x {
    case Data.Empty: return 3;
    case Data.Items(items):
        if items[0] != 9 { return 4; }
    }
    switch Data.Empty {
    case Data.Empty:
    case Data.Items(items): return 5;
    }
    return 0;
}''', {'frees': [1, 0]}),
    'computed-owning-loop-break-continue': ('''enum Data { Empty; Items(value Vec[i32]); }
func make(calls inout i32, value i32) Data {
    calls = calls + 1;
    return Data.Items(value: [value]);
}
func main() i32 {
    var calls i32 = 0;
    var log i64 = 0;
    var sum i32 = 0;
    var after i32 = 0;
    for var i i32 = 0; i < 3; i += 1 {
        switch make(inout calls, i) {
        case Data.Empty: return 1;
        case Data.Items(items):
            defer { sum += items[0]; log = log * 10 + i64(i + 1); }
            if i == 0 { continue; }
            if i == 1 { break; }
            sum += 100;
        }
        after += 1;
    }
    if calls != 3 || log != 123 || sum != 103 || after != 2 { return 2; }
    return 0;
}''', {'frees': [0, 1, 2]}),
    'computed-owning-catch-throw-nested': ('''enum Data { Empty; Items(value Vec[i32]); }
enum Failure { Bad(value Vec[i32]); }
func mark(log inout i64, digit i64) { log = log * 10 + digit; }
func fail() i32 throws Failure { throw Failure.Bad(value: [3]); }
func probe(log inout i64) i32 throws Failure {
    switch Data.Items(value: [7]) {
    case Data.Empty: return 1;
    case Data.Items(items):
        mark(inout log, 1);
        try { let n = fail(); if n != 0 { return 2; } }
        catch e Failure {
            mark(inout log, 2);
            switch e {
            case Failure.Bad(p): if p[0] != 3 { return 3; }
            }
            if items[0] != 7 { return 4; }
        }
        switch Data.Items(value: [9]) {
        case Data.Empty: return 5;
        case Data.Items(inner):
            mark(inout log, 5);
            if inner[0] != 9 { return 6; }
        }
        mark(inout log, 6);
        if items[0] != 7 { return 7; }
        throw Failure.Bad(value: items);
    }
}
func main() i32 {
    var log i64 = 0;
    try { probe(inout log); return 8; }
    catch e Failure {
        switch e {
        case Failure.Bad(items):
            mark(inout log, 7);
            if items[0] != 7 { return 9; }
        }
    }
    if log != 12567 { return 10; }
    return 0;
}''', {'sequence': [['event', 1], ['event', 2], ['free', 1], ['event', 5], ['free', 2], ['event', 6], ['event', 7], ['free', 0]]}),
    'computed-owning-return-payload': ('''enum Data { Empty; Items(value Vec[i32]); }
func mark(log inout i64, digit i64) { log = log * 10 + digit; }
func make(log inout i64, digit i64, value i32) Data {
    log = log * 10 + digit;
    return Data.Items(value: [value]);
}
func take(log inout i64) Vec[i32] {
    switch make(inout log, 1, 7) {
    case Data.Empty: return [0];
    case Data.Items(items):
        defer { mark(inout log, 2); }
        return items;
    }
}
func probe(log inout i64) i32 {
    let result = take(inout log);
    if log != 12 || result[0] != 7 { return 1; }
    result[0] = 8;
    mark(inout log, 3);
    return 0;
}
func main() i32 {
    var log i64 = 0;
    if probe(inout log) != 0 { return 2; }
    if log != 123 { return 3; }
    return 0;
}''', {'sequence': [['event', 2], ['event', 3], ['free', 0]]}),
    'computed-owning-error-escapes-arm': ('''enum Data { Empty; Items(value Vec[i32]); }
enum Failure { Bad; }
func mark(log inout i64, digit i64) { log = log * 10 + digit; }
func fail() i32 throws Failure { throw Failure.Bad; }
func probe(log inout i64) i32 throws Failure {
    switch Data.Items(value: [7]) {
    case Data.Empty: return 1;
    case Data.Items(items):
        defer { mark(inout log, 2); }
        let n = fail();
        mark(inout log, 3);
        return n;
    }
}
func main() i32 {
    var log i64 = 0;
    try { probe(inout log); return 1; }
    catch e Failure { mark(inout log, 4); }
    if log != 24 { return 2; }
    return 0;
}''', {'sequence': [['event', 2], ['free', 0], ['event', 4]]}),
    'computed-owning-recursive-pattern': ('''enum Inner { None; Some(value Vec[i32]); }
enum Outer { Empty; Wrap(item Inner); Pair(inner Inner, tag i32); }
func mark(log inout i64, digit i64) { log = log * 10 + digit; }
func make(log inout i64) Outer {
    log = log * 10 + 1;
    return Outer.Wrap(item: Inner.Some(value: [7]));
}
func probe(log inout i64) i32 {
    switch make(inout log) {
    case Outer.Empty: return 1;
    case Outer.Wrap(Inner.None): return 2;
    case Outer.Wrap(Inner.Some(items)):
        mark(inout log, 2);
        if items[0] != 7 { return 3; }
    case Outer.Pair(inner, tag): return 4;
    }
    mark(inout log, 3);
    return 0;
}
func main() i32 {
    var log i64 = 0;
    if probe(inout log) != 0 { return 1; }
    if log != 123 { return 2; }
    return 0;
}''', {'sequence': [['event', 2], ['free', 0], ['event', 3]]}),
    'indexed-field-compound': ('''struct Pair {x i32; y i32;}
func mutate(values Vec[Pair], n i32) i32 {values[0].x = 100; values[0].y = 9; return n;}
func main() i32 {var values Vec[Pair] = [Pair(x: 1, y: 2)]; values[0].x += mutate(values, 5);
if values[0].x != 6 {return 1;} if values[0].y != 9 {return 2;} return 0;}''', {}),
    'indexed-field-order': ('''struct Pair {x i32; y i32;}
func mark(log inout i64, digit i64) i64 {log = log * 10 + digit; return digit;}
func make(log inout i64) Vec[Pair] {mark(inout log, 1); return [Pair(x: 2, y: 3)];}
func choose(log inout i64) i64 {mark(inout log, 2); return 0;}
func value(log inout i64) i32 {mark(inout log, 3); return 7;}
func main() i32 {var log i64 = 0; make(inout log)[choose(inout log)].x = value(inout log);
if log != 123 {return 1;} return 0;}''', {}),
    'indexed-field-owned': ('''struct Box {items Vec[i32]; other Vec[i32];}
func main() i32 {var values Vec[Box] = [Box(items: [1], other: [2])];
let old = values[0].items; values[0].items = values[0].items;
if old[0] != 1 {return 1;} if values[0].items[0] != 1 {return 2;}
if values[0].other[0] != 2 {return 3;} values[0].items = [3, 4];
if old[0] != 1 {return 4;} if values[0].items[1] != 4 {return 5;}
if values[0].other[0] != 2 {return 6;} return 0;}''', {}),
    'indexed-field-owned-sibling': ('''struct Box {x i32; items Vec[i32]; other Vec[i32];}
func main() i32 {var values Vec[Box] = [Box(x: 1, items: [2], other: [3])];
values[0].x = 7; if values[0].x != 7 {return 1;} if values[0].items[0] != 2 {return 2;}
if values[0].other[0] != 3 {return 3;} values[0].items[0] = 8;
if values[0].other[0] != 3 {return 4;} return 0;}''', {}),
    'indexed-field-nested': ('''struct Inner {x i32; y i32;}
struct Box {inner Vec[Inner]; tag i32;}
func main() i32 {let outer Vec[Box] = [Box(inner: [Inner(x: 1, y: 2)], tag: 3)];
outer[0].inner[0].x = 7; if outer[0].inner[0].x != 7 {return 1;}
if outer[0].inner[0].y != 2 {return 2;} if outer[0].tag != 3 {return 3;}
let temp Vec[Box] = [Box(inner: [Inner(x: 4, y: 5)], tag: 6)];
temp[0].inner[0].y = 8; if temp[0].inner[0].x != 4 {return 4;}
if temp[0].inner[0].y != 8 {return 5;} if temp[0].tag != 6 {return 6;} return 0;}''', {}),
    'indexed-field-move-binding': ('''struct Pair {x i32; y i32;}
func consume(values move Vec[Pair]) i32 {return values[0].y;}
func main() i32 {let values Vec[Pair] = [Pair(x: 1, y: 2)]; let alias = values;
values[0].x = consume(move values); if alias[0].x != 2 {return 1;}
if alias[0].y != 2 {return 2;} return 0;}''', {}),
    'indexed-field-throw-defer': ('''enum E {Bad;}
struct Pair {x i32; y i32;}
func fail(log inout i64) i32 throws E {log = log * 10 + 3; throw E.Bad;}
func probe(log inout i64) i32 throws E {var values Vec[Pair] = [Pair(x: 1, y: 2)];
{defer {log = log * 10 + 9;} try {values[0].x = fail(inout log);} catch e E {log = log * 10 + 4;}}
if log != 349 {return 1;} if values[0].x != 1 {return 2;} return 0;}
func main() i32 throws E {var log i64 = 0; if probe(inout log) != 0 {return 1;} return 0;}''', {}),
    'indexed-field-assign-source': ('''struct P {x i32; y i32;}
func main() i32 {let v Vec[P] = [P(x: 1, y: 2)]; v[0].x = 3; v[0].y = 4;
if v[0].x != 3 {return 1;} if v[0].y != 4 {return 2;} return 0;}''', {}),
    'indexed-field-lifetime': ('''struct P {x i32; y i32;}
func consume(v move Vec[P]) i32 {return v[0].y;}
func main() i32 {let v Vec[P] = [P(x: 1, y: 2)]; v[0].x = consume(move v); return 0;}''', {}),
    'indexed-field-nested-fields': ('''struct Inner {x i32; y i32;}
struct Box {inner Inner; tag i32;}
func main() i32 {let values Vec[Box] = [Box(inner: Inner(x: 1, y: 2), tag: 3)];
values[0].inner.x = 7; if values[0].inner.x != 7 {return 1;}
if values[0].inner.y != 2 {return 2;} if values[0].tag != 3 {return 3;} return 0;}''', {}),
    'indexed-field-throw-edges': ('''enum E {Bad;}
struct P {x i32; y i32;}
func maybe(flag bool, v move Vec[P]) i32 throws E {if flag {throw E.Bad;} return v[0].y;}
func main() i32 throws E {let v Vec[P] = [P(x: 1, y: 2)]; let alias = v;
v[0].x = maybe(false, move v); if alias[0].x != 2 {return 1;}
let w Vec[P] = [P(x: 3, y: 4)]; let w_alias = w;
try {w[0].y = maybe(true, move w); return 2;} catch e E {}
if w_alias[0].y != 4 {return 3;} return 0;}''', {}),
    'indexed-field-deep-fields': ('''struct T0{x i32;y i32;}
struct T1{child T0;}
struct T2{child T1;}
struct T3{child T2;}
struct T4{child T3;}
struct T5{child T4;}
struct T6{child T5;}
struct T7{child T6;}
struct T8{child T7;}
struct T9{child T8;}
struct T10{child T9;}
struct T11{child T10;}
struct T12{child T11;}
func main()i32{let v Vec[T12]=[T12(child:T11(child:T10(child:T9(child:T8(child:T7(child:T6(child:T5(child:T4(child:T3(child:T2(child:T1(child:T0(x:1,y:2)))))))))))))];v[0].child.child.child.child.child.child.child.child.child.child.child.child.x=3;return v[0].child.child.child.child.child.child.child.child.child.child.child.child.x-3;}''', {}),
}
