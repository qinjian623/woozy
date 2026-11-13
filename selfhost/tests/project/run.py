#!/usr/bin/env python3
"""Real-FS project-loader differential suite; never copies production modules."""
from __future__ import annotations
import argparse
import difflib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

# Permission-restricted fixtures are restored in main() even after a failure.
RESTORE: list[tuple[Path, int]] = []


def restrict(path: Path) -> None:
    RESTORE.append((path, path.stat().st_mode & 0o7777))
    os.chmod(path, 0o000)


def remove_case(out: list[dict], directory: Path) -> None:
    """Drop a case that cannot be reproduced as root and remove its files."""
    out[:] = [item for item in out if item['manifest'] != str(directory/'lang.toml')]
    shutil.rmtree(directory)


def restore_permissions() -> None:
    for path, mode in RESTORE:
        try:
            os.chmod(path, mode)
        except OSError:
            pass
    RESTORE.clear()


def write(path: Path, data: str | bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data.encode() if isinstance(data, str) else data)


def manifest(base: Path, extra: str = '', source: str = 'src', hosted: bool = False) -> Path:
    p = base / 'lang.toml'
    write(p, 'schema_version=1\n[package]\nname="suite"\nsource_root=' + json.dumps(source) +
          '\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n' +
          ('[build.native]\nruntime="hosted"\n' if hosted else '') + extra)
    return p


def dep(name: str, kind: str, value: str, more: str = '') -> str:
    key = 'path' if kind == 'source' else 'binding'
    return f'\n[dependencies.{name}]\nkind={json.dumps(kind)}\n{key}={json.dumps(value)}\n' + more


