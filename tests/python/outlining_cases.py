"""Expectations fixed from Source evaluation and cleanup order."""
from pathlib import Path


def loops(body, count=24):
    for n in range(count):
        body = f'for var layer{n} i32=0;layer{n}<1;layer{n}+=1{{' + body + '}'
    return body


def cases():
    fixtures = Path(__file__).parent / 'fixtures'
    yield 'outlined-recursive-replacement', (fixtures /
        'outlining-recursive-replacement.lang').read_text(), dict(main_return=0,
        events=[10, 20] * 3, outlined=True,
        ownership=dict(allocations=27, iterations=3, free_intervals=[2, 7] * 3))
    yield 'outlined-unentered', (fixtures / 'outlining-unentered.lang').read_text(), dict(
        main_return=0, outlined=True)

    # Each defer reads a binding installed at a structural statement's entry:
    # an owned for-in element, an owned switch payload and an owned catch value.
    # The return transports a live Vec after the callee's own local is dropped.
    body = '''let values Vec[Vec[i32]]=[[7]];
for element in values {defer{log+=element[0];event(1);}}
switch Packet.Data(value:[8]) {
case Packet.Data(value): defer{log+=value[0];event(2);}
}
try{throw Packet.Data(value:[9]);}
catch caught Packet {
    switch caught {case Packet.Data(value):defer{log+=value[0];event(3);}}
}
let result Vec[i32]=[log];return result;'''
    source = ('''package app;enum Packet{Data(value Vec[i32]);}
func event(digit i32){}
func build(log inout i32)Vec[i32]{''' + loops(body) + '''return [];}
func main()i32{var log i32=0;let result=build(inout log);
if log!=24 || result[0]!=24{return 1;}return 0;}''')
    yield 'outlined-entry-captures-owned-return', source, dict(main_return=0,
        events=[1, 2, 3], outlined=True, ownership=dict(allocations=5, iterations=1))

    # Expression helpers return the original owning result in the shared
    # Frame. Extra copy/take/full-expression cleanup would leak or invalidate
    # one of these recursive Vec owners before the caller reads the leaf.
    chain = 'Node(value:20,children:[])'
    for n in reversed(range(20)):
        chain = f'Node(value:{n},children:[' + chain + '])'
    leaf = 'chain' + '.children[0]' * 20
    source = ('package app;struct Node{value i32;children Vec[Node];}'
              'func build()Node{' + loops('let chain=' + chain + ';return ' + leaf + ';') +
              'return Node(value:99,children:[]);}'
              'func main()i32{let leaf=build();if leaf.value!=20{return 1;}return 0;}')
    yield 'outlined-owning-expression', source, dict(main_return=0, outlined=True,
        ownership=dict(allocations=21, iterations=0))

    # A failure in an outlined arm must retain the computed owning subject
    # until that arm's defer and pattern binding cleanup have completed.
    body = '''try {
switch Packet.Data(value:[7]) {
case Packet.Data(value):
    defer{if value[0]!=7{log=90;}mark(2);}
    fail();
}
return 8;
}catch error E{log+=1;mark(3);}
return 0;'''
    source = ('''package app;enum E{Bad;}enum Packet{Data(value Vec[i32]);}
func mark(digit i32){}
func fail()throws E{throw E.Bad;}
func exercise(log inout i32)i32{''' + loops(body) + '''return 9;}
func main()i32{var log i32=0;if exercise(inout log)!=0 || log!=1{return 1;}return 0;}''')
    yield 'outlined-computed-switch-error', source, dict(main_return=0, events=[2, 3],
        outlined=True, ownership=dict(allocations=1, iterations=0,
            sequence=[['event', 2], ['free', 0], ['event', 3]]))

    source = ('''package app;func event(digit i32){}
func probe(log inout i32){defer{log+=1;event(9);}''' + loops('''
defer{log+=1;event(8);}log=7;event(1);var zero i32=0;let x=i32(1)/zero;
''') + '''}
func main(){var log i32=0;probe(inout log);}''')
    yield 'outlined-body-panic', source, dict(panic='integer division by zero',
        events=[1], probe_log=7, outlined=True)

    # Right-nested && requires >80 generated indentation levels before
    # outlining. The skipped arm contains an observable, throwing call.
    deep = 'touch(inout count, 2, true)'
    for _ in range(96):
        deep = 'true && (' + deep + ')'
    source = ('''package app;enum E{Bad;}func event(digit i32){}
func touch(count inout i32,digit i32,result bool)bool throws E{
count+=1;event(digit);if digit==9{throw E.Bad;}return result;}
func exercise(count inout i32)bool throws E{
let skipped=false && (''' + deep.replace('2, true', '9, true') + ''');
return (touch(inout count,1,true) && (''' + deep + ''')) || skipped;}
func main()i32 throws E{var count i32=0;
if !exercise(inout count) || count!=2{return 1;}return 0;}''')
    yield 'outlined-short-circuit', source, dict(main_return=0, events=[1, 2], outlined=True)
