#!/usr/bin/env python3
"""A cached import token cannot be redirected to a later source string."""
import argparse,hashlib,json,os,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cli',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--libxml',type=Path,required=True);p.add_argument('--allow-known-bug',action='store_true');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
env=os.environ.copy();env['LD_LIBRARY_PATH']=str(a.libxml)+(':'+env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '');records=[];sha=lambda b:hashlib.sha256(b).hexdigest()
def run(label,argv,expected=0):
 start=time.monotonic();r=subprocess.run(list(map(str,argv)),env=env,capture_output=True,timeout=180);(a.out/(label+'.stdout')).write_bytes(r.stdout);(a.out/(label+'.stderr')).write_bytes(r.stderr);records.append(dict(label=label,argv=list(map(str,argv)),exit=r.returncode,seconds=time.monotonic()-start,stdout_sha256=sha(r.stdout),stderr_sha256=sha(r.stderr)));assert expected is None or r.returncode==expected,(label,r.returncode,r.stdout,r.stderr);return r
project=a.out/'project';(project/'src/pkgA').mkdir(parents=True);(project/'src/pkgB').mkdir()
(project/'lang.toml').write_text('schema_version=1\n[package]\nname="token_source"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
source='package app; import "app/pkgA" as A; const BAIT string = "app/pkgB"; func main() i32 { return A.value(); }\n';(project/'src/main.wz').write_text(source)
for name,value in [('pkgA',17),('pkgB',29)]:(project/'src'/name/'value.wz').write_text(f'package {name}; pub func value() i32 {{ return {value}; }}')
def build(label):
 out=a.out/label;r=run(label,[a.cli,'build','--project',project/'lang.toml','--out-dir',out,'--package-objects','--cache-dir',a.out/'cache','--opt','2','--format','json']);assert json.loads(r.stdout)['status']=='ok';v=run(label+'-program',[out/'program'],None);return v.returncode,json.loads((out/'cache-statistics.json').read_text())
class Reader:
 def __init__(self,b):self.b=b;self.p=0
 def n(self):i=self.b.index(b':',self.p);v=int(self.b[self.p:i]);self.p=i+1;return v
 def s(self):n=self.n();v=self.b[self.p:self.p+n];self.p+=n;return v
n=lambda v:str(v).encode()+b':'
s=lambda v:n(len(v))+v
status='failed'
try:
 assert build('cold')[0]==17
 changed=[]
 for file in (a.out/'cache/parse').glob('*.ast'):
  before=file.read_bytes();r=Reader(before[65:]);header=[r.s() for _ in range(4)];count=r.n();tokens=[]
  for _ in range(count):
   values=[r.n() for _ in range(6)];string=r.s() if values[-1] else None;trivia=[]
   for _ in range(r.n()):trivia.append([r.n() for _ in range(5)])
   tokens.append([values,string,trivia])
  found=[t for t in tokens if t[1]==b'app/pkgA'];bait=[t for t in tokens if t[1]==b'app/pkgB']
  if len(found)!=1 or len(bait)!=1:continue
  target=found[0];other=bait[0];target[0][1:5]=other[0][1:5];target[1]=other[1]
  body=b''.join(s(h) for h in header)+n(count)
  for vals,string,trivia in tokens:
   body+=b''.join(n(v) for v in vals)+(s(string) if string is not None else b'')+n(len(trivia))+b''.join(n(v) for t in trivia for v in t)
  body+=r.b[r.p:];file.write_bytes(sha(body).encode()+b'\n'+body);changed.append(dict(path=str(file),before=sha(before),after=sha(file.read_bytes())))
 assert len(changed)==1
 result,statistics=build('relocated-token');bug=result==29
 assert (bug if a.allow_known_bug else result==17 and statistics['parse_misses']==1),(result,statistics)
 if not bug:
  # The missed record has now been replaced from the actual parser. Alter only
  # the import AST span; token order remains valid and must still be rejected.
  files=list((a.out/'cache/parse').glob('*.ast'));changed_nodes=[]
  for file in files:
   before=file.read_bytes();r=Reader(before[65:]);header=[r.s() for _ in range(4)]
   if header[1].decode()!=str((project/'src/main.wz').resolve()):continue
   for _ in range(r.n()):
    vals=[r.n() for _ in range(6)]
    if vals[-1]:r.s()
    for _ in range(r.n()):
     for _ in range(5):r.n()
   start=r.p;count=r.n();root=r.n();nodes=[]
   for _ in range(count):
    vals=[r.n() for _ in range(5)];children=[r.n() for _ in range(r.n())];nodes.append([vals,children])
   rpos=source.index('"app/pkgB"');imports=[node for node in nodes if node[0][0]==2];assert len(imports)==1
   imports[0][0][1:3]=[rpos,rpos+len('"app/pkgB"')]
   body=r.b[:start]+n(count)+n(root)
   for vals,children in nodes:body+=b''.join(n(v) for v in vals)+n(len(children))+b''.join(n(v) for v in children)
   file.write_bytes(sha(body).encode()+b'\n'+body);changed_nodes.append(dict(path=str(file),before=sha(before),after=sha(file.read_bytes())))
  assert len(changed_nodes)==1
  span_result,span_stats=build('altered-ast-span');assert span_result==17 and span_stats['parse_misses']==1,(span_result,span_stats)
  warm_result,warm_stats=build('warm');assert warm_result==17 and warm_stats['parse_misses']==0 and warm_stats['parse_hits']==2,(warm_result,warm_stats)
 status='known_bug_reproduced' if bug else 'passed' 
finally:
 (a.out/'report.json').write_text(json.dumps(dict(status=status,commands=records,changes=locals().get('changed',[]),result=locals().get('result'),statistics=locals().get('statistics'),compiler_sha256=sha(a.cli.read_bytes())),indent=2)+'\n');print(json.dumps(dict(status=status,out=str(a.out))))
