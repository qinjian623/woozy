#!/usr/bin/env python3
"""Real CLI programs, generated LLVM probes, and corrupt NativeIR rejection."""
import argparse
import json
from pathlib import Path
import struct
from run_cli import command, digest

EXTRA = {
    'return-error': [1,2,3], 'pod-copy': [1,2,3], 'recursion': [1,2,3,4],
    'generic-match': [1,2], 'generic-payload': [1,2],
    'short-circuit': [1], 'registration': [1,2],
    'panic-body': [1], 'panic-defer': [1,2],
    'same-value-error': [], 'unit-success': [], 'main-i32-error': [], 'main-unit-error': [],
    'main-generic-error': [], 'main-string-error': [], 'main-return101': [],
    'main-return7': [], 'cross-module': [],
    'local-binding-two-returns': [], 'local-handler-two-returns': [],
    'local-loop-two-returns': [], 'local-handler-call-exits': [],
    'unit-cleanup-obligation': [1], 'unit-explicit-return': [1],
    'unit-nested-block': [1], 'unit-no-defer': [], 'unit-multiple-exits': [],
    'unit-two-call-exits': [1,1],
    'fn-error-inner-throws': [], 'fn-error-outer-throws': [],
    'fn-error-inner-throws-renumbered': [], 'fn-error-outer-throws-renumbered': [],
    'function-key-parameters': [],
    'enum-switch-single-in-defer': [3], 'enum-switch-in-defer': [1,3],
    'unsafe-scalar-lambda': [], 'unsafe-return-owned-defer': [1,2,3],
    'unsafe-loop-owned-defer': [11,21,32],
    'unsafe-caught-throw-owned-defer': [1,2,3],
    'unsafe-inner-catch-owned-defer': [1,2,3,4],
}
ERROR_KEYS = {'main-i32-error': 'app.E', 'main-unit-error': 'app.E',
              'main-generic-error': 'app.Err[i32]', 'main-string-error': 'string',
              'cross-module': 'app/errors.Err[i32]',
              'fn-error-inner-throws': 'func()->(func()->(Unit) throws(app.E))',
              'fn-error-outer-throws': 'func()->(func()->(Unit)) throws(app.E)',
              'fn-error-inner-throws-renumbered': 'func()->(func()->(Unit) throws(app.E))',
              'fn-error-outer-throws-renumbered': 'func()->(func()->(Unit)) throws(app.E)',
              'function-key-parameters': 'func(i32,inout i64)->(i32) throws(app.E)'}