def cases(base: Path) -> list[dict]:
    out = []
    def add(name: str, source='package main; func main() {}\n', extra='', *, ok=True,
            state=None, codes=(), std=None, binding='-', hosted=False, source_root='src'):
        directory=base/name
        p=manifest(directory, extra, source_root, hosted)
        (directory/'src').mkdir(exist_ok=True)
        if source is not None: write(directory/'src/main.wz',source)
        item={'name':name,'manifest':str(p),'std':str(ROOT/'stdlib') if std is None else str(std),
              'binding':str(binding),'ok':ok,'state':state or ('complete' if ok else 'failed'),
              'codes':list(codes)}
        out.append(item)
        return directory,item
    d,c=add('sorted_multifile',b'\xef\xbb\xbfpackage main;\r\n// \xe6\xb1\x89\xf0\x9f\x98\x80\r\nfunc main() {}\r\n')
    write(d/'src/z.wz','package main; const z i32=2;\n')
    write(d/'src/a.wz','package main; const a i32=1;\n')
    os.symlink('a.wz',d/'src/copy.wz')
    write(d/'src/汉.wz','package main; // Unicode filename\n')
    write(d/'src/ignored.WZ','not source');write(d/'src/.wz','not source')
    (d/'src/directory.wz').mkdir()
    d,c=add('backend_python_word32')
    manifest_text=(d/'lang.toml').read_text().replace('backend="native"\nword_bits=64','backend="python"\nword_bits=32')
    write(d/'lang.toml',manifest_text)
    add('unicode_import_diagnostic','\ufeffpackage main;\r\n// 汉😀\r\nimport "missing";\r\n',ok=False,state='recovered',codes=['E_IMPORT_NOT_FOUND'])
    d,c=add('package_mismatch',ok=False,codes=['E_PACKAGE_MISMATCH'])
    write(d/'src/z.wz','package other;\n')
    d,c=add('module_empty',source=None,ok=False,codes=['E_MODULE_EMPTY'])
    write(d/'src/readme.txt','ignored\n')
    for name,data,code in [('invalid_utf8_source',b'package main;\n\xff','E_INVALID_SOURCE'),
                           ('raw_nul_source',b'package main;\n\0','E_INVALID_SOURCE'),
                           ('lone_cr_source',b'package main;\r','E_INVALID_SOURCE'),
                           ('lexer_failure','package main; const x="unterminated;','E_LEX_INVALID_LITERAL'),
                           ('parser_recovery','package main; func main( { return; }','E_PARSE_EXPECTED_TOKEN')]:
        add(name,data,ok=False,codes=[code])
    d,c=add('imports_multiple_errors','package main; import "bad/../x"; import "missing"; import "absent/lib"; import "app/no_directory"; import "app/leaf"; func main() {}\n',ok=False,state='recovered',codes=['E_IMPORT_PATH','E_IMPORT_NOT_FOUND','E_INPUT_IO'])
    write(d/'src/leaf/lib.wz','package leaf;\n')
    d,c=add('source_symlink_alias','package main; import "first" as f; import "second" as s;\n',dep('first','source','alias')+dep('second','source','real'))
    write(d/'real/lib.wz','package lib;\n');os.symlink('real',d/'alias')
    d,c=add('app_roots','package main; import "app/helper"; import "vendor";\n',dep('vendor','source','vendor'))
    write(d/'src/helper/h.wz','package roothelper;\n')
    manifest(d/'vendor');write(d/'vendor/src/lib.wz','package vendor; import "app/helper";\n');write(d/'vendor/src/helper/h.wz','package vendorhelper;\n')
    d,c=add('dependency_without_manifest','package main; import "raw/sub";\n',dep('raw','source','raw'))
    write(d/'raw/sub/lib.wz','package raw;\n')
    d,c=add('dependency_does_not_inherit','package main; import "raw";\n',dep('raw','source','raw')+dep('shared','source','shared'),ok=False,state='recovered',codes=['E_IMPORT_NOT_FOUND'])
    write(d/'raw/lib.wz','package raw; import "shared";\n');write(d/'shared/lib.wz','package shared;\n')
    d,c=add('absolute_dependency','package main; import "absolute";\n')
    write(d/'actual/lib.wz','package absolute;\n');manifest(d,dep('absolute','source',str(d/'actual')))
    d,c=add('dependency_symlink_dotdot','package main; import "vendor";\n',dep('vendor','source','jump/../dep'))
    (d/'physical/inside').mkdir(parents=True);os.symlink('physical/inside',d/'jump')
    write(d/'physical/dep/lib.wz','package correct;\n');write(d/'dep/lib.wz','package wrong;\n')
    d,c=add('absolute_manifest_source_root','package main; import "vendor";\n',dep('vendor','source','vendor'))
    manifest(d/'vendor',source=str(d/'actual'));write(d/'actual/lib.wz','package actual;\n')
    d,c=add('logical_claim_conflict','package main; import "left"; import "right";\n',dep('left','source','left')+dep('right','source','right'),ok=False,state='recovered',codes=['E_MODULE_CONFLICT'])
    for side in ('left','right'):
        manifest(d/side,dep('shared','source','shared'));write(d/side/'src/lib.wz',f'package {side}; import "shared";\n');write(d/side/'shared/lib.wz',f'package {side}shared;\n')
    d,c=add('cycle','package main; import "app/a";\n',ok=False,state='recovered',codes=['E_IMPORT_CYCLE'])
    write(d/'src/a/lib.wz','package a; import "app/b";\n');write(d/'src/b/lib.wz','package b; import "app/a";\n')
    add('self_cycle','package main; import "app";\n',ok=False,state='recovered',codes=['E_IMPORT_CYCLE'])
    add('std_source','package main; import "std/option";\n')
    add('std_missing_module','package main; import "std/not_provided_here";\n',ok=False,state='recovered',codes=['E_IMPORT_NOT_FOUND'])
    example=ROOT/'contracts/python_bindings.example.json'
    add('std_binding','package main; import "std/py";\n',std='-',binding=example)
    add('std_explicit_conflict','package main; import "std/py";\n',binding=example,ok=False,state='recovered',codes=['E_MODULE_CONFLICT'])
    add('std_binding_missing_module','package main; import "std/not_provided_here";\n',std='-',binding=example,ok=False,state='recovered',codes=['E_BINDING_INVALID'])
    d,c=add('python_exports','package main; import "torch/nn"; import "numpy"; import "torch/optim";\n',dep('torch','python',str(example),'host_package="torch"\n')+dep('numpy','python',str(example),'host_package="numpy"\n'))
    add('python_host_missing','package main; import "torch/nn";\n',dep('torch','python',str(example),'host_package="no_such_host"\n'),ok=False,state='recovered',codes=['E_BINDING_INVALID'])
    # Two function candidates in declaration order, independent of name sorting.
    d,c=add('python_overloads','package main; import "py";\n',dep('py','python','binding.json','host_package="host"\n'))
    binding={'schema_version':1,'contract':'compiler-dev/1','review_status':'draft_requires_conformance','allowed_backends':['python'],'package_requirements':[{'package':'host','version_policy':'test'}],'modules':[{'logical_path':'py','host_module':'host','provider':'python_package'}],'types':[],'values':[{'module':'py','name':'z','type':'i64','host_attribute':['z']}],'callables':[],'conformance_required':['check']}
    for suffix,typ in [('wide','i64'),('narrow','i32')]:
        binding['callables'].append({'id':'host.run.'+suffix,'module':'py','name':'run','call_kind':'function','owner_type':None,'host_attribute':['run'],'parameters':[{'name':'x','type':typ,'kind':'positional_or_keyword','optional':False,'mode':'ordinary','escapes':'no'}],'return_type':'Unit','error_type':None,'ownership':{'return':'unit','storage':'no_tensor_storage'},'effects':[],'receiver_escapes':'not_applicable'})
    write(d/'binding.json',json.dumps(binding,ensure_ascii=False,indent=2)+'\n')
    # A valid non-hosted ABI bundle, with two modules to check first use order.
    def c_bundle(prefix):
        return {'schema_version':2,'contract':'compiler-dev/1','abi':'c','target':'x86_64-linux-gnu','requires_hosted':False,'modules':[{'logical_path':prefix+'/'+x,'provider':'native_library','library':'module'+x.upper()} for x in ('a','b')],'types':[{'id':prefix+'.Handle','module':prefix+'/a','name':'Handle','kind':'opaque','c_name':'handle_t'}],'callables':[{'id':prefix+'.'+x,'module':prefix+'/'+x,'name':'read_'+x,'symbol':prefix+'_read_'+x,'parameters':[],'result':{'type':'i32','ownership':'value'}} for x in ('a','b')]}
    d,c=add('native_exports_first_use','package main; import "c/b"; import "c/a"; import "c/b" as again; import "d/a";\n',dep('c','native','c.json','libraries=["first","moduleB"]\nlibrary_dirs=["lib2","lib1","aliaslib1"]\n')+dep('d','native','d.json','libraries=["first","last"]\nlibrary_dirs=["lib1"]\n'))
    write(d/'c.json',json.dumps(c_bundle('c'),indent=2)+'\n');write(d/'d.json',json.dumps(c_bundle('d'),indent=2)+'\n');(d/'lib1').mkdir();(d/'lib2').mkdir();os.symlink('lib1',d/'aliaslib1')
    d,c=add('native_hosted_required','package main; import "c/a";\n',dep('c','native','c.json','libraries=[]\n'),ok=False,state='recovered',codes=['E_CAPABILITY_UNAVAILABLE'])
    cb=c_bundle('c');cb['requires_hosted']=True;write(d/'c.json',json.dumps(cb,indent=2)+'\n')
    d,c=add('native_hosted','package main; import "capturing/lib";\n',dep('capturing','native',str(ROOT/'tests/native/c_retained_capturing/bindings/capturing.json'),'libraries=[]\n'),hosted=True)
    d,c=add('invalid_utf8_unrelated_filename')
    fd=os.open(os.fsencode(d/'src')+b'/\xff.txt',os.O_CREAT|os.O_WRONLY,0o600);os.write(fd,b'ignored');os.close(fd)
    d,c=add('loop_txt_symlink',ok=False,codes=['E_INPUT_IO'])
    os.symlink('loop.txt',d/'src/loop.txt')
    # Filesystem failure text: the message keeps the failing operation, the
    # generic errno wording and the path spelling that was passed in. The
    # oracle and Wz are compared field by field, including the import span.
    d,c=add('missing_module_directory','package main; import "app/absent/deep";\n',ok=False,state='recovered',codes=['E_INPUT_IO'])
    d,c=add('missing_dependency_directory','package main; import "dep/x";\n',dep('dep','source','absent'),ok=False,state='recovered',codes=['E_INPUT_IO'])
    d,c=add('dependency_not_a_directory','package main; import "dep/x";\n',dep('dep','source','notdir.txt'),ok=False,state='recovered',codes=['E_INPUT_IO'])
    write(d/'notdir.txt','not a directory\n')
    d,c=add('dependency_symlink_loop','package main; import "dep/x";\n',dep('dep','source','loop'),ok=False,state='recovered',codes=['E_INPUT_IO'])
    os.symlink('loop',d/'loop')
    d,c=add('unicode_space_missing_directory','package main; import "dep/x";\n',dep('dep','source','dir with space/模块 缺'),ok=False,state='recovered',codes=['E_INPUT_IO'])
    d,c=add('unicode_space_module','package main; import "dep";\n',dep('dep','source','dir with space/模块 名'))
    write(d/'dir with space/模块 名/a b.wz','package spaced;\n')
    write(d/'dir with space/模块 名/ü 文.wz','package spaced;\n')
    d,c=add('permission_denied_directory','package main; import "app/secret/deep";\n',ok=False,state='recovered',codes=['E_INPUT_IO'])
    (d/'src/secret/deep').mkdir(parents=True)
    write(d/'src/secret/deep/lib.wz','package hidden;\n')
    if os.geteuid()==0:
        print('skip: denied-directory fixture is readable as root',file=sys.stderr)
        remove_case(out,d)
    else:
        restrict(d/'src/secret')
    d,c=add('loop_entry_in_dependency','package main; import "dep";\n',dep('dep','source','dep'),ok=False,state='recovered',codes=['E_INPUT_IO'])
    write(d/'dep/main.wz','package dep;\n')
    os.symlink('loop.wz',d/'dep/loop.wz')
    d,c=add('dangling_wz_symlink')
    os.symlink('nothing.wz',d/'src/dangling.wz')
    # An input open failure adds no snapshot; a file read earlier keeps its
    # snapshot and the failing file is never retained.
    d,c=add('unreadable_input_file','package main;\n',ok=False,codes=['E_INPUT_IO'])
    write(d/'src/z_broken.wz','package main;\n')
    if os.geteuid()==0:
        print('skip: unreadable-input fixture is readable as root',file=sys.stderr)
        remove_case(out,d)
    else:
        restrict(d/'src/z_broken.wz')
    d,c=add('unreadable_module_directory','package main;\n',ok=False,codes=['E_INPUT_IO'],source_root='blocked')
    write(d/'blocked/main.wz','package main;\n')
    if os.geteuid()==0:
        print('skip: unreadable-directory fixture is readable as root',file=sys.stderr)
        remove_case(out,d)
    else:
        restrict(d/'blocked')
    return out


