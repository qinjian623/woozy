from contextlib import chdir, redirect_stderr
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'selfhost'))
SCRIPT = REPO / 'selfhost/bootstrap.py'
spec = importlib.util.spec_from_file_location('bootstrap_tested', SCRIPT)
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)

foreign_make = {
    'MAKEFLAGS': '-n -j9 --jobserver-auth=3,4', 'GNUMAKEFLAGS': '-n',
    'MAKEFILES': 'unexpected.mk', 'MAKEOVERRIDES': 'JOBS=9',
    'MFLAGS': '-n', 'MAKELEVEL': '2', 'MAKE_RESTARTS': '1',
    'WOOZY_MAKE': '/missing/make',
}
with tempfile.TemporaryDirectory(dir=REPO) as tmp:
    root = Path(tmp)
    (root / 'selfhost/src').mkdir(parents=True)
    (root / 'selfhost/src/main.wz').write_text('source')
    (root / 'selfhost/lang.toml').write_text('manifest')
    (root / 'stdlib').mkdir()
    (root / 'stdlib/main.wz').write_text('stdlib')
    (root / 'runtime/python').mkdir(parents=True)
    for name in boot.PYTHON_RUNTIME:
        (root / 'runtime/python' / name).write_text(name)
    runtime = root / 'runtime-archives'
    runtime.mkdir()
    for name in boot.RUNTIME_ARCHIVES:
        (runtime / name).write_text(name)
    tools = root / 'llvm/bin'
    tools.mkdir(parents=True)
    for name in boot.TOOL_NAMES:
        (tools / name).write_text(name)
    seed = root / 'seed'
    helper = root / 'helper'
    seed.write_text('seed')
    helper.write_text('helper')
    stage0 = {'seed': {'path': str(seed), 'sha256': boot.digest(seed)},
              'helper': {'path': str(helper)}}
    (root / 'bin').mkdir()
    for name in ('strace', 'bin/strace', 'bin/named-tracer'):
        (root / name).touch(mode=0o755)
    calls = []
    tracers = []
    unexpected_executable = None

    def process(command, cwd, env, timeout):
        actual = command[command.index('build') - 1:]
        if '--source-make' in actual:
            assert not foreign_make.keys() & env.keys()
            assert env['PATH'] == '/usr/bin:/bin'
        calls.append(actual)
        output = Path(actual[actual.index('--out-dir') + 1])
        assert cwd == output.parents[1] / 'source' and timeout == 10
        output.mkdir()
        binary = output / 'program'
        binary.write_text('identical compiler')
        binary.chmod(0o755)
        if command != actual:
            tracers.append(command[0])
            trace = Path(command[command.index('-o') + 1])
            executables = [actual[0], tools / 'opt', tools / 'clang']
            if unexpected_executable is not None:
                executables.append(unexpected_executable)
            trace.write_text('\n'.join(f'execve("{x}", [], 0) = 0' for x in executables))
        return dict(stdout=b'{"status":"ok"}', stderr=b'', exit=0, timed_out=False,
                    cleanup_complete=True, abandoned_pipes=False)

    def invoke(out, *options):
        argv = [str(SCRIPT), '--source-make', '--out-dir', str(out), '--stage0', str(seed),
                '--toolchain', str(root / 'llvm'), '--runtime-dir', str(runtime),
                '--libxml-dir', str(root), '--opt=0', '--jobs=3', '--timeout=10', *options]
        with mock.patch.object(sys, 'argv', argv):
            return boot.main()

    with chdir(root), mock.patch.dict(os.environ, foreign_make | {'PATH': 'bin'}), \
         mock.patch.object(boot, 'ROOT', root), \
         mock.patch.object(boot, 'inspect_stage0', return_value=stage0) as inspect, \
         mock.patch.object(boot, '_compare_stage0_inputs'), \
         mock.patch.object(boot, '_frozen_inputs', return_value={'files': {}, 'paths': {}}), \
         mock.patch.object(boot, '_run_process_impl', side_effect=process), \
         mock.patch.object(boot.subprocess, 'run',
                           side_effect=AssertionError('unexpected bootstrap subprocess')):
        for value in ('nan', 'inf', '-inf', '0', '-1'):
            out = root / 'invalid-timeout'
            stderr = io.StringIO()
            with redirect_stderr(stderr):
                try:
                    invoke(out, f'--timeout={value}')
                except SystemExit as error:
                    assert error.code == 2
                else:
                    raise AssertionError(f'invalid timeout accepted: {value}')
            assert 'timeout must be a positive finite number' in stderr.getvalue()
            assert not out.exists()
        for name in ('./named-tracer', './helper'):
            out = root / ('missing-' + Path(name).name)
            stderr = io.StringIO()
            with redirect_stderr(stderr):
                assert invoke(out, '--stages=2', '--strace', name) == 1
            assert 'executable strace is required' in stderr.getvalue()
            assert not (out / 'source').exists()
        assert not calls and not inspect.called

        assert invoke(root / 'out', '--strace=./strace') == 0
        assert tracers == [str(root / 'strace')] * 2
        assert invoke(root / 'bare', '--stages=2', '--strace=named-tracer') == 0
        assert tracers[-1] == str(root / 'bin/named-tracer')
        assert invoke(root / 'default', '--stages=2') == 0
        assert tracers[-1] == str(root / 'bin/strace')
        with mock.patch.dict(os.environ, {'PATH': 'missing-bin'}):
            assert invoke(root / 'single', '--stages=1') == 0
        assert len(tracers) == 4
        for unexpected_executable in ('/usr/bin/make', '/bin/sh', '/usr/bin/mkdir', '/usr/bin/gcc'):
            out = root / f'reject-{Path(unexpected_executable).name}'
            with redirect_stderr(io.StringIO()):
                assert invoke(out, '--stages=2') == 1
            report = json.loads((out / 'result.json').read_text())
            assert report['failed_stage'] == 'stage2'
            failure = report['stages'][1]
            assert failure['status'] == 'failed'
            assert str(Path(unexpected_executable).resolve()) in failure['trace_error']
        assert all(call.kwargs['package_objects'] for call in inspect.call_args_list)
    calls = calls[:3]
    report = json.loads((root / 'out/result.json').read_text())
    assert report['status'] == 'ok' and report['stage23_binary_equal']
    assert '--package-objects' in calls[0] and '--source-make' not in calls[0]
    assert '--jobs' not in calls[0]
    assert all('--source-make' in c and '--cache-dir' not in c for c in calls[1:])
    assert all(c[c.index('--jobs') + 1] == '3' for c in calls[1:])
    assert all(c[c.index('--opt') + 1] == '0' for c in calls)
    assert 'source_make_tools' not in report
    for stage, command in zip(report['stages'][1:], calls[1:]):
        assert set(stage['executables']) == {command[0], str(tools / 'opt'), str(tools / 'clang')}
    # The optional Make entry point only forwards argv to Python.
    make = shutil.which('make')
    if make is not None:
        entry_dir = root / 'repo with "quotes" and $HOME' / 'selfhost'
        entry_dir.mkdir(parents=True)
        makefile = entry_dir / 'Makefile'
        makefile.write_bytes((REPO / 'selfhost/Makefile').read_bytes())
        recorder = entry_dir / 'bootstrap.py'
        recorder.write_text('import json, sys\nprint(json.dumps([sys.flags.isolated, *sys.argv]))\n')
        env = {k: v for k, v in os.environ.items()
               if k not in foreign_make and k not in {'OUT', 'STAGES', 'OPT', 'JOBS'}}
        env.update(dict.fromkeys(('STAGE0', 'STRACE', 'RUNTIME_DIR', 'TIMEOUT'), ''))
        python = shlex.quote(sys.executable)

        def make_argv(entry, *options, cwd=root):
            process = subprocess.run([make, '--no-print-directory', '-j4',
                                      *entry, 'bootstrap', *options],
                                     cwd=cwd, env=env, capture_output=True, text=True,
                                     check=True, timeout=10)
            return json.loads(process.stdout)

        path = str(root / 'something "quoted" $HOME $(NOT_A_MAKE_VAR) $$')
        assert make_argv(['-f', str(makefile)], f'PYTHON={python} -I',
                         f'OUT={path} out', f'STAGE0={path} seed', f'STRACE={path} trace',
                         f'RUNTIME_DIR={path} runtime', 'STAGES=2', 'OPT=2', 'JOBS=3',
                         'TIMEOUT=15') == [
            1, str(recorder), '--source-make', '--out-dir', path + ' out',
            '--stages', '2', '--opt', '2', '--jobs', '3', '--stage0', path + ' seed',
            '--strace', path + ' trace', '--runtime-dir', path + ' runtime', '--timeout', '15',
        ]
        assert make_argv(['-C', 'selfhost'], f'PYTHON={python}', cwd=entry_dir.parent) == [
            0, './bootstrap.py', '--source-make', '--out-dir', str(root / 'woozy-bootstrap'),
            '--stages', '3', '--opt', '0', '--jobs', '1',
        ]
        preload = root / 'valid-preload.mk'
        preload.write_text('# Intentionally no rules or overrides.\n')
        env['MAKEFILES'] = str(preload)
        for entry, cwd, script in (
            (['-C', 'selfhost'], entry_dir.parent, './bootstrap.py'),
            (['-f', str(makefile)], root, str(recorder)),
        ):
            assert make_argv(entry, f'PYTHON={python}', cwd=cwd) == [
                0, script, '--source-make', '--out-dir', str(root / 'woozy-bootstrap'),
                '--stages', '3', '--opt', '0', '--jobs', '1',
            ]
print('bootstrap source-make controls passed')
