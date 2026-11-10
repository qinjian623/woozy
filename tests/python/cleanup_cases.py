"""Additional source programs with independent cleanup expectations."""

PRELUDE = '''
enum E {Bad(code i32);}
enum F {Bad;}
func mark(log inout i64,digit i64) {log=log*10+digit;}
'''

CASES = {
    'return-expression-failure': dict(events=[1, 2, 3, 4], log=1234, source=PRELUDE + '''
func fail(log inout i64) i32 throws E {mark(inout log,1);throw E.Bad(code:7);}
func probe(log inout i64) i32 throws E {
    defer {mark(inout log,2);} return fail(inout log);
}
func main() i32 {
    var log i64=0;
    try {defer {mark(inout log,3);} probe(inout log);}
    catch err E {mark(inout log,4);}
    if log!=1234 {return 1;}return 0;
}'''),
    'throw-expression-failure': dict(events=[1, 2, 3], log=123, source=PRELUDE + '''
func fail(log inout i64) F throws E {mark(inout log,1);throw E.Bad(code:8);}
func probe(log inout i64) throws E {
    defer {mark(inout log,2);}
    try {throw fail(inout log);}catch err F {mark(inout log,9);}
}
func main() i32 {
    var log i64=0;
    try {probe(inout log);}catch err F {mark(inout log,9);}
    catch err E {mark(inout log,3);}
    if log!=123 {return 1;}return 0;
}'''),
    'generic-matching': dict(events=[1, 2, 3, 1, 3, 4], source='''
enum E {Bad(code i32);}enum F {Bad;}
func event(digit i64) {}
func g[A,B](a A) i32 throws A {
    defer {event(3);}
    try {defer {event(1);}throw a;}
    catch err B {event(2);}
    return 7;
}
func invoke[A,B](a A) throws A {
    let f func(A) i32 throws A=g[A,B];f(a);
}
func main() i32 {
    try {if g[E,E](E.Bad(code:7))!=7 {return 1;}}
    catch err E {event(9);}
    try {invoke[E,F](E.Bad(code:8));}catch err E {event(4);}
    return 0;
}'''),
    'generic-nominal-matching': dict(events=[1, 2, 3, 1, 3, 4], source='''
func event(digit i64) {}
enum Err[T] {Bad(value T);}
func g[A,B](a Err[A]) throws Err[A] {
    defer {event(3);}
    try {defer {event(1);}throw a;}
    catch err Err[B] {event(2);}
}
func main() i32 {
    try {g[i32,i32](Err[i32].Bad(value:7));}catch err Err[i32] {event(9);}
    try {g[i32,i64](Err[i32].Bad(value:8));}catch err Err[i32] {event(4);}
    return 0;
}'''),
    'recursive-failure': dict(events=[1, 2, 3], log=123, source=PRELUDE + '''
func recurse(n i32,log inout i64) i32 throws E {
    defer {mark(inout log,i64(n));}
    if n==1 {throw E.Bad(code:23);}return recurse(n-1,inout log);
}
func main() i32 {
    var log i64=0;
    try {recurse(3,inout log);}catch err E {
        switch err {case E.Bad(code):if code!=23 {return 1;}}
    }
    if log!=123 {return 2;}return 0;
}'''),
    'value-copy-field-return': dict(events=[2, 4], log=24, source=PRELUDE + '''
struct S {x i32;}
func probe(log inout i64) S {
    var x=S(x:4);let old=x;
    defer {x.x=9;mark(inout log,i64(old.x));}
    {defer {mark(inout log,2);}return x;}
}
func main() i32 {
    var log i64=0;let value=probe(inout log);
    if value.x!=4 || log!=24 {return 1;}return 0;
}'''),
    'continue-step-failure': dict(events=[1, 2, 3], log=123, source=PRELUDE + '''
func step(log inout i64) i32 throws E {mark(inout log,2);throw E.Bad(code:5);}
func main() i32 {
    var log i64=0;
    try {
        for var i i32=0;i<1;i=step(inout log) {
            defer {mark(inout log,1);}continue;
        }
    }catch err E {mark(inout log,3);}
    if log!=123 {return 1;}return 0;
}'''),
    'short-circuit-errors': dict(events=[3], log=3, source=PRELUDE + '''
func fail(log inout i64) bool throws E {mark(inout log,9);throw E.Bad(code:0);}
func main() i32 {
    var log i64=0;
    try {defer {mark(inout log,3);}let a=false&&fail(inout log);
         let b=true||fail(inout log);}
    catch err E {mark(inout log,8);}
    if log!=3 {return 1;}return 0;
}'''),
    'panic-body': dict(events=[1], log=1, panic='integer division by zero', source=PRELUDE + '''
func probe(log inout i64) throws E {
    defer {mark(inout log,9);}
    {defer {mark(inout log,8);}mark(inout log,1);var z i32=0;let x=i32(1)/z;}
}
func main() i32 {
    var log i64=0;try {probe(inout log);}catch err E {return 0;}return 2;
}'''),
    'panic-nested-defer': dict(events=[1, 2], log=12, panic='integer division by zero', source=PRELUDE + '''
func probe(log inout i64) throws E {
    defer {mark(inout log,9);}
    {defer {defer {mark(inout log,8);}mark(inout log,2);
            var z i32=0;let x=i32(1)/z;}
     mark(inout log,1);}
}
func main() i32 {
    var log i64=0;try {probe(inout log);}catch err E {return 0;}return 2;
}'''),
    'typed-boundary': dict(events=[2, 3], log=23, source=PRELUDE + '''
func signal() throws E {throw E.Bad(code:19);}
func main() i32 {
    var log i64=0;defer {mark(inout log,3);}
    try {signal();}catch err E {mark(inout log,2);}return 0;
}'''),
    'return7': dict(main_return=7, source='func main() i32 {defer {}return 7;}'),
    'return101': dict(main_return=101, source='func main() i32 throws i32 {return 101;}'),
    'unit-throws': dict(main_return=None, source='func main() throws i32 {defer {}}'),
    'unhandled-i32': dict(typed='i32', source='func main() i32 throws i32 {throw i32(-7);}'),
    'unhandled-generic': dict(typed='app.Err[i32]', source='''
enum Err[T] {Bad(value T);}
func main() throws Err[i32] {throw Err[i32].Bad(value:23);}
'''),
}

CROSS_MODULE = {
    'main.wz': '''package main;import "app/a";import "app/b";
func main() i32 {
    try {a.fail();}catch err b.E {return 9;}
    catch err a.E {switch err {case a.E.Bad(code):return code;}}return 8;
}''',
    'a/a.wz': 'package a;pub enum E {Bad(code i32);}pub func fail() throws E {throw E.Bad(code:7);}',
    'b/b.wz': 'package b;pub enum E {Bad(code i32);}',
}