def run(command: list[str], *, timeout: float, env=None) -> subprocess.CompletedProcess:
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env,timeout=timeout)
    if result.returncode: raise RuntimeError(f'command failed ({result.returncode}): {command!r}\n{result.stdout}\n{result.stderr}')
    return result


def require(condition: bool, message: str) -> None:
    if not condition: raise AssertionError(message)


def validate(case: dict, document: dict) -> None:
    if document['success'] != case['ok'] or document['state'] != case['state']:
        raise AssertionError(f"{case['name']}: expected success={case['ok']} state={case['state']}; got {document['success']} {document['state']} diagnostics={document['diagnostics']}")
    actual={d['code'] for d in document['diagnostics']}
    if not set(case['codes']) <= actual:
        raise AssertionError(f"{case['name']}: missing codes {set(case['codes'])-actual}; got {actual}")
    if (document['project'] is not None) != document['has_value']:
        raise AssertionError('a no-value failure printed an internal partial Project')
    require(document['has_value']==(case['state'] in ('complete','recovered')), 'unexpected value availability for phase')
    project=document['project']
    if project is not None:
        if case['name']=='source_symlink_alias':
            require(project['modules'][0]['imports'][0]['target']==project['modules'][0]['imports'][1]['target'], 'symlink aliases did not reuse a module ID')
        if case['name']=='app_roots': require(len(project['modules'])==4, 'app roots were conflated')
        if case['name']=='imports_multiple_errors':
            require(len(document['diagnostics'])>=4 and project['modules'][0]['imports'][-1]['target']!=0, 'imports stopped after an earlier error')
        if case['name']=='dependency_symlink_dotdot': require(project['modules'][1]['package_name']=='correct', 'symlink/.. order changed')
        if case['name']=='python_exports':
            require(len(project['bindings'])==1 and len(project['foreign_bindings'])==26, 'Python bundle/declaration counts changed')
            digest=project['bindings'][0]['sha256']
            require(all(digest in x['stable_identity'] for x in project['foreign_bindings']), 'Python persistent identities lost the bundle digest')
        if case['name']=='python_overloads':
            export=next(x for x in project['modules'][1]['foreign_exports'] if x['name']=='run')
            require(export['candidates']==[1,2], 'callable candidate order changed')
        if case['name']=='native_exports_first_use':
            require(project['c_libraries']==['first','moduleB','moduleA','last'], 'library first-use order changed')
            require(len(project['c_bindings'])==2 and [x['id'] for x in project['c_types']]==[1,2], 'C bundle IDs were not rebased')
            require(len(project['c_library_dirs'])==2, 'canonical library directory aliases were not deduplicated')
        if case['name']=='backend_python_word32': require(project['backend']=='python' and project['word_bits']==32, 'nondefault project fields were lost')
    for index,snapshot in enumerate(document['sources'],1):
        require(snapshot['id']==index and snapshot['file_id']==index and snapshot['revision_id']==index, 'source ID or revision order changed')


