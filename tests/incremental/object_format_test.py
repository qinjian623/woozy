#!/usr/bin/env python3
"""Linux object cache rejects otherwise valid COFF and Mach-O records."""
import argparse,hashlib,json,os,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--helper',required=True,type=Path);p.add_argument('--clang',required=True,type=Path);p.add_argument('--out',required=True,type=Path);p.add_argument('--libxml',required=True,type=Path);p.add_argument('--allow-known-bug',action='store_true');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);env=os.environ.copy();env['LD_LIBRARY_PATH']=str(a.libxml);commands=[];sha=lambda b:hashlib.sha256(b).hexdigest();n=lambda v:str(v).encode()+b':';s=lambda b:n(len(b))+b
project=a.out/'project';(project/'src').mkdir(parents=True);(project/'lang.toml').write_text('schema_version=1\n[package]\nname="format"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n');(project/'src/main.wz').write_text('package app; func main() i32 { return 17; }');(a.out/'foreign.c').write_text('int foreign_function(void) { return 1; }')
def run(label,argv,expected=0):
 start=time.monotonic();r=subprocess.run(list(map(str,argv)),env=env,capture_output=True,timeout=180);(a.out/(label+'.stdout')).write_bytes(r.stdout);(a.out/(label+'.stderr')).write_bytes(r.stderr);commands.append(dict(label=label,argv=list(map(str,argv)),exit=r.returncode,seconds=time.monotonic()-start,stdout_sha256=sha(r.stdout),stderr_sha256=sha(r.stderr)));assert expected is None or r.returncode==expected,(label,r.returncode,r.stdout,r.stderr);return r
status='failed';results=[]
try:
 for format,target in [('coff','x86_64-w64-windows-gnu'),('macho','x86_64-apple-darwin')]:
  out=a.out/format;cache=a.out/(format+'-cache');argv=[a.helper,'--project',project/'lang.toml','--out-dir',out,'--package-objects','--cache-dir',cache,'--opt','2','--format','json'];run(format+'-cold',argv)
  key=json.loads((out/'package-records.json').read_text())[0]['object_key'];file=cache/'frontend'/key;before=file.read_bytes();run(format+'-foreign-compile',[a.clang,'--target='+target,'-c',a.out/'foreign.c','-o',a.out/(format+'.o')]);obj=(a.out/(format+'.o')).read_bytes();body=s(b'wz-native-object/1')+s(key.encode())+s(obj);file.write_bytes(sha(body).encode()+b'\n'+body)
  r=run(format+'-injected',argv,None);bug=r.returncode!=0
  if not bug:
   stats=json.loads((out/'cache-statistics.json').read_text());assert stats['object_hits']==0 and stats['opt_jobs']==1,stats;assert run(format+'-program',[out/'program'],None).returncode==17
  assert bug if a.allow_known_bug else not bug,(format,r.returncode)
  results.append(dict(format=format,known_bug=bug,before_cache_sha256=sha(before),foreign_object_sha256=sha(obj)))
 status='known_bug_reproduced' if any(r['known_bug'] for r in results) else 'passed'
finally:
 (a.out/'report.json').write_text(json.dumps(dict(status=status,commands=commands,results=results,helper_sha256=sha(a.helper.read_bytes())),indent=2)+'\n');print(json.dumps(dict(status=status,out=str(a.out))))
