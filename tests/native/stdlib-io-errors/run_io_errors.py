#!/usr/bin/env python3
"""Real nonblocking pipes and copy failures, Native O0/O2, 20s per process."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser()
for name in ('cli','root','out'):
    parser.add_argument('--'+name,type=Path,required=True)
args=parser.parse_args()
args.out.mkdir(parents=True,exist_ok=True)
report=dict(status='failed',runs=[])
def command(argv,**kwargs):
    p=subprocess.run([str(x) for x in argv],capture_output=True,timeout=20,**kwargs)
    row=dict(argv=[str(x) for x in argv],exit=p.returncode,
             stdout=p.stdout.decode('utf-8','backslashreplace'),stderr=p.stderr.decode('utf-8','backslashreplace'))
    assert p.returncode==0,row
    return row
try:
    for opt in (0,2):
        out=(args.out/f'o{opt}').resolve()
        row=dict(opt=opt)
        report['runs'].append(row)
        row['build']=command([args.cli,'build','--project',args.root/'tests/native/stdlib-io-errors/lang.toml',
                              '--backend','native','--opt',opt,'--out-dir',out,'--format','json'])
        with tempfile.TemporaryDirectory(prefix='woozy-io-errors-') as work:
            row['run']=command([out/'program'],cwd=work)
            assert not row['run']['stdout'] and not row['run']['stderr'],row
            assert (Path(work)/'destination').read_bytes()=='copied\0😀'.encode(),row
            assert not (Path(work)/'absent-destination').exists(),row
    report['status']='passed'
finally:
    (args.out/'io-errors-results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(dict(status=report['status'],runs=len(report['runs']),report=str(args.out/'io-errors-results.json'))))
