"""Independent expectations for the root's two Python T08 findings."""

PRELUDE = 'enum E {Bad;}enum F {Bad;}enum Token[T] {Bad;}'

# Each row preserves the source's parameter/return/error grouping in its
# separately specified key. No HIR IDs or generated text supply expectations.
FUNCTION_TYPES = [
    ('returned-throws', 'func() (func() throws E)',
     'func()->(func()->(Unit) throws(app.E))'),
    ('outer-throws', 'func() (func()) throws E',
     'func()->(func()->(Unit)) throws(app.E)'),
    ('parameter-returned-throws', 'func(func() (func() throws E))',
     'func(func()->(func()->(Unit) throws(app.E)))->(Unit)'),
    ('parameter-outer-throws', 'func(func() (func()) throws E)',
     'func(func()->(func()->(Unit)) throws(app.E))->(Unit)'),
    ('error-returned-throws', 'func() throws (func() (func() throws E))',
     'func()->(Unit) throws(func()->(func()->(Unit) throws(app.E)))'),
    ('error-outer-throws', 'func() throws (func() (func()) throws E)',
     'func()->(Unit) throws(func()->(func()->(Unit)) throws(app.E))'),
    ('mixed', 'func(func() throws E,inout i32) (func() throws F) throws E',
     'func(func()->(Unit) throws(app.E),inout i32)->(func()->(Unit) throws(app.F)) throws(app.E)'),
    ('nested-error', 'func(func() throws E,inout i32) (func()) throws (func() throws F)',
     'func(func()->(Unit) throws(app.E),inout i32)->(func()->(Unit)) throws(func()->(Unit) throws(app.F))'),
    ('ordinary', 'func(i32) i32', 'func(i32)->(i32)'),
    ('inout', 'func(inout i32) i32', 'func(inout i32)->(i32)'),
]


def key_cases(original):
    yield 'original-function-key-collision', original, dict(main_return=0,
        raised_key='app.Err[' + FUNCTION_TYPES[0][2] + ']')
    # Reverse the thrown type and handler order, retaining both actual values.
    source = original.replace('throws Err[func() (func() throws E)]',
                              'throws Err[func() (func()) throws E]')
    source = source.replace('throw Err[func() (func() throws E)].Bad(value:outerPlain)',
                            'throw Err[func() (func()) throws E].Bad(value:outerThrows)')
    source = source.replace('catch wrong Err[func() (func()) throws E]',
                            'catch wrong Err[func() (func() throws E)]')
    source = source.replace('catch right Err[func() (func() throws E)]',
                            'catch right Err[func() (func()) throws E]')
    yield 'reverse-function-key-collision', source, dict(main_return=0,
        raised_key='app.Err[' + FUNCTION_TYPES[1][2] + ']')
    direct = '''package app;enum E {Bad;}
func innerPlain() {}func innerThrows() throws E {}
func outerPlain() (func() throws E) {return innerThrows;}
func outerThrows() (func()) throws E {return innerPlain;}
'''
    for label, thrown, wrong, value, key in (
        ('direct-returned-function-error', FUNCTION_TYPES[0][1], FUNCTION_TYPES[1][1],
         'outerPlain', FUNCTION_TYPES[0][2]),
        ('direct-outer-function-error', FUNCTION_TYPES[1][1], FUNCTION_TYPES[0][1],
         'outerThrows', FUNCTION_TYPES[1][2])):
        source = direct + f'''func fail() throws ({thrown}) {{throw {value};}}
func main() i32 {{try {{fail();}}catch wrong {wrong} {{return 9;}}
catch right {thrown} {{return 0;}}return 7;}}'''
        yield label, source, dict(main_return=0, raised_key=key)
    for ident, source_type, key in FUNCTION_TYPES:
        for shifted in (False, True):
            noise = 'struct Noise {a u64;}enum Earlier {A(value Noise);}' if shifted else ''
            source = ('package app;' + noise + PRELUDE +
                      f'func main() throws Token[{source_type}] {{throw Token[{source_type}].Bad;}}')
            yield 'key-' + ident + ('-shifted' if shifted else ''), source, dict(
                typed='app.Token[' + key + ']')
    # Collisions hidden in a parameter or an error type also select the wrong
    # catch in the rejected emitter, independent of its entrypoint diagnostics.
    for left, right in ((2, 3), (4, 5)):
        a, b = FUNCTION_TYPES[left], FUNCTION_TYPES[right]
        source = ('package app;' + PRELUDE + f'''func main() i32 {{
try {{throw Token[{a[1]}].Bad;}}catch wrong Token[{b[1]}] {{return 9;}}
catch right Token[{a[1]}] {{return 0;}}return 7;}}''')
        yield 'catch-' + a[0], source, dict(main_return=0, raised_key='app.Token[' + a[2] + ']')


