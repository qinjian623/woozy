"""Small source programs with expectations from exact powers of two/control flow."""


def value(typ, bits):
    return dict(type=typ, bits=bits)


def supplemental():
    yield 'old-string-float', 'func main() {let x f64=1.0;let y=x+x;}', {}, []
    yield 'evaluation', '''
struct P {x f32;wide f64;op func(f32,f32) f32;}
func id[T](x T) T {return x;}
func add(a f32,b f32) f32 {return a+b;}
func mark(log inout i32,digit i32) f32 {log=log*10+digit;if digit==1{return 1.0;}return 2.0;}
func tick(log inout i32) bool {log+=100;return true;}
func probe() i32 {
 var log i32=0;var p=P(x:1.0,wide:2.0,op:add);let old=id(p);
 let f=p.op;let r=f(mark(inout log,2),mark(inout log,1));
 if r!=3.0 || log!=21 {return 1;}
 p.x+=mark(inout log,1);p.x-=1.0;p.x*=2.0;p.x/=2.0;
 p.wide+=1.0;p.wide-=1.0;p.wide*=2.0;p.wide/=2.0;
 if old.x!=1.0 || p.x!=1.0 || p.wide!=2.0 || log!=211 {return 2;}
 if false && tick(inout log) {return 3;}
 if true || tick(inout log) {} else {return 4;}
 if log!=211 {return 5;}
 let named=add(b:mark(inout log,2),a:mark(inout log,1));
 if named!=3.0 || log!=21121 {return 6;}
 let lam=func(x f32) f32 {return -x*x;};
 if id(lam)(2.0)!=-4.0 {return 7;}
 return 0;
}
func main() i32 {return probe();}
''', {}, [dict(function='probe', arguments=[], result=dict(type='i32', value=0))]
    for width in (32, 64):
        ft = f'f{width}'
        one = '3f800000' if width == 32 else '3ff0000000000000'
        two = '40000000' if width == 32 else '4000000000000000'
        half = '3f000000' if width == 32 else '3fe0000000000000'
        inf = '7f800000' if width == 32 else '7ff0000000000000'
        args = [dict(type='i32', value=0, cell=True), dict(type=ft, bits=one, cell=True)]
        prelude = f'enum Token[T] {{Bad(value T);}}\n'
        ret = f'''
func probe(log inout i32,x inout {ft}) {ft} {{
 defer {{log=log*10+2;x*=2.0;}}
 log=1;return x/2.0;
}}
func main() i32 {{var log i32=0;var x {ft}=1.0;let r=probe(inout log,inout x);
 if log!=12 || x!=2.0 || r!=0.5 {{return 1;}}return 0;}}
'''
        yield ft + '-return-defer', ret, {}, [dict(function='probe', arguments=args,
            result=value(ft, half), cells=[dict(type='i32', value=12), value(ft, two)])]
        fail = prelude + f'''
func probe(log inout i32,x inout {ft}) {ft} throws Token[{ft}] {{
 defer {{log=log*10+2;x*=2.0;}}
 log=1;throw Token[{ft}].Bad(value:x/2.0);
}}
func main() i32 {{var log i32=0;var x {ft}=1.0;
 try {{let r=probe(inout log,inout x);}}
 catch wrong Token[f{96-width}] {{return 3;}}
 catch right Token[{ft}] {{switch right {{case Token.Bad(v):
 if log!=12 || x!=2.0 || v!=0.5 {{return 1;}}return 0;}}}}
 return 2;}}
'''
        yield ft + '-typed-defer', fail, {}, [dict(function='probe', arguments=args,
            error_key=f'app.Token[{ft}]', error_value=value(ft, half),
            cells=[dict(type='i32', value=12), value(ft, two)])]
        panic = prelude + f'''
func probe(log inout i32,x inout {ft}) {ft} throws Token[{ft}] {{
 defer {{log=log*10+2;x*=2.0;}}
 x/=0.0;log=1;var z i32=0;let n=1/z;return x;
}}
func main() i32 throws Token[{ft}] {{var log i32=0;var x {ft}=1.0;
 try {{let r=probe(inout log,inout x);}}
 catch e Token[{ft}] {{return 9;}}return 0;}}
'''
        yield ft + '-panic-skip', panic, dict(panic='integer division by zero'), [dict(
            function='probe', arguments=args, panic='integer division by zero',
            cells=[dict(type='i32', value=1), value(ft, inf)])]
        exceptional = f'''
func probe(log inout i32,x inout {ft}) {ft} {{
 defer {{x/=0.0;log=log*10+2;}}
 log=1;return x/2.0;
}}
func main() i32 {{var log i32=0;var x {ft}=1.0;let r=probe(inout log,inout x);
 if r!=0.5 || log!=12 || x==1.0 {{return 1;}}return 0;}}
'''
        yield ft + '-float-zero-in-defer', exceptional, {}, [dict(function='probe',
            arguments=args, result=value(ft, half),
            cells=[dict(type='i32', value=12), value(ft, inf)])]
        # A floating overflow must allow an ordinary return, without a marker.
        yield ft + '-ordinary-101', f'''func main() i32 {{var x {ft}=1.0;
 x/=0.0;if x>1.0 {{return 101;}}return 1;}}''', dict(main_return=101), []
        yield ft + '-unhandled-typed', prelude + f'''func main() throws Token[{ft}] {{
 var x {ft}=1.0;x/=0.0;throw Token[{ft}].Bad(value:x);}}''', dict(
            typed=f'app.Token[{ft}]'), []


def dynamic_source():
    text = []
    for width in (32, 64):
        ft = f'f{width}'
        for op, symbol in dict(add='+', sub='-', mul='*', div='/', eq='==',
                               ne='!=', lt='<', le='<=', gt='>', ge='>=').items():
            result = 'bool' if op in ('eq', 'ne', 'lt', 'le', 'gt', 'ge') else ft
            text.append(f'func {op}{width}(a {ft},b {ft}) {result} {{return a{symbol}b;}}')
        text.append(f'func neg{width}(a {ft}) {ft} {{return -a;}}')
    return '\n'.join(text) + '\nfunc main() i32 {return 0;}\n'
