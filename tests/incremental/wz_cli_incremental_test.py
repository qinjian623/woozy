#!/usr/bin/env python3
"""Run a fixed Wz CLI on genuine cold/warm/private-body package builds."""
import argparse,hashlib,json,os,re,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cli',required=True,type=Path);p.add_argument('--out',required=True,type=Path);p.add_argument('--libxml',required=True,type=Path);p.add_argument('--source-manifest',required=True,type=Path);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);sha=lambda b:hashlib.sha256(b).hexdigest();compiler_sha=sha(a.cli.read_bytes());manifest_sha=sha(a.source_manifest.read_bytes());resources={str(a.cli):compiler_sha}
for name in ['liblang_native_runtime.a','liblang_hosted_runtime.a']:file=a.cli.parent/'runtime'/name;resources[str(file)]=sha(file.read_bytes())
for name in ['opt','llc','clang','ld.lld']:file=a.cli.parent/'toolchain'/name;resources[str(file)]=sha(file.read_bytes())
env=os.environ.copy();env['LD_LIBRARY_PATH']=str(a.libxml)+(':'+env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '');commands=[];cases=[]
def unchanged():
 assert sha(a.source_manifest.read_bytes())==manifest_sha
 for file,value in resources.items():assert sha(Path(file).read_bytes())==value,file
 return dict(resources)
def run(label,argv,expected=0):
 start=time.monotonic();r=subprocess.run(list(map(str,argv)),env=env,capture_output=True,timeout=300);(a.out/(label+'.stdout')).write_bytes(r.stdout);(a.out/(label+'.stderr')).write_bytes(r.stderr);commands.append(dict(label=label,argv=list(map(str,argv)),exit=r.returncode,seconds=time.monotonic()-start,stdout_sha256=sha(r.stdout),stderr_sha256=sha(r.stderr)));assert r.returncode==expected,(label,r.returncode,r.stdout,r.stderr);return r
P=a.out/'project';(P/'src/a').mkdir(parents=True);(P/'src/b').mkdir();(P/'lang.toml').write_text('schema_version=1\n[package]\nname="wz_three"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n');(P/'src/main.wz').write_text('package main; import "app/a" as a; import "app/b" as b; func main() i32 { return a.value() + b.value() - 42; }');(P/'src/a/a.wz').write_text('package a; func secret() i32 { return 12; } pub func value() i32 { return secret(); }');(P/'src/b/b.wz').write_text('package b; pub func value() i32 { return 30; }')
def build(label,value,cached=True):
 unchanged();out=a.out/label;argv=[a.cli,'build','--project',P/'lang.toml','--out-dir',out,'--package-objects','--opt','2','--format','json']
 if cached:argv+=['--cache-dir',a.out/'cache']
 result=run(label,argv);assert json.loads(result.stdout)['status']=='ok';run(label+'-program',[out/'program'],value);stats=json.loads((out/'cache-statistics.json').read_text());records=json.loads((out/'package-records.json').read_text());log=(out/'package-build.log').read_text();assert stats['opt_jobs']==len(re.findall(r'^\[\d+/\d+\].* -passes=',log,re.M));assert stats['llc_jobs']==len(re.findall(r'^\[\d+/\d+\].* -filetype=obj ',log,re.M));cases.append(dict(label=label,statistics=stats,packages=records,program_sha256=sha((out/'program').read_bytes()),source_files={str(f.relative_to(P)):sha(f.read_bytes()) for f in sorted((P/'src').rglob('*.wz'))},compiler_after=unchanged()));return stats,{v['logical_path']:v for v in records}
status='failed'
try:
 cold,cold_records=build('cold',0);assert (cold['parse_misses'],cold['checked_bodies'],cold['opt_jobs'],cold['llc_jobs'])==(3,4,3,3),cold
 warm,_=build('warm',0);assert (warm['parse_hits'],warm['checked_bodies'],warm['object_hits'],warm['opt_jobs'],warm['llc_jobs'])==(3,0,3,0,0),warm
 if 'tool_digest_hits' in warm:
  assert (cold['tool_digest_hits'],cold['tool_digest_misses'])==(0,8),cold
  assert cold['tool_digest_bytes_read']>800000000,cold
  assert (warm['tool_digest_hits'],warm['tool_digest_misses'],warm['tool_digest_bytes_read'])==(8,0,0),warm
 (P/'src/a/a.wz').write_text('package a; func secret() i32 { var x i32 = 11; x += 2; return x; } pub func value() i32 { return secret(); }')
 changed,changed_records=build('private-length-change',1);assert (changed['frontend_package_hits'],changed['checked_bodies'],changed['object_hits'],changed['opt_jobs'])==(2,2,2,1),changed
 for name in ['app','app/b']:assert cold_records[name]['llvm_ir_sha256']==changed_records[name]['llvm_ir_sha256'] and changed_records[name]['object_cache_hit'],name
 assert cold_records['app/a']['llvm_ir_sha256']!=changed_records['app/a']['llvm_ir_sha256'] and not changed_records['app/a']['object_cache_hit']
 build('clean',1,False)
 if 'tool_digest_hits' in warm:
  assert (changed['tool_digest_hits'],changed['tool_digest_misses'],changed['tool_digest_bytes_read'])==(8,0,0),changed
  # Corrupt only the candidate program's small digest record; the executable
  # and real LLVM inputs remain untouched. Rehash must preserve its identity.
  active=[f for f in (a.out/'cache/file-digests').glob('*.sha') if compiler_sha.encode() in f.read_bytes()]
  assert len(active)==1,active
  original=active[0].read_bytes();(a.out/'program-digest-before.bin').write_bytes(original);active[0].write_bytes(original[:29])
  repaired,_=build('digest-record-truncated',1)
  assert (repaired['tool_digest_hits'],repaired['tool_digest_misses'],repaired['tool_digest_bytes_read'])==(7,1,a.cli.stat().st_size),repaired
  assert repaired['checked_bodies']==0 and repaired['object_hits']==3 and repaired['opt_jobs']==0,repaired
 status='passed'
finally:
 (a.out/'report.json').write_text(json.dumps(dict(status=status,commands=commands,cases=cases,compiler_before=resources,compiler_after=unchanged(),source_manifest_sha256=manifest_sha,scope='real Wz CLI package cache; warm still emits LLVM; not formal self-host bootstrap'),indent=2)+'\n');print(json.dumps(dict(status=status,out=str(a.out))))
