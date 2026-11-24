#!/usr/bin/env python3
"""CLI execution, final-release observation and damaged NativeIR checks."""
import argparse,hashlib,json,subprocess,traceback
from pathlib import Path

def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser()
 for key in ('cli','probe','root','clang','runtime','runtime-test','out'):p.add_argument('--'+key,type=Path,required=True)
 p.add_argument('--alloc-selftest',type=Path)
 p.add_argument('--case',action='append');p.add_argument('--sanitize',action='store_true');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
 report=dict(status='failed',runs=[],runtime=[],commands=[])
 def command(argv,timeout=45):
  argv=list(map(str,argv));r=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
  row=dict(argv=argv,exit=r.returncode,stdout=r.stdout,stderr=r.stderr);report['commands'].append(row);return row
 def ok(r): assert r['exit']==0,r
 if a.alloc_selftest:
  try:
   r=command([a.alloc_selftest],timeout=20);assert r['exit']==0,r
   live={};allocations=[];frees=[]
   for line in r['stderr'].splitlines():
    if line.startswith('ALLOC:'):
     pointer=line.split(':',1)[1];assert pointer not in live,('duplicate live allocation',line)
     live[pointer]=1;allocations.append(pointer)
    elif line.startswith('FREE:'):
     pointer=line.split(':',1)[1];assert pointer in live,('unknown/duplicate free',line)
     del live[pointer];frees.append(pointer)
   assert len(allocations)==2 and len(frees)==2 and not live,(allocations,frees,live)
   report['alloc_selftest']=dict(status='passed',result=r,allocations=allocations,frees=frees)
  except Exception as e:
   report['alloc_selftest']=dict(status='failed',error=str(e),traceback=traceback.format_exc())
   (a.out/'summary.json').write_text(json.dumps(report,indent=2));print(report['alloc_selftest']);return 1
 catalog=json.loads((a.root/'tests/ownership/cases.json').read_text())['cases']
 capability=command([a.cli,'check','--project',a.root/'tests/ownership/fixtures/vec-shared-index/lang.toml','--backend','native','--format','json'])
 if capability['exit']==1 and any(d['code']=='E_CAPABILITY_UNAVAILABLE' for d in json.loads(capability['stdout']).get('diagnostics',[])):
  report.update(status='unsupported',reason='shared capability check still rejects Vec; apply root native.patch to execute')
  (a.out/'summary.json').write_text(json.dumps(report,indent=2));print(report['reason']);return 77
 cases=[(c['id'],a.root/c['manifest'],c['expected_static'],c.get('runtime_expected',{})) for c in catalog]
 cases += [(m.parent.name,m,dict(status='accept'),{}) for m in sorted((a.root/'tests/native/ownership').glob('*/lang.toml'))]
 if a.case:
  cases=[c for c in cases if c[0] in a.case]
  assert len(cases)==len(set(a.case)), 'unknown case'
 # Continue after a case failure so the report records all unimplemented combinations.
 for ident,manifest,static,expected in cases:
  for opt in (0,2):
   row=dict(id=ident,opt=opt,status='failed');report['runs'].append(row)
   out=a.out/ident/f'O{opt}';out.mkdir(parents=True,exist_ok=True)
   try:
    row['source_hashes']={str(p.relative_to(a.root)):digest(p) for p in sorted(manifest.parent.rglob('*')) if p.is_file()}
    limit=20 if ident=='recursive-generic-growth' else 45
    r=command([a.cli,'build','--project',manifest,'--backend','native','--opt',opt,'--out-dir',out/'cli','--format','json'],timeout=limit);row['build']=r
    if static['status']=='reject':
     assert r['exit']==1 and json.loads(r['stdout'])['diagnostics'],r
     code=static.get('code')
     if code:assert json.loads(r['stdout'])['diagnostics'][0]['code']==code,r
     if ident=='recursive-generic-growth':
      diagnostic=json.loads(r['stdout'])['diagnostics'][0]
      assert 'budget' in diagnostic['message'],diagnostic
      assert diagnostic['primary']['end_byte']>diagnostic['primary']['start_byte'],diagnostic
      assert not (out/'cli').exists(),out/'cli'
     row['status']='passed';row['kind']='static-rejection';continue
    ok(r)
    if ident=='defer-diverges': row['status']='passed';row['kind']='build-only';continue
    panic='panic' in expected or ident in ('negative-index','huge-index','negative-slice','reverse-slice','huge-slice','local-index-borrow-negative')
    code=101 if panic else 1 if ident=='owned-main-error' else expected.get('main_return',0)
    row['execute']=r=command([out/'cli/program']);assert r['exit']==code and not r['stdout'],r
    if panic:assert r['stderr'].startswith('LANG_NATIVE_PANIC:v1:'),r
    elif code==1:assert r['stderr']=='LANG_TYPED_ERROR: app.E\n',r
    else:assert not r['stderr'],r
    row['probe']=r=command([a.probe,manifest,out/'probe',opt]);ok(r)
    row['probe_checks']=json.loads(r['stdout'])
    row['observe_build']=r=command([a.clang,f'-O{opt}',out/'probe/observe.ll',
       a.root/'tests/native/vec_observer.cpp',a.root/'tests/native/alloc_observer.cpp',
       a.runtime,'-o',out/'observe','-Wl,--wrap=lang_alloc_v1','-Wl,--wrap=lang_free_v1']);ok(r)
    row['observe']=r=command([out/'observe']);assert r['exit']==code,r
    live={};allocations=[];frees=[];events=[];obs=[];drops={};iterations={};last_refs={};refs_peak={}
    cleared_without_event4=[]
    heap_live={};heap_allocated=[];heap_freed=[]
    merged=[]
    for line in r['stderr'].splitlines():
     if line.startswith('VEC:'):
      _,event,identity,detail=line.split(':');event=int(event);detail=int(detail);events.append([event,identity,detail])
      merged.append(('vec',event,identity))
      if event==1:
       assert identity not in live,('allocated live identity',line)
       live[identity]=detail;allocations.append([identity,detail]);drops[identity]=[];iterations[identity]=0
      elif event==5:
       assert identity in live,('duplicate free',line)
       length=live.pop(identity)
       # An ordinary final release drops the remaining prefix in reverse order.
       # SourceGC clear() has already dropped the cycle's elements directly, so
       # the collected self-cycle's final event5 has no original event4 prefix.
       # Keep the strict reverse-drop check for every ordinary Vec and only
       # admit that one empty-prefix cycle below.
       if drops[identity]==list(range(length-1,-1,-1)):
        pass
       elif ident=='recursive-cycle-retained' and length and not drops[identity]:
        cleared_without_event4.append(identity)
       else:
        raise AssertionError(('element order',drops[identity],length))
       assert iterations[identity]==0,('freed during iteration',line)
       frees.append([identity,length])
      elif event==2:assert identity in live;last_refs[identity]=detail;refs_peak[identity]=max(refs_peak.get(identity,0),detail)
      elif event==3:assert identity in live;last_refs[identity]=detail;refs_peak[identity]=max(refs_peak.get(identity,0),detail)
      elif event==4:assert identity in live;drops[identity].append(detail)
      elif event in (6,7):assert identity in live;iterations[identity]+=1 if event==6 else -1;assert iterations[identity]>=0
      else:assert identity in live,('operation on freed identity',line)
     elif line.startswith('ALLOC:'):
      pointer=line.split(':',1)[1];assert pointer not in heap_live,('duplicate live allocation',line)
      heap_live[pointer]=1;heap_allocated.append(pointer)
     elif line.startswith('FREE:'):
      pointer=line.split(':',1)[1];assert pointer in heap_live,('unknown/duplicate heap free',line)
      del heap_live[pointer];heap_freed.append(pointer)
     elif line.startswith('OBS:'):
      parts=list(map(int,line.split(':')[1:]));obs.append(parts)
      merged.append(('obs',*parts))
     elif panic:assert line.startswith('LANG_NATIVE_PANIC:v1:'),line
     else:assert line=='LANG_TYPED_ERROR: app.E',line
    cycle=ident=='recursive-cycle-retained'
    if not panic:
     # SourceGC collects the self-cycle, so no Vec identity or heap allocation
     # may survive final release.  Missing frees still fail this balance check.
     assert not live,('still live',live)
     assert not heap_live,('still live allocations',heap_live)
     assert len(heap_allocated)==len(heap_freed),(heap_allocated,heap_freed)
     if cycle:
      # The retained cycle is the one non-empty Vec; it must now be collected
      # after clear(), including its final event5 release.
      cycle_ids=[identity for identity,length in allocations if length]
      assert len(cycle_ids)==1,allocations
      identity=cycle_ids[0]
      assert last_refs.get(identity,0)==0,last_refs
      assert refs_peak.get(identity,0)>=2,refs_peak
      assert cleared_without_event4==[identity],cleared_without_event4
      assert identity in [x[0] for x in frees],frees
      assert drops[identity]==[],drops
    if expected.get('events') is not None:
     assert [x[1] for x in obs if x[0]==1]==expected['events'],obs
    if ident=='field-replace-order':assert [x[1] for x in frees]==[1,5,4,2,3],frees
    if ident=='argument-order-failure':assert [x[1] for x in obs if x[0]==1]==[1,2,3],obs
    if ident=='recursive-mutual-struct-enum':
     assert len(allocations)==2 and len(frees)==2,(allocations,frees)
     assert [x[0] for x in frees]==[allocations[1][0],allocations[0][0]],(allocations,frees)
    if ident=='recursive-typed-error':
     assert [x[1] for x in obs if x[0]==1]==[1,2,3,4,5,6,7,8,9,0],obs
     defer_mark=merged.index(('obs',1,0,1234567890))
     probe_return=merged.index(('obs',2,7,1234567890))
     last_free=max(i for i,x in enumerate(merged) if x[0]=='vec' and x[1]==5)
     assert defer_mark<last_free,merged
     assert probe_return>defer_mark,merged
    if ident=='recursive-for-in-early-exits':
     assert [x[1] for x in obs if x[0]==1]==[10,20,10,20,10,20],obs
     assert [x[2] for x in obs if x[0]==1]==[10,1020,10,1020,10,1020],obs
     assert sorted(length for _,length in allocations)==[0,0,0,1,1,1,3],allocations
     roots=[identity for identity,length in allocations if length==3]
     assert len(roots)==1,allocations
     root=roots[0]
     marks=[i for i,x in enumerate(merged) if x[0]=='obs']
     last_frees=[i for i,x in enumerate(merged) if x[0]=='vec' and x[1]==5]
     assert len(marks)==6 and len(last_frees)==7,merged
     assert min(last_frees)>max(marks),merged
     assert [x for x in merged if (x[0]=='vec' and x[2]==root and x[1] in (6,7)) or x[0]=='obs']==[
       ('vec',6,root),('obs',1,10,10),('obs',1,20,1020),('vec',7,root)]*3,merged
    if ident=='defer-before-destroy':assert [x[2] for x in obs if x[0]==2]==[17],obs
    if ident in ('local-index-borrow-error','local-index-borrow-place-error'):
     assert len(allocations)==1 and len(frees)==1,(allocations,frees)
     assert [x[1] for x in obs if x[0]==1]==[1,2,3,4],obs
     assert merged.index(('obs',1,3,123))<merged.index(('vec',5,allocations[0][0]))<merged.index(('obs',1,4,1234)),merged
    if ident=='owned-return-error-defer':assert [x[1] for x in obs if x[0]==1]==[2,1,4,3],obs
    if ident=='computed-owning-once-defer-order':
     assert len(allocations)==1 and len(frees)==1,(allocations,frees)
     assert merged.index(('obs',1,2,12))<merged.index(('vec',5,allocations[0][0]))<merged.index(('obs',2,0,123)),merged
    if ident=='computed-owning-return-payload':
     assert len(allocations)==1 and len(frees)==1,(allocations,frees)
     assert merged.index(('obs',1,2,12))<merged.index(('vec',5,allocations[0][0]))<merged.index(('obs',2,0,123)),merged
    if ident=='computed-owning-loop-break-continue':
     assert [x for x in merged if x[0]=='vec' and x[1] in (1,5)]==[('vec',1,allocations[0][0]),('vec',5,allocations[0][0]),('vec',1,allocations[1][0]),('vec',5,allocations[1][0]),('vec',1,allocations[2][0]),('vec',5,allocations[2][0])],merged
    if ident=='computed-owning-construction-move-empty':
     assert len(allocations)==2 and len(frees)==2,(allocations,frees)
     assert [x[0] for x in frees]==[allocations[1][0],allocations[0][0]],frees
    if ident=='computed-owning-recursive-pattern':
     assert len(allocations)==1 and len(frees)==1,(allocations,frees)
     assert merged.index(('obs',1,2,12))<merged.index(('vec',5,allocations[0][0]))<merged.index(('obs',1,3,123)),merged
    if ident=='computed-owning-error-escapes-arm':
     assert len(allocations)==1 and len(frees)==1,(allocations,frees)
     assert merged.index(('obs',1,2,2))<merged.index(('vec',5,allocations[0][0]))<merged.index(('obs',1,4,24)),merged
    if ident=='computed-literal-exits':
     assert len(allocations)==2 and len(frees)==2,(allocations,frees)
     assert [x[1] for x in obs if x[0]==1]==[1,2,3,1,4,5],obs
     first_free=[p for p,x in enumerate(merged) if x[0]=='vec' and x[1]==5][0]
     second_free=[p for p,x in enumerate(merged) if x[0]=='vec' and x[1]==5][1]
     make_marks=[p for p,x in enumerate(merged) if x==('obs',1,1,1)]
     assert (make_marks[0]<merged.index(('obs',1,2,12))<
             merged.index(('obs',1,3,123))<first_free<make_marks[1]<
             merged.index(('obs',1,4,14))<merged.index(('obs',1,5,145))<
             second_free),merged
    if ident=='computed-owning-catch-throw-nested':
     assert len(allocations)==3 and len(frees)==3,(allocations,frees)
     ida,idb,idc=[x[0] for x in allocations]
     assert [x[0] for x in frees]==[idb,idc,ida],frees
     freed=[p for p,x in enumerate(merged) if x[0]=='vec' and x[1]==5]
     assert len(freed)==3,freed
     def at(t): return merged.index(t)
     assert at(('obs',1,1,1))<at(('obs',1,2,12))<freed[0]<at(('obs',1,5,125))<freed[1]<at(('obs',1,6,1256))<at(('obs',1,7,12567))<freed[2],merged
    row['observations']=dict(allocations=allocations,last_frees=frees,live=live,
      values=obs,events=events,heap_allocations=heap_allocated,
      heap_frees=heap_freed,heap_live=heap_live,refs=last_refs,refs_peak=refs_peak)
    if a.sanitize:
     row['sanitized_build']=r=command([a.clang,f'-O{opt}','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-exceptions','-fno-rtti',
       out/'probe/sanitize.ll',a.root/'runtime/native/weak.cpp',a.root/'runtime/native/panic.cpp',a.root/'runtime/native/alloc.cpp',a.root/'runtime/native/text.cpp',a.root/'runtime/native/thread.cpp',a.root/'tests/native/vec_observer.cpp','-o',out/'sanitize']);ok(r)
     row['sanitized_execute']=r=command([out/'sanitize']);assert r['exit']==code and 'ERROR: AddressSanitizer' not in r['stderr'] and 'runtime error:' not in r['stderr'],r
    row['hashes']={str(p.relative_to(out)):digest(p) for p in sorted(out.rglob('*')) if p.is_file()}
    row['kind']='panic' if panic else 'execution-and-final-release';row['status']='passed'
   except Exception as e:row['failure']=str(e);row['traceback']=traceback.format_exc()
 for mode,cause in [('normal',None),('object-oom','allocation-failed'),('storage-oom','allocation-failed'),('size-overflow','allocation-size-overflow'),('size-overflow-total','allocation-size-overflow'),('count-overflow','reference-count-overflow'),('iteration-resize','active-iteration'),('null-iteration-resize','active-iteration'),('null-observe',None),('rotate-bounds','vec-index'),('rotate-pin','active-iteration')]:
  r=command([a.runtime_test,mode]);report['runtime'].append(dict(mode=mode,result=r,status='passed' if (r['exit']==0 if cause is None else r['exit']==101 and ':'+cause+':' in r['stderr']) else 'failed'))
 report['passed']=sum(r['status']=='passed' for r in report['runs']);report['failed']=len(report['runs'])-report['passed']
 report['status']='passed' if report['failed']==0 and all(r['status']=='passed' for r in report['runtime']) else 'failed'
 (a.out/'summary.json').write_text(json.dumps(report,indent=2))
 print(json.dumps(dict(status=report['status'],passed=report['passed'],failed=report['failed'],failures=[dict(id=r['id'],opt=r['opt'],failure=r.get('failure')) for r in report['runs'] if r['status']=='failed'])))
 return report['status']!='passed'
if __name__=='__main__':raise SystemExit(main())
