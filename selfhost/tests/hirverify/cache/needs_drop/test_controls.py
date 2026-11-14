"""Pure controls for finite JSON and both exec trace forms; launch nothing."""
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
        require('no external launch attempts',launches==[])
    result={'status':'passed','checks':checks,'external_launch_attempts':launches}
    print(json.dumps(result));return result
if __name__=='__main__':main()
