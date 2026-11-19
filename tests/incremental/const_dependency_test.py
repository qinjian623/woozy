#!/usr/bin/env python3
"""Transitive private/public constants must affect the consuming object key."""
import argparse,hashlib,json,os,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--helper',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--libxml',type=Path,required=True);p.add_argument('--allow-known-bug',action='store_true');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);env=os.environ.copy();env['LD_LIBRARY_PATH']=str(a.libxml);commands=[];sha=lambda b:hashlib.sha256(b).hexdigest()
def run(label,argv,expected=0):
 start=time.monotonic();r=subprocess.run(list(map(str,argv)),env=env,capture_output=True,timeout=180);(a.out/(label+'.stdout')).write_bytes(r.stdout);(a.out/(label+'.stderr')).write_bytes(r.stderr);commands.append(dict(label=label,argv=list(map(str,argv)),exit=r.returncode,seconds=time.monotonic()-start,stdout_sha256=sha(r.stdout),stderr_sha256=sha(r.stderr)));assert expected is None or r.returncode==expected,(label,r.returncode,r.stdout,r.stderr);return r
project=a.out/'project';src=project/'src';(src/'a').mkdir(parents=True);(src/'b').mkdir()
(project/'lang.toml').write_text('schema_version=1\n[package]\nname="constants"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
(src/'b/b.wz').write_text('package b; const k i32 = 7; pub const K i32 = k;')
(src/'a/a.wz').write_text('package a; import "app/b" as B; pub const K i32 = B.K;')
(src/'main.wz').write_text('package app; import "app/a" as A; func main() i32 { return A.K; }')
def build(label,cached=True):
 out=a.out/label;argv=[a.helper,'--project',project/'lang.toml','--out-dir',out,'--opt','2','--format','json','--package-objects']
 if cached:argv+=['--cache-dir',a.out/'cache']
 run(label,argv);r=run(label+'-program',[out/'program'],None);return r.returncode,json.loads((out/'cache-statistics.json').read_text())
status='failed'
try:
 assert build('cold')[0]==7
 (src/'b/b.wz').write_text('package b; const k i32 = 17; pub const K i32 = k;')
 result,statistics=build('private-constant-change');clean,_=build('clean',False);assert clean==17
 bug=result!=clean;assert bug if a.allow_known_bug else result==clean and statistics['object_hits']==0,(result,clean,statistics)
 status='known_bug_reproduced' if bug else 'passed'
finally:
 (a.out/'report.json').write_text(json.dumps(dict(status=status,commands=commands,incremental_exit=locals().get('result'),clean_exit=locals().get('clean'),statistics=locals().get('statistics'),helper_sha256=sha(a.helper.read_bytes())),indent=2)+'\n');print(json.dumps(dict(status=status,out=str(a.out))))
