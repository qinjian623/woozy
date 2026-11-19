#!/usr/bin/env python3
"""Check digest metadata invalidation against Python on small controlled files."""
import argparse,hashlib,json,os,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cpp',type=Path,required=True);p.add_argument('--wz',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);commands=[];sha=lambda b:hashlib.sha256(b).hexdigest()
def probe(label,executable,cache,file,hit):
 data=file.read_bytes();start=time.monotonic();argv=[str(executable),str(cache),str(file)];r=subprocess.run(argv,capture_output=True,timeout=60);(a.out/(label+'.stdout')).write_bytes(r.stdout);(a.out/(label+'.stderr')).write_bytes(r.stderr);j=json.loads(r.stdout);commands.append(dict(label=label,argv=argv,exit=r.returncode,seconds=time.monotonic()-start,result=j,stdout_sha256=sha(r.stdout),stderr_sha256=sha(r.stderr),probe_sha256=sha(executable.read_bytes())));assert r.returncode==0 and j['sha256']==sha(data),(label,r.stdout,r.stderr);assert (j['hits'],j['misses'],j['bytes_read'])==((1,0,0) if hit else (0,1,len(data))),(label,j);return j
status='failed'
try:
 for name,executable in [('cpp',a.cpp)]+([('wz',a.wz)] if a.wz else []):
  directory=a.out/name;directory.mkdir();cache=directory/'cache';file=directory/'工具 $ 空格.bin';file.write_bytes(b'alpha');probe(name+'-cold',executable,cache,file,False);probe(name+'-warm',executable,cache,file,True)
  before=file.stat();file.write_bytes(b'bravo');os.utime(file,ns=(before.st_atime_ns,before.st_mtime_ns));after=file.stat();assert before.st_mtime_ns==after.st_mtime_ns and before.st_ctime_ns!=after.st_ctime_ns;probe(name+'-preserved-mtime',executable,cache,file,False)
  replacement=directory/'replacement';replacement.write_bytes(b'bravo');os.utime(replacement,ns=(after.st_atime_ns,after.st_mtime_ns));old_inode=file.stat().st_ino;replacement.replace(file);assert file.stat().st_ino!=old_inode;probe(name+'-inode-replaced',executable,cache,file,False)
  one=directory/'target-one';two=directory/'target-two';one.write_bytes(b'cat');two.write_bytes(b'dog');link=directory/'tool-link';link.symlink_to(one);probe(name+'-symlink-one',executable,cache,link,False);probe(name+'-symlink-one-warm',executable,cache,link,True);link.unlink();link.symlink_to(two);probe(name+'-symlink-retarget',executable,cache,link,False)
  active=[p for p in (cache/'file-digests').glob('*.sha') if sha(b'dog').encode() in p.read_bytes()];assert len(active)==1;record=active[0];record.write_bytes(record.read_bytes()[:29]);probe(name+'-truncated-record',executable,cache,link,False)
  body=record.read_bytes()[65:].replace(b'wz-file-digest/1',b'wz-file-digest/X');record.write_bytes(sha(body).encode()+b'\n'+body);probe(name+'-wrong-format-record',executable,cache,link,False)
  body=record.read_bytes()[65:].replace(sha(b'dog').encode(),b'z'*64);record.write_bytes(sha(body).encode()+b'\n'+body);probe(name+'-invalid-digest-record',executable,cache,link,False);probe(name+'-after-repair-warm',executable,cache,link,True)
 if a.wz:
  file=a.out/'cross.bin';file.write_bytes(b'cross-language digest record');cache=a.out/'cross-cache';probe('cross-cpp-write',a.cpp,cache,file,False);probe('cross-wz-read',a.wz,cache,file,True)
  cache=a.out/'reverse-cache';probe('cross-wz-write',a.wz,cache,file,False);probe('cross-cpp-read',a.cpp,cache,file,True)
 status='passed'
finally:
 (a.out/'report.json').write_text(json.dumps(dict(status=status,commands=commands,scope='small controlled files only; real LLVM executables unchanged'),indent=2)+'\n');print(json.dumps(dict(status=status,commands=len(commands),out=str(a.out))))
