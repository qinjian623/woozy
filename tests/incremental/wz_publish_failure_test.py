#!/usr/bin/env python3
"""Different Wz compiler builds sharing one directory publish coherent files."""
import argparse,concurrent.futures,hashlib,json,os,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cli',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--libxml',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);env=os.environ.copy();env['LD_LIBRARY_PATH']=str(a.libxml);commands=[];sha=lambda b:hashlib.sha256(b).hexdigest();resources={str(a.cli):sha(a.cli.read_bytes())}
for name in ['liblang_native_runtime.a','liblang_hosted_runtime.a']:file=a.cli.parent/'runtime'/name;resources[str(file)]=sha(file.read_bytes())
for name in ['clang','ld.lld','opt','llc']:file=a.cli.parent/'toolchain'/name;resources[str(file)]=sha(file.read_bytes())
def stable():
 for path,value in resources.items():assert sha(Path(path).read_bytes())==value,path
 return dict(resources)
def run(label,argv,expected=0):
 start=time.monotonic();r=subprocess.run(list(map(str,argv)),env=env,capture_output=True,timeout=300);(a.out/(label+'.stdout')).write_bytes(r.stdout);(a.out/(label+'.stderr')).write_bytes(r.stderr);commands.append(dict(label=label,argv=list(map(str,argv)),exit=r.returncode,seconds=time.monotonic()-start,stdout_sha256=sha(r.stdout),stderr_sha256=sha(r.stderr)));assert expected is None or r.returncode==expected,(label,r.returncode,r.stdout,r.stderr);return r
projects=[]
for name,value in [('one',17),('two',29)]:
 project=a.out/name;(project/'src').mkdir(parents=True);(project/'lang.toml').write_text('schema_version=1\n[package]\nname="publisher"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n');(project/'src/main.wz').write_text(f'package app; func main() i32 {{ return {value}; }}');projects.append(project)
output=a.out/'shared';cache=a.out/'cache'
def build(label,project,out,opt='2',expected=0):
 stable();r=run(label,[a.cli,'build','--project',project/'lang.toml','--out-dir',out,'--package-objects','--cache-dir',cache,'--opt',opt,'--format','json'],expected);j=json.loads(r.stdout);assert j['status']==('ok' if expected==0 else 'error');stable();return j
status='failed'
try:
 build('seed',projects[0],output);run('seed-program',[output/'program'],17);alias=a.out/'shared-alias';alias.symlink_to(output,target_is_directory=True)
 with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
  futures=[pool.submit(build,'concurrent-one',projects[0],output,'0'),pool.submit(build,'concurrent-two',projects[1],alias,'2')]
  for future in futures:future.result()
 winner=run('published-program',[output/'program'],None).returncode;assert winner in (17,29)
 metadata=json.loads((output/'package-records.json').read_text());assert len(metadata)==1
 winning_project=projects[0] if winner==17 else projects[1];assert str(winning_project/'src') in metadata[0]['identity'],(winner,metadata)
 log=(output/'package-build.log').read_text();expected_opt='0' if winner==17 else '2';assert '-passes=\'verify,default<O'+expected_opt+'>,verify\'' in log or ('-passes=verify,default<O'+expected_opt+'>,verify') in log,log
 tools=a.cli.parent/'toolchain';runtime=a.cli.parent/'runtime/liblang_native_runtime.a';run('relink-published-object',[tools/'clang','--target=x86_64-unknown-linux-gnu','--ld-path='+str(tools/'ld.lld'),'-nostdlib','-static',output/'program.o',runtime,'-o',a.out/'relinked']);run('relinked-program',[a.out/'relinked'],winner)
 before={name:sha((output/name).read_bytes()) for name in ['program','program.o','package-records.json','cache-statistics.json','package-build.log']}
 (winning_project/'src/main.wz').write_text('package app; func main() i32 { return true; }')
 failed=build('type-failure',winning_project,output,expected=1);assert any(d['code']=='E_TYPE_MISMATCH' for d in failed['diagnostics']);after={name:sha((output/name).read_bytes()) for name in before};assert before==after,(before,after);run('old-program-after-failure',[output/'program'],winner);status='passed'
finally:
 (a.out/'report.json').write_text(json.dumps(dict(status=status,commands=commands,winner_exit=locals().get('winner'),metadata=locals().get('metadata'),before_failure=locals().get('before'),after_failure=locals().get('after'),compiler_before=resources,compiler_after=stable()),indent=2)+'\n');print(json.dumps(dict(status=status,out=str(a.out))))