def io_messages(document: dict) -> list[dict]:
    return [{'index':index,'message':diagnostic['message']}
            for index,diagnostic in enumerate(document['diagnostics'])
            if diagnostic['code']=='E_INPUT_IO']


def main() -> int:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--woozyc',type=Path,default=ROOT/'build/woozy-stdlib-native/woozyc')
    p.add_argument('--oracle-build-dir',type=Path,default=ROOT/'build/woozy-stdlib-native')
    p.add_argument('--cxx',default=os.environ.get('CXX','g++'))
    p.add_argument('--work-dir',type=Path,help='parent for a fresh results directory')
    p.add_argument('--oracle-only',action='store_true',help='build/run only the C++ oracle; never counts Wz as passed')
    p.add_argument('--opt',type=int,choices=(0,2),action='append')
    p.add_argument('--timeout',type=float,default=240)
    args=p.parse_args()
    parent=args.work_dir.expanduser().resolve() if args.work_dir else None
    if parent: parent.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='woozy-project-suite-',dir=parent)).resolve()
    suite=cases(work/'cases');write(work/'cases.json',json.dumps(suite,ensure_ascii=False,indent=2)+'\n')
    # libstdc++ reports errno text through strerror, so pin the message locale
    # for both sides; wzcc always uses the C/English table.
    env=dict(os.environ);env['LC_ALL']='C';toolchains=ROOT.parent/'toolchains'
    env['LD_LIBRARY_PATH']=':'.join([str(toolchains/'llvm-22.1.8/lib'),str(toolchains/'libxml2-legacy-2.13.9/usr/lib'),env.get('LD_LIBRARY_PATH','')])
    oracle=work/'oracle'
    build=[args.cxx,'-std=c++20','-O2','-DNDEBUG','-I'+str(ROOT/'include'),str(HERE/'oracle.cpp'),str(args.oracle_build_dir/'liblang_modules.a'),str(args.oracle_build_dir/'liblang_frontend.a'),'-ljson-c','-ltomlplusplus','-o',str(oracle)]
    result=run(build,timeout=args.timeout,env=env);write(work/'oracle-build.stdout',result.stdout);write(work/'oracle-build.stderr',result.stderr)
    expected={}
    for case in suite:
        result=run([str(oracle),case['manifest'],case['std'],case['binding']],timeout=args.timeout,env=env)
        document=json.loads(result.stdout);validate(case,document);expected[case['name']]=document
        write(work/'cpp'/f"{case['name']}.json",json.dumps(document,ensure_ascii=False,indent=2)+'\n')
        write(work/'cpp'/f"{case['name']}.stderr",result.stderr)
    print(f'C++ oracle: {len(suite)} real-FS cases passed; artifacts: {work}',flush=True)
    if args.oracle_only:
        write(work/'summary.json',json.dumps({'cpp_cases':len(suite),'wz_status':'not_run','reason':'--oracle-only'},indent=2)+'\n')
        return 0
    if not (ROOT/'selfhost/src/wzcc/project').is_dir():
        raise RuntimeError('production wzcc/project is not integrated; C++ passed, Wz was not run')
    staged=work/'driver';write(staged/'lang.toml',(HERE/'lang.toml').read_text());write(staged/'src/main.wz',(HERE/'src/main.wz').read_text())
    # Point to real production packages. No copied/stale model/project/bindings.
    os.symlink(ROOT/'selfhost/src/wzcc',staged/'src/wzcc',target_is_directory=True)
    completed=[]
    for opt in args.opt or [0,2]:
        output=work/f'out-o{opt}'
        command=[str(args.woozyc),'build','--project',str(staged/'lang.toml'),'--out-dir',str(output),'--opt',str(opt)]
        result=run(command,timeout=args.timeout,env=env);write(work/f'build-o{opt}.stdout',result.stdout);write(work/f'build-o{opt}.stderr',result.stderr)
        human=[]
        for case in suite:
            result=run([str(output/'program'),case['manifest'],case['std'],case['binding']],timeout=args.timeout,env=env)
            document=json.loads(result.stdout);validate(case,document)
            write(work/f'wz-o{opt}'/f"{case['name']}.json",json.dumps(document,ensure_ascii=False,indent=2)+'\n')
            actual_io=io_messages(document);expected_io=io_messages(expected[case['name']])
            if document!=expected[case['name']]:
                left=json.dumps(expected[case['name']],ensure_ascii=False,indent=2,sort_keys=True).splitlines()
                right=json.dumps(document,ensure_ascii=False,indent=2,sort_keys=True).splitlines()
                diff='\n'.join(difflib.unified_diff(left,right,fromfile='C++',tofile=f'Wz O{opt}'))
                write(work/f'difference-o{opt}-{case["name"]}.diff',diff+'\n')
                raise AssertionError(f"{case['name']} O{opt} differs:\n{diff[:18000]}")
            if actual_io or expected_io: human.append({'case':case['name'],'cpp':expected_io,'wz':actual_io})
        write(work/f'io-messages-o{opt}.json',json.dumps(human,ensure_ascii=False,indent=2)+'\n')
        completed.append({'opt':opt,'cases':len(suite)});print(f'Wz O{opt}: {len(suite)} cases exactly matched, including E_INPUT_IO text and spans',flush=True)
    write(work/'summary.json',json.dumps({'cpp_cases':len(suite),'wz':completed,'wz_status':'passed'},indent=2)+'\n')
    return 0


if __name__=='__main__':
    try: raise SystemExit(main())
    except (RuntimeError,AssertionError,subprocess.TimeoutExpired,ValueError,OSError) as error:
        print(f'FAIL: {error}',file=sys.stderr);raise SystemExit(1)
    finally: restore_permissions()