def flat_cases():
    for count in (0, 1, 8, 9, 10, 11, 20, 21, 22, 64, 256):
        source = ('package app;func probe(log inout i32) i32 {' +
                  'defer{log+=1;}' * count + 'return log;}' +
                  f'''func main() i32 {{var log i32=0;let r=probe(inout log);
if r!=0 || log!={count} {{return 1;}}return 0;}}''')
        yield 'flat-writing-' + str(count), source, dict(main_return=0,
            probe_return=0, probe_log=count)
    yield 'root-21-empty', ('package app;func main() i32 {' +
                            'defer{}' * 21 + 'return 0;}\n'), dict(main_return=0)
    yield 'root-21-writing-return', ('package app;func main() i32 {var x i32=0;' +
        'defer{x+=1;}' * 21 + 'return x;}\n'), dict(main_return=0, main_cell=21)
    event = 'package app;enum E {Bad;}func event(digit i64) {}'
    yield 'flat-lifo-21', (event + 'func main() i32 {' +
        ''.join(f'defer{{event({n});}}' for n in range(1, 22)) +
        'return 0;}'), dict(main_return=0, events=list(range(21, 0, -1)))
    yield 'mixed-continue-21', (event + '''func main() i32 {var count i32=0;
for var i i64=0;i<2;i+=1 {defer{event(i);}''' + 'defer{count+=1;}' * 21 +
        '''try {defer{count+=1;}if i==0 {continue;}}catch err E {return 9;}}
if count!=44 {return 1;}return 0;}'''), dict(main_return=0, events=[0, 1], main_cell=44)
    yield 'partial-registration-21', (event + '''
func fail() throws E {throw E.Bad;}
func probe(log inout i32) throws E {''' + 'defer{log+=1;}' * 10 +
        'fail();' + 'defer{log+=1;}' * 11 + '''}
func main() i32 {var log i32=0;try {probe(inout log);}catch err E {}
if log!=10 {return 1;}return 0;}'''), dict(main_return=0, probe_log=10, probe_typed='app.E')
    yield 'flat-defer-panic', (event + '''
func probe(log inout i32) throws E {defer{event(8);}
{''' + 'defer{event(9);}' * 21 + '''
defer{log+=1;event(2);var zero i32=0;let x=i32(1)/zero;}
event(1);}}
func main() i32 {var log i32=0;try {probe(inout log);}catch err E {return 9;}return 0;}
'''), dict(panic='integer division by zero', events=[1, 2], probe_log=1)
    yield 'flat-body-panic', (event + '''
func probe(log inout i32) throws E {''' + 'defer{event(9);}' * 21 + '''
log=7;event(1);var zero i32=0;let x=i32(1)/zero;}
func main() i32 {var log i32=0;try {probe(inout log);}catch err E {return 9;}return 0;}
'''), dict(panic='integer division by zero', events=[1], probe_log=7)
    # These four formerly refused sources fit CPython's real control stack.
    for count in (2, 3, 4, 5):
        body = 'return 0;'
        for _ in range(count):
            body = 'try{defer{}' + body + '}catch err E {return 9;}'
        yield 'nested-try-defer-' + str(count), ('package app;enum E {Bad;}' +
            'func main() i32 {' + body + 'return 7;}'), dict(main_return=0)
    # Ten generated loops reach the target stack limit; eleven exceed it.
    for count in (4, 5, 10, 11):
        body = ''
        for n in range(count):
            body = f'for var i{n} i32=0;i{n}<1;i{n}+=1 {{' + body + '}'
        yield 'nested-loops-' + str(count), 'package app;func main() {' + body + '}', (
            dict(main_return=None) if count <= 10 else dict(unsupported=True))
    for count in (2, 3):
        body = ''
        for _ in range(count):
            body = 'try{defer{}' + body + '}catch err E {}'
        body = 'for var i i32=0;i<1;i+=1 {' + body + '}'
        yield 'nested-loop-try-defer-' + str(count), ('package app;enum E {Bad;}' +
            'func main() {' + body + '}'), dict(main_return=None)
    # Exercise returns, typed throws, break and continue across a finally at
    # the real stack limit; one further loop must be refused before output.
    for count in (9, 10):
        body = ('defer{log+=1;event(1);}'
                'if mode==0{return 7;}if mode==1{throw E.Bad;}'
                'if mode==2{break;}continue;')
        for n in range(count):
            body = f'for var i{n} i32=0;i{n}<1;i{n}+=1 {{' + body + '}'
        source = ('package app;enum E{Bad;}func event(digit i32){}'
                  'func control(log inout i32,mode i32)i32 throws E{' + body +
                  'return 9;}'
                  'func main()i32 throws E{var log i32=0;'
                  'if control(inout log,0)!=7 || log!=1{return 1;}log=0;'
                  'try{let x=control(inout log,1);return 2;}'
                  'catch err E{if log!=1{return 3;}}log=0;'
                  'if control(inout log,2)!=9 || log!=1{return 4;}log=0;'
                  'if control(inout log,3)!=9 || log!=1{return 5;}return 0;}')
        yield 'nested-exit-finally-' + str(count), source, (
            dict(main_return=0, events=[1, 1, 1, 1]) if count == 9
            else dict(unsupported=True))
