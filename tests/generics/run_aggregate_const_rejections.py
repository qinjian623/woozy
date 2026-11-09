#!/usr/bin/env python3
"""Pure aggregate const rejection and output preservation checks."""
import argparse
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
import string_cases

CAP = 'E_CAPABILITY_UNAVAILABLE'
TYPE = 'E_TYPE_MISMATCH'
string_cases.REJECTED = {
    'private-field': ('import "app/hidden";const C=hidden.Value.x;func main(){}', 'E_PRIVATE_ACCESS'),
    'private-constructor': ('import "app/hidden";const C=hidden.S(x:1);func main(){}', 'E_PRIVATE_ACCESS'),
    'private-result': ('struct S{x i32;}pub const C=S(x:1);func main(){}', 'E_PRIVATE_TYPE_LEAK'),
    'ordinary-call': ('struct S{x i32;} func f()i32{return 1;} const C=S(x:f());func main(){}', CAP),
    'runtime-let': ('struct S{x i32;}func main(){let x i32=1;const C=S(x:x);}', CAP),
    'runtime-parameter': ('struct S{x i32;}func f(x i32){const C=S(x:x);}func main(){}', CAP),
    'vec-field': ('struct S{x Vec[i32];}const C=S(x:[1]);func main(){}', CAP),
    'const-cycle': ('struct S{x i32;}const A=S(x:B.x);const B=A;func main(){}', 'E_CONSTANT_CYCLE'),
    'readonly-field': ('struct S{x i32;}func main(){const C=S(x:1);C.x=2;}', TYPE),
    'readonly-alias': ('struct S{x i32;}func main(){const C=S(x:1);const D=C;D.x=2;}', TYPE),
    'enum-payload-type': ('enum E{A(x i32);}const C=E.A("x");func main(){}', TYPE),
    'panic-field': ('struct S{x i32;}const C=S(x:1/0);func main(){}', CAP),
    'move-after-read': ('struct S{x string;}func main(){const C=S(x:"x");let y=move C;let z=C.x;}', 'E_OWNERSHIP_USE_AFTER_MOVE'),
}

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--cli',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    report={'status':'failed','backends':{}}
    try:
        for backend in ('native','python'):
            for ident in ('private-field','private-constructor'):
                module=args.out/backend/'rejected'/ident/'src/hidden/lib.lang'
                module.parent.mkdir(parents=True,exist_ok=True)
                module.write_text('package hidden;pub struct S{x i32;}pub const Value=S(x:1);')
            report['backends'][backend]=string_cases.rejection_checks(args.cli,backend,args.out/backend)
        report['status']='passed'
    finally:
        (args.out/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'cases':sum(map(len,report['backends'].values()))}))
if __name__=='__main__':main()
