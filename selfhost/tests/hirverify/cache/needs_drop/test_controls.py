"""Pure controls for finite JSON, both exec trace forms and stage0 routing; launch nothing."""
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
from unittest.mock import patch
sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
for parent in HERE.parents:
    if (parent/'selfhost/corpus.py').is_file():
        sys.path.insert(0,str(parent/'selfhost'))
        break
spec=importlib.util.spec_from_file_location('needs_drop_control_runner',HERE/'run.py')
runner=importlib.util.module_from_spec(spec)
checks=[]
def require(name,value):
    if not value:raise AssertionError(name)
    checks.append(name)
def rejects(name,call):
    try:call()
    except (RuntimeError,ValueError):require(name,True)
    else:raise AssertionError(name)
def main():
    launches=[]
    def blocked(*a,**k):
        launches.append(str(a[0] if a else '?'))
        raise AssertionError('external launch forbidden')
    with contextlib.ExitStack() as stack:
        for module,names in [(subprocess,('Popen','run','call','check_call','check_output')),(os,('system','posix_spawn','posix_spawnp','execv','execve','spawnv','spawnve')),(socket,('socket','create_connection'))]:
            for name in names:
                if hasattr(module,name):stack.enter_context(patch.object(module,name,blocked))
        spec.loader.exec_module(runner)
        require('finite JSON accepted',runner.parse_json_report('ok',b'{"status":"ok","diagnostics":[],"ns":1}')['ns']==1)
        for value in ('NaN','Infinity','-Infinity'):
            rejects('nonfinite '+value,lambda value=value:runner.parse_json_report('bad',('{"status":"ok","diagnostics":[],"ns":'+value+'}').encode()))
        rejects('malformed JSON',lambda:runner.parse_json_report('bad',b'{'))
        with tempfile.TemporaryDirectory() as temporary:
            p=Path(temporary)/'exec.log'
            def trace(text):p.write_text(text);return runner.parse_exec_log(p,'/program')
            require('execve accepted',trace('1 execve("/program", ["/program"], 0x0) = 0\n')==['/program'])
            require('execveat AT_FDCWD accepted',trace('1 execveat(AT_FDCWD, "/program", ["/program"], 0x0, 0) = 0\n')==['/program'])
            for call in ['2 execveat(AT_FDCWD, "/unexpected/tool", [], 0x0, 0) = 0\n','2 execve("/unexpected/tool", [], 0x0) = -1 ENOENT (No such file or directory)\n','2 execveat(4, "", [], 0x0, AT_EMPTY_PATH) = 0\n']:
                rejects('unexpected or unresolved exec '+str(len(checks)),lambda call=call:trace('1 execve("/program", [], 0x0) = 0\n'+call))
            rejects('empty trace',lambda:trace(''))
        with tempfile.TemporaryDirectory() as temporary:
            base=Path(temporary)
            helper=base/'lang_native_helper';helper.write_bytes(b'helper\n')
            native=base/'liblang_native_runtime.a';native.write_bytes(b'native\n')
            hosted=base/'liblang_hosted_runtime.a';hosted.write_bytes(b'hosted\n')
            stdlib=base/'stdlib';stdlib.mkdir();(stdlib/'std.wz').write_bytes(b'package fixture;\n')
            files=runner.tree_hashes(stdlib,('.wz',))
            stage0={'build':{'macros':{'LANG_NATIVE_HELPER':str(helper),'LANG_NATIVE_RUNTIME':str(native),'LANG_HOSTED_RUNTIME':str(hosted),'LANG_STD_SOURCE_ROOT':str(stdlib)},'build_ninja':str(base/'build.ninja'),'build_ninja_sha256':'0'*64,'cmake_cache':str(base/'CMakeCache.txt'),'cmake_cache_sha256':'1'*64},'stdlib':{'files':files},'uses_copied_resources':False}
            routed=runner.verify_stage0_routing(stage0,helper,base,stdlib,files)
            require('stage0 routing accepted',routed['macros']['LANG_NATIVE_HELPER']==str(helper))
            wrong=json.loads(json.dumps(stage0));wrong['build']['macros']['LANG_NATIVE_HELPER']=str(stdlib/'std.wz')
            rejects('stage0 helper routing mismatch',lambda:runner.verify_stage0_routing(wrong,helper,base,stdlib,files))
            wrong=json.loads(json.dumps(stage0));wrong['stdlib']['files']={'std.wz':'0'*64}
            rejects('stage0 stdlib routing mismatch',lambda:runner.verify_stage0_routing(wrong,helper,base,stdlib,files))
        require('no external launch attempts',launches==[])
    result={'status':'passed','checks':checks,'external_launch_attempts':launches}
    print(json.dumps(result));return result
if __name__=='__main__':main()
