#!/usr/bin/env python3
"""Use a fixed Wz compiler to reject COFF, Mach-O and a truncated ELF payload."""
import argparse,hashlib,json,os,shutil,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cli',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--digest-records',type=Path,required=True);p.add_argument('--coff',type=Path,required=True);p.add_argument('--macho',type=Path,required=True);p.add_argument('--libxml',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);sha=lambda b:hashlib.sha256(b).hexdigest();commands=[];cases=[];env=os.environ.copy();env['LD_LIBRARY_PATH']=str(a.libxml);fixed={str(a.cli):sha(a.cli.read_bytes())}
for name in ['liblang_native_runtime.a','liblang_hosted_runtime.a']:f=a.cli.parent/'runtime'/name;fixed[str(f)]=sha(f.read_bytes())
for name in ['opt','llc','clang','ld.lld']:f=a.cli.parent/'toolchain'/name;fixed[str(f)]=sha(f.read_bytes())
def stable():
 for path,h in fixed.items():assert sha(Path(path).read_bytes())==h,path
 return fixed
P=a.out/'project';(P/'src').mkdir(parents=True);(P/'lang.toml').write_text('schema_version=1\n[package]\nname="object_injection"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n');(P/'src/main.wz').write_text('package app; func main() i32 { return 17; }');cache=a.out/'cache';shutil.copytree(a.digest_records,cache/'file-digests')
def run(label,argv,expected=0):
 start=time.monotonic();r=subprocess.run(list(map(str,argv)),env=env,capture_output=True,timeout=180);(a.out/(label+'.stdout')).write_bytes(r.stdout);(a.out/(label+'.stderr')).write_bytes(r.stderr);commands.append(dict(label=label,argv=list(map(str,argv)),exit=r.returncode,seconds=time.monotonic()-start,stdout_sha256=sha(r.stdout),stderr_sha256=sha(r.stderr)));assert r.returncode==expected,(label,r.returncode,r.stdout,r.stderr);return r
out=a.out/'output'
def build(label):
 stable();r=run(label,[a.cli,'build','--project',P/'lang.toml','--out-dir',out,'--package-objects','--cache-dir',cache,'--opt','2','--format','json']);assert json.loads(r.stdout)['status']=='ok';run(label+'-program',[out/'program'],17);stats=json.loads((out/'cache-statistics.json').read_text());assert (stats['tool_digest_hits'],stats['tool_digest_bytes_read'])==(8,0),stats;cases.append(dict(label=label,statistics=stats,program_sha256=sha((out/'program').read_bytes())));return stats
class Reader:
 def __init__(self,b):self.b=b;self.p=0
 def n(self):end=self.b.index(b':',self.p);v=int(self.b[self.p:end]);self.p=end+1;return v
 def s(self):size=self.n();v=self.b[self.p:self.p+size];self.p+=size;return v
n=lambda v:str(v).encode()+b':';s=lambda v:n(len(v))+v
status='failed';injections=[]
try:
 cold=build('cold');assert cold['object_misses']==cold['opt_jobs']==1
 warm=build('warm');assert warm['object_hits']==1 and warm['object_misses']==warm['opt_jobs']==0
 key=json.loads((out/'package-records.json').read_text())[0]['object_key'];record=cache/'frontend'/key;reader=Reader(record.read_bytes()[65:]);assert reader.s()==b'wz-native-object/1' and reader.s()==key.encode();elf=reader.s();assert elf[:4]==b'\x7fELF';(a.out/'original-elf.o').write_bytes(elf)
 for label,payload in [('coff',a.coff.read_bytes()),('macho',a.macho.read_bytes()),('truncated-elf',elf[:32])]:
  body=s(b'wz-native-object/1')+s(key.encode())+s(payload);record.write_bytes(sha(body).encode()+b'\n'+body);injections.append(dict(label=label,payload_sha256=sha(payload),cache_record_sha256=sha(record.read_bytes())));stats=build(label+'-injected');assert (stats['object_hits'],stats['object_misses'],stats['opt_jobs'],stats['llc_jobs'])==(0,1,1,1),stats
 final=build('repaired-warm');assert final['object_hits']==1 and final['opt_jobs']==0;status='passed'
finally:
 (a.out/'report.json').write_text(json.dumps(dict(status=status,commands=commands,cases=cases,injections=injections,reused_format_objects={str(a.coff):sha(a.coff.read_bytes()),str(a.macho):sha(a.macho.read_bytes())},compiler_before=fixed,compiler_after=stable(),scope='fixed Wz candidate v2; no Wz compiler or format object rebuild'),indent=2)+'\n');print(json.dumps(dict(status=status,commands=len(commands),out=str(a.out))))
