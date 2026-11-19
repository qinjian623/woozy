#!/usr/bin/env python3
"""Optional cache writes cannot fail a real build; cache schemas stay aligned."""
import argparse,hashlib,json,os,shutil,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cli',required=True,type=Path);p.add_argument('--out',required=True,type=Path);p.add_argument('--wz-baseline',required=True,type=Path);p.add_argument('--libxml',required=True,type=Path);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);env=os.environ.copy();env['LD_LIBRARY_PATH']=str(a.libxml);sha=lambda b:hashlib.sha256(b).hexdigest();commands=[];cases=[]
P=a.out/'project';(P/'src/a').mkdir(parents=True);(P/'src/b').mkdir();(P/'lang.toml').write_text('schema_version=1\n[package]\nname="schema_io"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n');(P/'src/main.wz').write_text('package app; import "app/a" as a; import "app/b" as b; func main() i32 { return a.value() + b.value() - 42; }');(P/'src/a/a.wz').write_text('package a; func secret() i32 { return 12; } pub func value() i32 { return secret(); }');(P/'src/b/b.wz').write_text('package b; pub func value() i32 { return 30; }')
fixed={str(a.cli):sha(a.cli.read_bytes()),str(a.cli.parent/'native/lang_native_helper'):sha((a.cli.parent/'native/lang_native_helper').read_bytes())}
def stable():
 for path,value in fixed.items():assert sha(Path(path).read_bytes())==value,path
 return fixed
baseline=json.loads(a.wz_baseline.read_text());expected_stat_keys=set(baseline['cases'][0]['statistics']);expected_record_keys=set(baseline['cases'][0]['packages'][0])
def run(label,argv,expected=0):
 start=time.monotonic();r=subprocess.run(list(map(str,argv)),env=env,capture_output=True,timeout=180);(a.out/(label+'.stdout')).write_bytes(r.stdout);(a.out/(label+'.stderr')).write_bytes(r.stderr);commands.append(dict(label=label,argv=list(map(str,argv)),exit=r.returncode,seconds=time.monotonic()-start,stdout_sha256=sha(r.stdout),stderr_sha256=sha(r.stderr)));assert expected is None or r.returncode==expected,(label,r.returncode,r.stdout,r.stderr);return r
cache=a.out/'cache'
def build(label,cache_path,output=None,expected=0):
 stable();out=output or a.out/label;argv=[a.cli,'build','--project',P/'lang.toml','--out-dir',out,'--package-objects','--opt','2','--format','json']
 if cache_path is not None:argv+=['--cache-dir',cache_path]
 r=run(label,argv,expected);protocol=json.loads(r.stdout);assert protocol['status']==('ok' if expected==0 else 'error')
 if expected:return protocol
 run(label+'-program',[out/'program'],0);stats=json.loads((out/'cache-statistics.json').read_text());records=json.loads((out/'package-records.json').read_text());assert set(stats)==expected_stat_keys,(set(stats),expected_stat_keys)
 for record in records:
  assert set(record)==expected_record_keys,record
  assert isinstance(record['object_key'],str) and isinstance(record['frontend_key'],str)
  if cache_path is None:assert record['object_key']==record['frontend_key']=='' and not record['object_cache_hit'],record
 cases.append(dict(label=label,statistics=stats,packages=records,program_sha256=sha((out/'program').read_bytes())));stable();return stats
status='failed'
try:
 cold=build('cold',cache);assert (cold['object_hits'],cold['object_misses'],cold['opt_jobs'])==(0,3,3),cold
 warm=build('warm',cache);assert (warm['object_hits'],warm['object_misses'],warm['opt_jobs'],warm['checked_bodies'])==(3,0,0,0),warm
 uncached=build('disabled',None);assert (uncached['object_hits'],uncached['object_misses'],uncached['opt_jobs'])==(0,0,3),uncached
 blocked=a.out/'blocked-cache';blocked.write_bytes(b'keep cache file');before_cache=sha(blocked.read_bytes());failed_cache=build('cache-path-is-file',blocked);assert (failed_cache['object_hits'],failed_cache['object_misses'],failed_cache['opt_jobs'],failed_cache['checked_bodies'])==(0,3,3,4),failed_cache;assert sha(blocked.read_bytes())==before_cache
 # All cache::store destinations (parse, HIR, object and file digest) are now
 # blocked by the ordinary file. Real source errors still fail before publish.
 out=a.out/'cache-path-is-file';before={name:sha((out/name).read_bytes()) for name in ['program','program.o','cache-statistics.json','package-records.json','package-build.log']};source=(P/'src/a/a.wz').read_text();(P/'src/a/a.wz').write_text(source.replace('return 12;','return true;'));bad=build('source-error-stays-fatal',blocked,output=out,expected=1);assert any(d['code']=='E_TYPE_MISMATCH' for d in bad['diagnostics']);assert before=={name:sha((out/name).read_bytes()) for name in before};(P/'src/a/a.wz').write_text(source)
 output_file=a.out/'blocked-output';output_file.write_bytes(b'keep output file');r=run('output-error-stays-fatal',[a.cli,'build','--project',P/'lang.toml','--out-dir',output_file,'--package-objects','--opt','2','--format','json'],None);assert r.returncode!=0 and json.loads(r.stdout)['status']=='error';assert output_file.read_bytes()==b'keep output file';status='passed'
finally:
 (a.out/'report.json').write_text(json.dumps(dict(status=status,commands=commands,cases=cases,compiler_before=fixed,compiler_after=stable(),wz_baseline=str(a.wz_baseline),wz_baseline_sha256=sha(a.wz_baseline.read_bytes())),indent=2)+'\n');print(json.dumps(dict(status=status,commands=len(commands),out=str(a.out))))
