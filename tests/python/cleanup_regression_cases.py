"""Independent expectations for the root's two Python T08 findings."""

from pathlib import Path

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
    yield 'mixed-loop-cleanup', (Path(__file__).parent /
        'fixtures/mixed-loop-cleanup.lang').read_text(), dict(main_return=0,
        events=[1, 2, 0, 2, 3, 4, 5, 6, 7, 8, 9] +
               [1, 2, 1, 2, 3, 4, 5, 6, 7, 8, 9] +
               [1, 2, 2, 3, 4, 5, 6, 0, 6, 7, 8, 9] +
               [1, 2, 2, 3, 4, 5, 6, 0, 6, 7, 8, 7, 0, 9])
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
    # Plain loops now use one control block each, without a continue handler.
    for count in (4, 5, 10, 11, 21, 22):
        body = ''
        for n in range(count):
            body = f'for var i{n} i32=0;i{n}<1;i{n}+=1 {{' + body + '}'
        yield 'nested-loops-' + str(count), 'package app;func main() {' + body + '}', (
            dict(main_return=None) if count <= 21 else dict(unsupported=True))
    for count in (2, 3):
        body = ''
        for _ in range(count):
            body = 'try{defer{}' + body + '}catch err E {}'
        body = 'for var i i32=0;i<1;i+=1 {' + body + '}'
        yield 'nested-loop-try-defer-' + str(count), ('package app;enum E {Bad;}' +
            'func main() {' + body + '}'), dict(main_return=None)
    # A cleanup guard uses one control block in addition to the loops.
    for count in (9, 10, 18, 19, 20, 21):
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
            dict(main_return=0, events=[1, 1, 1, 1]) if count <= 20
            else dict(unsupported=True))
    yield 'for-in-empty-single', ('''package app;
func event(digit i32) {}
func main() i32 {
    let empty Vec[i32] = [];
    for value in empty { event(9); return 1; }
    let single Vec[i32] = [7];
    var log i32 = 0;
    for value in single {
        defer { log = log * 10 + 2; event(2); }
        if value != 7 { return 2; }
        log = log * 10 + 1; event(1);
        continue;
    }
    if log != 12 { return 3; }
    return 0;
}'''), dict(main_return=0, events=[1, 2])

    # Fixed independently from source order: init=0, condition=1, body
    # defer=2, step=3, function defer=9, caller/catch=8. The first body
    # iteration continues. A later break/return/throw must skip the next step;
    # an error in step/condition must not rerun the preceding body cleanup.
    yield 'loop-head-step-order', '''package app;
enum E { Bad; }

func event(digit i64) {}
func record(log inout i64, digit i64) {
    log = log * 10 + digit;
    event(digit);
}
func initialize(mode i32, log inout i64) i32 {
    record(inout log, 0);
    if mode == 0 { return 2; }
    return 0;
}
func condition(i i32, mode i32, limit i32, log inout i64) bool throws E {
    record(inout log, 1);
    if mode == 6 && i == 1 { throw E.Bad; }
    return i < limit;
}
func advance(i inout i32, mode i32, log inout i64) throws E {
    record(inout log, 3);
    if mode == 5 { throw E.Bad; }
    i += 1;
}
func control(mode i32, log inout i64) i32 throws E {
    defer { record(inout log, 9); }
    for var i i32 = initialize(mode, inout log);
        condition(i, mode, 2, inout log);
        advance(inout i, mode, inout log) {
        defer { record(inout log, 2); }
        if i == 0 { continue; }
        if mode == 2 { break; }
        if mode == 3 { return 42; }
        if mode == 4 { throw E.Bad; }
    }
    return 13;
}
func switch_control(log inout i64) i32 throws E {
    defer { record(inout log, 9); }
    for var i i32 = initialize(7, inout log);
        condition(i, 7, 3, inout log);
        advance(inout i, 7, inout log) {
        defer { record(inout log, 2); }
        switch i {
            case 0:
                defer { record(inout log, 5); }
                continue;
            default:
                defer { record(inout log, 5); }
                break;
        }
        record(inout log, 4);
        if i == 2 { break; }
    }
    return 13;
}
func main() i32 throws E {
    var empty_log i64 = 0;
    let empty = control(0, inout empty_log);
    record(inout empty_log, 8);
    if empty != 13 || empty_log != 198 { return 1; }

    var normal_log i64 = 0;
    let normal = control(1, inout normal_log);
    record(inout normal_log, 8);
    if normal != 13 || normal_log != 123123198 { return 2; }

    var break_log i64 = 0;
    let broken = control(2, inout break_log);
    record(inout break_log, 8);
    if broken != 13 || break_log != 1231298 { return 3; }

    var return_log i64 = 0;
    let returned = control(3, inout return_log);
    record(inout return_log, 8);
    if returned != 42 || return_log != 1231298 { return 4; }

    var body_error_log i64 = 0;
    try {
        let unexpected = control(4, inout body_error_log);
        return 5;
    } catch body_error E {
        record(inout body_error_log, 8);
    }
    if body_error_log != 1231298 { return 6; }

    var step_error_log i64 = 0;
    try {
        let unexpected = control(5, inout step_error_log);
        return 7;
    } catch step_error E {
        record(inout step_error_log, 8);
    }
    if step_error_log != 12398 { return 8; }

    var condition_error_log i64 = 0;
    try {
        let unexpected = control(6, inout condition_error_log);
        return 9;
    } catch condition_error E {
        record(inout condition_error_log, 8);
    }
    if condition_error_log != 123198 { return 10; }

    var switch_log i64 = 0;
    let switched = switch_control(inout switch_log);
    record(inout switch_log, 8);
    if switched != 13 || switch_log != 152315423154298 { return 11; }
    return 0;
}
''', dict(main_return=0, events=(
        [0, 1, 9, 8] +
        [0, 1, 2, 3, 1, 2, 3, 1, 9, 8] +
        [0, 1, 2, 3, 1, 2, 9, 8] +
        [0, 1, 2, 3, 1, 2, 9, 8] +
        [0, 1, 2, 3, 1, 2, 9, 8] +
        [0, 1, 2, 3, 9, 8] +
        [0, 1, 2, 3, 1, 9, 8] +
        [0, 1, 5, 2, 3, 1, 5, 4, 2, 3, 1, 5, 4, 2, 9, 8]))
