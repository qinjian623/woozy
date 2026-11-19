#!/usr/bin/env python3
"""Generic code owned by a package must track foreign nominal layouts."""
import argparse,hashlib,json,os,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--helper',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--libxml',type=Path,required=True);p.add_argument('--allow-known-bug',action='store_true');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);env=os.environ.copy();env['LD_LIBRARY_PATH']=str(a.libxml);commands=[];sha=lambda b:hashlib.sha256(b).hexdigest()
def run(label,argv,expected=0):
 start=time.monotonic();r=subprocess.run(list(map(str,argv)),env=env,capture_output=True,timeout=180);(a.out/(label+'.stdout')).write_bytes(r.stdout);(a.out/(label+'.stderr')).write_bytes(r.stderr);commands.append(dict(label=label,argv=list(map(str,argv)),exit=r.returncode,seconds=time.monotonic()-start,stdout_sha256=sha(r.stdout),stderr_sha256=sha(r.stderr)));assert expected is None or r.returncode==expected,(label,r.returncode,r.stdout,r.stderr);return r
project=a.out/'project';src=project/'src';(src/'dep').mkdir(parents=True);(src/'model').mkdir()
(project/'lang.toml').write_text('schema_version=1\n[package]\nname="layouts"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
(src/'dep/id.wz').write_text('package dep; pub func id[T](value T) T { return value; }')
(src/'model/model.wz').write_text('package model; pub struct S { pub value i32; }')
(src/'main.wz').write_text('package app; import "app/dep" as dep; import "app/model" as model; func main() i32 { let value = dep.id[model.S](model.S(value: 17)); return value.value; }')
def build(label,cached=True):
 out=a.out/label;argv=[a.helper,'--project',project/'lang.toml','--out-dir',out,'--opt','2','--format','json','--package-objects'];
 if cached:argv+=['--cache-dir',a.out/'cache']
 run(label,argv);r=run(label+'-program',[out/'program'],None);return r.returncode,json.loads((out/'package-records.json').read_text())
status='failed'
try:
 assert build('cold')[0]==17
 (src/'model/model.wz').write_text('package model; pub struct S { pub p0 i64; pub p1 i64; pub p2 i64; pub value i32; }')
 (src/'main.wz').write_text('package app; import "app/dep" as dep; import "app/model" as model; func main() i32 { let value = dep.id[model.S](model.S(p0: 1, p1: 2, p2: 3, value: 17)); return value.value; }')
 result,records=build('layout-change');clean,_=build('clean',False);assert clean==17
 dep=next(record for record in records if record['logical_path']=='app/dep');bug=result!=clean or dep['object_cache_hit']
 assert bug if a.allow_known_bug else result==clean and not dep['object_cache_hit'],(result,clean,records)
 if not a.allow_known_bug:
  project=a.out/'transitive-project';src=project/'src';(src/'dep').mkdir(parents=True);(src/'model').mkdir();(src/'outer').mkdir()
  (project/'lang.toml').write_text('schema_version=1\n[package]\nname="transitive_layouts"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
  (src/'dep/id.wz').write_text('package dep; pub func id[T](value T) T { return value; }')
  (src/'model/model.wz').write_text('package model; pub struct S { pub value i32; }')
  (src/'outer/outer.wz').write_text('package outer; import "app/model" as model; pub struct Outer { pub inner model.S; } pub func make() Outer { return Outer(inner: model.S(value: 17)); }')
  (src/'main.wz').write_text('package app; import "app/dep" as dep; import "app/outer" as outer; func main() i32 { let value = dep.id[outer.Outer](outer.make()); return value.inner.value; }')
  assert build('transitive-cold')[0]==17
  (src/'model/model.wz').write_text('package model; pub struct S { pub p0 i64; pub p1 i64; pub p2 i64; pub value i32; }')
  (src/'outer/outer.wz').write_text('package outer; import "app/model" as model; pub struct Outer { pub inner model.S; } pub func make() Outer { return Outer(inner: model.S(p0: 1, p1: 2, p2: 3, value: 17)); }')
  transitive,transitive_records=build('transitive-layout-change');transitive_clean,_=build('transitive-clean',False)
  assert transitive==transitive_clean==17,(transitive,transitive_clean)
  assert not any(item['object_cache_hit'] for item in transitive_records if item['logical_path'] in ('app','app/dep')),transitive_records
 status='known_bug_reproduced' if bug else 'passed' 
finally:
 (a.out/'report.json').write_text(json.dumps(dict(status=status,commands=commands,incremental_exit=locals().get('result'),clean_exit=locals().get('clean'),package_records=locals().get('records'),transitive_records=locals().get('transitive_records'),transitive_exit=locals().get('transitive'),helper_sha256=sha(a.helper.read_bytes())),indent=2)+'\n');print(json.dumps(dict(status=status,out=str(a.out))))