def main():
    parser=argparse.ArgumentParser()
    for name in ('cli','probe','root','clang','runtime','out'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    report=dict(operation_id='language-t08-native-execution-20261104', status='failed',
                runs=[], rejections=[], controls=[], profile='native/64 typed errors and lexical cleanup')
    try:
        catalog=args.root/'tests/cleanup/cases.json'
        report['catalog_sha256']=digest(catalog)
        original=json.loads(catalog.read_text())['cases']
        cases=[(c['id'],args.root/c['manifest'],c.get('runtime_expected',{}))
               for c in original if c['expected_static']['status']=='accept']
        cases += [(k,args.root/'tests/native/cleanup'/k/'lang.toml',dict(events=v)) for k,v in EXTRA.items()]
        report['clang']=command([args.clang,'--version'])
        assert report['clang']['exit']==0 and '22.1.8' in report['clang']['stdout']
        for ident,manifest,expected in cases:
            for opt in (0,2):
                out=args.out/ident/f'O{opt}';row=dict(id=ident,opt=opt,status='failed',
                    sources={str(p.relative_to(args.root)):digest(p) for p in sorted(manifest.parent.rglob('*')) if p.is_file()})
                report['runs'].append(row)
                def run(name,argv):
                    row[name]=r=command(argv);return r
                r=run('check',[args.cli,'check','--project',manifest,'--backend','native','--format','json'])
                assert r['exit']==0 and json.loads(r['stdout'])['status']=='ok',r
                r=run('build',[args.cli,'build','--project',manifest,'--backend','native','--opt',opt,'--out-dir',out/'cli','--format','json'])
                assert r['exit']==0 and not r['stderr'],r
                expected_exit = 101 if ident in ('case-10','panic-body','panic-defer','main-return101') else 1 if ident in ERROR_KEYS else 7 if ident=='main-return7' else 0
                r=run('execute',[out/'cli/program'])
                assert r['exit']==expected_exit and not r['stdout'],r
                if ident in ('case-10','panic-body','panic-defer'):
                    assert r['stderr'].startswith('LANG_NATIVE_PANIC:v1:division-by-zero:'),r
                    source=(manifest.parent/'src/main.lang').read_bytes()
                    fields=r['stderr'].strip().split(':');begin,end=map(int,fields[-2:])
                    assert source[begin:end]==b'i32(1)/zero', (begin,end,source[begin:end])
                    if ident=='case-10':assert source[:begin].count(b'\n')+1==12
                elif ident in ERROR_KEYS:
                    assert r['stderr']=='LANG_TYPED_ERROR: '+ERROR_KEYS[ident]+'\n',r
                else:assert not r['stderr'],r
                r=run('probe',[args.probe,manifest,out/'probe',opt])
                assert r['exit']==0 and not r['stderr'],r
                row['probe_checks']=json.loads(r['stdout'])
                assert (out/'probe/generated.o').read_bytes()==(out/'cli/program.o').read_bytes(),'CLI/probe object differs'
                r=run('observe_build',[args.clang,f'-O{opt}',out/'probe/observe.ll',args.runtime,'-o',out/'observe'])
                assert r['exit']==0,r
                # Binary records emitted by instrumented actual generated functions.
                import subprocess
                q=subprocess.run([str(out/'observe')],capture_output=True,timeout=30)
                row['observe_execute']=dict(exit=q.returncode,stdout_hex=q.stdout.hex(),stderr=q.stderr.decode())
                assert q.returncode==expected_exit and q.stderr.decode()==row['execute']['stderr'],row['observe_execute']
                assert len(q.stdout)%24==0,'partial event record'
                records=list(struct.iter_unpack('<qqq',q.stdout));row['records']=records
                events=[a for tag,a,b in records if tag==1]
                if ident=='errors-original':
                    assert [(a,b) for tag,a,b in records if tag==3]==[(1,1),(2,0)],records
                    assert [a for tag,a,b in records if tag==6]==[7],records
                else:
                    assert events==expected['events'],(ident,events,expected['events'])
                    if ident!='generic-match':
                        log=0
                        for tag,a,b in records:
                            if tag==1:
                                log=log+a if ident=='unit-two-call-exits' else log*10+a
                                assert b==log,(ident,b,log)
                        if 'log' in expected:assert log==expected['log']
                        if 'log_at_panic' in expected:assert log==expected['log_at_panic']
                if ident in ('case-01','case-02'):
                    assert [(a,b) for tag,a,b in records if tag==2]==[(expected['function_returns']['probe'],expected['log'])],records
                if ident in ('case-10','panic-body','panic-defer'):
                    assert all(tag!=4 for tag,a,b in records),'Panic reached normal main return'
                elif ident not in ERROR_KEYS and not ident.startswith('main-return') and ident!='unit-success':
                    assert [a for tag,a,b in records if tag==4]==[0],records
                repeated = {'local-binding-two-returns': [(1,4),(2,8)],
                            'local-handler-two-returns': [(1,1),(2,2)],
                            'local-loop-two-returns': [(1,2),(2,4)],
                            'unit-multiple-exits': [(0,1),(0,2)],
                            'unit-two-call-exits': [(0,1),(0,2)]}
                if ident in repeated:
                    assert [(a,b) for tag,a,b in records if tag==7]==repeated[ident],records
                if ident=='local-handler-call-exits':
                    assert [(a,b) for tag,a,b in records if tag==8]==[(1,4),(0,8)],records
                if ident in ('unit-cleanup-obligation','unit-explicit-return','unit-nested-block','unit-no-defer'):
                    assert [(a,b) for tag,a,b in records if tag==2]==[(0,1)],records
                row['artifacts']={str(p.relative_to(out)):digest(p) for p in sorted(out.rglob('*')) if p.is_file()}
                row['status']='passed'
        for case in original:
            if case['expected_static']['status']!='reject':continue
            manifest=args.root/case['manifest'];out=args.out/'rejected'/case['id'];out.mkdir(parents=True,exist_ok=True)
            for name in ('program','program.o','unrelated'):(out/name).write_text('preserve-'+name)
            before={p.name:digest(p) for p in out.iterdir()}
            row=dict(id=case['id'],expected=case['expected_static']['code'],status='failed');report['rejections'].append(row)
            for phase in ('check','build'):
                argv=[args.cli,phase,'--project',manifest,'--backend','native','--format','json']
                if phase=='build':argv+=['--out-dir',out]
                row[phase]=r=command(argv)
                assert r['exit']==1 and any(d['code']==row['expected'] for d in json.loads(r['stdout'])['diagnostics']),r
                assert before=={p.name:digest(p) for p in out.iterdir()}
            row['status']='passed'
        for ident in ('fn-error-inner-throws','fn-error-outer-throws'):
            for opt in (0,2):
                pair=[r for r in report['runs'] if r['opt']==opt and r['id'] in (ident,ident+'-renumbered')]
                assert len(pair)==2 and pair[0]['probe_checks']['entry_error_type_id'] != pair[1]['probe_checks']['entry_error_type_id'],pair
                assert pair[0]['execute']['stderr']==pair[1]['execute']['stderr'],pair
        report['status']='passed'
    finally:
        path=args.out/'results.json';path.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(dict(status=report['status'],executions=len(report['runs']),
        observations=sum(len(r.get('records',[])) for r in report['runs']),
        native_ir_rejected=sum(r.get('probe_checks',{}).get('native_ir_rejected',0) for r in report['runs']),report=str(path))))

if __name__=='__main__':main()
