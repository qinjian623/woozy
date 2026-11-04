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
}
