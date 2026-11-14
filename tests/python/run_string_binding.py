#!/usr/bin/env python3
"""Check dynamic Python strings through the test binding bundle.

The legacy CMake invocation is unchanged: ``--cli`` is the C++ reference CLI
and ``--helper`` is the matching C++ ``lang_python_emit`` helper.

The public-API/candidate mode adds ``--candidate-cli`` and either
``--api-driver`` (a prebuilt Wz pythonobs driver) or ``--driver-compiler``
(the C++ seed used once at O0).  The candidate roles never run a C++ helper.
Every compile is traced with the fixed strace and audited by
``corpus.audit_trace``.  The two new driver arguments controlled here are
``--std-binding`` and the ``symbols`` array in the driver's existing JSON
status line; the array is built from the real VerifiedHirModule snapshot and
pythonemit.Output.symbols, never from a guessed ``_f`` number.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import shutil
import sys
import time
import traceback
from pathlib import Path

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
COMPILER_ROOT = HERE.parents[1]
SELFHOST = COMPILER_ROOT / "selfhost"
sys.path.insert(0, str(SELFHOST))
from corpus import TraceError, _run_process_impl, audit_trace  # noqa: E402

EXPECTED = '宿\0主😀e\u0301'.encode('utf-8')
PROBE_NAMES = {
    'probe_host', 'probe_host_enum', 'probe_host_vec',
    'probe_host_release', 'probe_host_value',
}
CASE_OPERATION_ID = 'string-aggregate-binding-20261109'
OPERATION_ID = 'SH14-string-binding-public-api-20261114'
LEGACY_CMAKE_TIMEOUT_SECONDS = 180
RUNTIME_VERSION_LINE = "RUNTIME_VERSION = 'compiler-dev/1-python-values/10'"
PYTHON_VERSION = 'Python 3.12.15'
STD_BINDING_MARKER = 'std/py'


def digest(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2,
                               sort_keys=True) + '\n', encoding='utf-8')


def finite_positive(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError('timeout must be finite and positive') from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError('timeout must be finite and positive')
    return value


def label_name(text: str) -> str:
    return ''.join(
        character if character.isalnum() or character in '-_.' else '_'
        for character in text)


def prepare_output(out: Path, strict: bool) -> None:
    if out.exists():
        if not out.is_dir():
            raise RuntimeError(f'--out is not a directory: {out}')
        if strict and any(out.iterdir()):
            raise RuntimeError(
                f'refusing to overwrite existing evidence in --out: {out}')
    out.mkdir(parents=True, exist_ok=True)


def environment(toolchain_root: Path | None, out: Path) -> dict[str, str]:
    env = dict(os.environ)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PYTHONNOUSERSITE'] = '1'
    env.pop('PYTHONPATH', None)
    if toolchain_root is not None and toolchain_root.is_dir():
        paths = []
        llvm = toolchain_root / 'llvm-22.1.8' / 'lib'
        libxml = toolchain_root / 'libxml2-legacy-2.13.9' / 'usr' / 'lib'
        if llvm.is_dir():
            paths.append(str(llvm))
        if libxml.is_dir():
            paths.append(str(libxml))
        if env.get('LD_LIBRARY_PATH'):
            paths.append(env['LD_LIBRARY_PATH'])
        env['LD_LIBRARY_PATH'] = ':'.join(paths)
    tmp = out / 'tmp'
    tmp.mkdir(parents=True, exist_ok=True)
    env['TMPDIR'] = str(tmp)
    return env


def run_command(argv, cwd: Path, timeout: float, out: Path,
                log: list[dict], label: str, env: dict[str, str],
                strace: Path | None = None,
                audit_allowed: set[str] | None = None) -> dict:
    """Run one real command with the shared bounded process-group helper."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise RuntimeError(f'{label}: timeout must be finite and positive')
    logs = out / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    sequence = len(log)
    stem = f'{sequence:03d}-{label_name(label)}'
    stdout_path = logs / f'{stem}.stdout'
    stderr_path = logs / f'{stem}.stderr'
    tool_argv = [str(part) for part in argv]
    real_argv = list(tool_argv)
    trace_path = None
    if strace is not None:
        traces = out / 'traces'
        traces.mkdir(parents=True, exist_ok=True)
        trace_path = traces / f'{stem}.trace'
        real_argv = [str(strace), '--seccomp-bpf', '-f', '-qq', '-s',
                     '16384', '-e', 'trace=execve,execveat', '-o',
                     str(trace_path), *real_argv]
    started = time.monotonic()
    process = _run_process_impl(real_argv, cwd, env, timeout)
    elapsed = time.monotonic() - started
    stdout_path.write_bytes(process['stdout'])
    stderr_path.write_bytes(process['stderr'])
    record = {
        'label': label,
        'argv': real_argv,
        'tool_argv': tool_argv,
        'cwd': str(cwd),
        'timeout_seconds': timeout,
        'elapsed_seconds': elapsed,
        'exit': process['exit'],
        'timed_out': process['timed_out'],
        'cleanup_complete': process['cleanup_complete'],
        'abandoned_pipes': process['abandoned_pipes'],
        'stdout_path': str(stdout_path),
        'stderr_path': str(stderr_path),
        'stdout_sha256': hashlib.sha256(process['stdout']).hexdigest(),
        'stderr_sha256': hashlib.sha256(process['stderr']).hexdigest(),
        'stdout': process['stdout'].decode('utf-8', errors='replace'),
        'stderr': process['stderr'].decode('utf-8', errors='replace'),
    }
    if trace_path is not None:
        record['trace_path'] = str(trace_path)
        record['trace_sha256'] = digest(trace_path) if trace_path.is_file() else ''
    if audit_allowed is not None:
        if trace_path is None:
            raise RuntimeError(f'{label}: compile was not traced')
        try:
            audit = audit_trace(trace_path, audit_allowed)
        except TraceError as error:
            raise RuntimeError(
                f'{label}: exact exec whitelist rejected trace: {error}') from error
        if set(audit['observed']) != set(audit_allowed):
            raise RuntimeError(
                f'{label}: expected exactly {sorted(audit_allowed)}, '
                f'observed {audit["observed"]}')
        record['trace_audit'] = audit
    log.append(record)
    # Persist immediately, before a later failure can lose the argv/stream hashes.
    with (out / 'commands.jsonl').open('a', encoding='utf-8') as stream:
        stream.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + '\n')
    return record


def require_ok(record: dict, expected_exit: int = 0) -> None:
    if (type(record.get('exit')) is not int
            or record['exit'] != expected_exit
            or record.get('timed_out') is not False
            or record.get('cleanup_complete') is not True
            or record.get('abandoned_pipes') is not False):
        raise RuntimeError(
            f"{record['label']}: exit {record.get('exit')} "
            f"timed_out={record.get('timed_out')} "
            f"cleanup_complete={record.get('cleanup_complete')} "
            f"abandoned_pipes={record.get('abandoned_pipes')}\n"
            f"stdout:\n{record.get('stdout')}\nstderr:\n{record.get('stderr')}")


def parse_json(record: dict) -> dict:
    try:
        document = json.loads(record['stdout'])
    except json.JSONDecodeError as error:
        raise RuntimeError(
            f"{record['label']}: expected JSON, got {record['stdout']!r}: "
            f'{error}') from error
    if not isinstance(document, dict):
        raise RuntimeError(f"{record['label']}: JSON is not an object")
    return document


def require_python_trace(record: dict, python: Path) -> None:
    if 'trace_path' not in record:
        raise RuntimeError(f"{record['label']}: Python command was not traced")
    allowed = {str(python.resolve())}
    try:
        audit = audit_trace(Path(record['trace_path']), allowed)
    except TraceError as error:
        raise RuntimeError(
            f"{record['label']}: not exactly fixed Python 3.12: {error}") from error
    if set(audit['observed']) != allowed:
        raise RuntimeError(
            f"{record['label']}: Python trace observed {audit['observed']} "
            f'instead of {sorted(allowed)}')
    record['trace_audit'] = audit


def canonical_sha(value) -> str:
    return hashlib.sha256(
        json.dumps(value, ensure_ascii=False, sort_keys=True,
                   separators=(',', ':')).encode('utf-8')).hexdigest()


def symbol_probes(symbols) -> dict[str, str]:
    """Use only published names; never synthesize ``_f``/``_s`` numbers."""
    if not isinstance(symbols, list):
        raise RuntimeError('API metadata has no symbols array')
    probes = {}
    for item in symbols:
        if not isinstance(item, dict):
            raise RuntimeError(f'API symbol is not an object: {item!r}')
        symbol = item.get('symbol')
        source_name = item.get('source_name')
        generated = item.get('generated')
        if type(symbol) is not int or symbol < 0:
            raise RuntimeError(f'API symbol id is not an int: {item!r}')
        if not isinstance(source_name, str) or not source_name:
            raise RuntimeError(f'API source_name is not a string: {item!r}')
        if not isinstance(generated, str) or not generated:
            raise RuntimeError(f'API generated name is not a string: {item!r}')
        if source_name.startswith('probe_host'):
            if source_name in probes:
                raise RuntimeError(
                    f'duplicate published source_name: {source_name}')
            probes[source_name] = generated
    if set(probes) != PROBE_NAMES:
        raise RuntimeError(
            f'published probe symbols differ: {sorted(probes)} != '
            f'{sorted(PROBE_NAMES)}')
    if len(set(probes.values())) != len(probes):
        raise RuntimeError(f'duplicate generated probe names: {probes}')
    return probes


def read_api_metadata(kind: str, directory: Path, record: dict) -> tuple[dict, dict[str, str]]:
    if kind == 'helper':
        path = directory / 'metadata.json'
        if not path.is_file():
            raise RuntimeError(f'{record["label"]}: helper wrote no metadata.json')
        try:
            document = json.loads(path.read_text(encoding='utf-8'))
        except json.JSONDecodeError as error:
            raise RuntimeError(f'{record["label"]}: bad metadata.json: {error}') from error
        metadata_path = path
        if parse_json(record).get('status') != 'ok':
            raise RuntimeError(f'{record["label"]}: API status is not ok')
    else:
        document = parse_json(record)
        metadata_path = None
        if document.get('status') != 'ok':
            raise RuntimeError(f'{record["label"]}: API status is not ok')
    symbols = document.get('symbols')
    probes = symbol_probes(symbols)
    if metadata_path is not None:
        metadata_sha = digest(metadata_path)
    else:
        metadata_sha = canonical_sha(document)
    return ({'symbol_count': len(symbols), 'probe_symbols': probes,
             'source': str(metadata_path) if metadata_path else 'stdout',
             'sha256': metadata_sha}, document)


def entry_hashes(directory: Path, label: str) -> dict:
    program = directory / 'program.py'
    runtime = directory / 'langrt.py'
    if not program.is_file() or not runtime.is_file():
        raise RuntimeError(f'{label}: did not publish both program.py and langrt.py')
    runtime_text = runtime.read_text(encoding='utf-8')
    if RUNTIME_VERSION_LINE not in runtime_text:
        raise RuntimeError(f'{label}: generated runtime is not version 10')
    return {
        'program.py': digest(program),
        'langrt.py': digest(runtime),
    }


def compare_entries(first: Path, second: Path, label: str) -> None:
    for name in ('program.py', 'langrt.py'):
        left = digest(first / name)
        right = digest(second / name)
        if left != right:
            raise RuntimeError(
                f'{label}: CLI/API differ on {name} ({left} != {right})')


def check_build(cli: Path, manifest: Path, binding: Path, directory: Path,
                timeout: float, out: Path, log: list[dict], label: str,
                env: dict[str, str], strace: Path | None,
                audit_allowed: set[str] | None) -> dict:
    check_record = run_command(
        [cli, 'check', '--project', manifest, '--backend', 'python',
         '--std-binding', binding, '--format', 'json'],
        out, timeout, out, log, f'{label}-check', env, strace, audit_allowed)
    require_ok(check_record)
    if check_record['stderr']:
        raise RuntimeError(f"{check_record['label']}: unexpected stderr")
    check_document = parse_json(check_record)
    if check_document.get('status') != 'ok' or check_document.get('diagnostics'):
        raise RuntimeError(f"{check_record['label']}: check failed: {check_document}")

    directory.mkdir(parents=True, exist_ok=True)
    build_record = run_command(
        [cli, 'build', '--project', manifest, '--backend', 'python',
         '--std-binding', binding, '--out-dir', directory, '--format', 'json'],
        out, timeout, out, log, f'{label}-build', env, strace, audit_allowed)
    require_ok(build_record)
    if build_record['stderr']:
        raise RuntimeError(f"{build_record['label']}: unexpected stderr")
    build_document = parse_json(build_record)
    if build_document.get('status') != 'ok' or build_document.get('diagnostics'):
        raise RuntimeError(f"{build_record['label']}: build failed: {build_document}")
    return {'check': check_record, 'build': build_record,
            'entries': entry_hashes(directory, f'{label}-build')}


def observe(name: str, python: Path, directory: Path, probes: dict[str, str],
            timeout: float, out: Path, log: list[dict], label: str,
            env: dict[str, str], strace: Path | None) -> list[dict]:
    code = ('import json,program\n'
            'out={}\n'
            f'for name,generated in {probes!r}.items():\n'
            ' value=getattr(program,generated)()\n'
            ' assert type(value) is str\n'
            ' data=value.encode("utf-8")\n'
            ' out[name]={"hex":data.hex(),"byte_length":len(data),"text":value}\n'
            'print(json.dumps(out,ensure_ascii=True))\n')
    record = run_command(
        [python, '-B', '-s', '-c', code], directory, timeout, out, log,
        f'{label}-observe-{name}', env, strace)
    require_ok(record)
    if record['stderr']:
        raise RuntimeError(f"{record['label']}: unexpected stderr")
    if strace is not None:
        require_python_trace(record, python)
    observed = parse_json(record)
    if set(observed) != set(probes):
        raise RuntimeError(
            f"{record['label']}: observed keys {sorted(observed)} != "
            f'{sorted(probes)}')
    rows = []
    for key in sorted(probes):
        value = observed[key]
        if not isinstance(value, dict):
            raise RuntimeError(f"{record['label']}: bad value row {value!r}")
        result = dict(id=key, emit=name,
                      actual_hex=value.get('hex'),
                      actual_byte_length=value.get('byte_length'),
                      expected_hex=EXPECTED.hex(),
                      expected_byte_length=len(EXPECTED), status='failed')
        rows.append(result)
        if value.get('hex') != EXPECTED.hex():
            raise RuntimeError(f"{record['label']}: {key} UTF-8 differs: {result}")
        if value.get('byte_length') != len(EXPECTED):
            raise RuntimeError(f"{record['label']}: {key} length differs: {result}")
        if value.get('text') != EXPECTED.decode('utf-8'):
            raise RuntimeError(f"{record['label']}: {key} text differs: {result}")
        result['status'] = 'passed'
    return rows


def run_program(python: Path, directory: Path, timeout: float, out: Path,
                log: list[dict], label: str, env: dict[str, str],
                strace: Path | None) -> dict:
    record = run_command(
        [python, '-B', '-s', directory / 'program.py'], directory,
        timeout, out, log, f'{label}-program', env, strace)
    require_ok(record)
    if record['stdout'] or record['stderr']:
        raise RuntimeError(f"{record['label']}: program produced output")
    if strace is not None:
        require_python_trace(record, python)
    return record


def run_group(name: str, args, out: Path, driver: Path | None,
              log: list[dict], env: dict[str, str],
              strace: Path | None) -> dict:
    project = args.root / 'tests/python_bindings/string-aggregate'
    manifest = project / 'lang.toml'
    binding = project / 'binding.json'
    host = args.root / 'tests/python_bindings/string_aggregate_host.py'
    reference = name == 'reference'
    cli = args.cli if reference else args.candidate_cli
    api_kind = 'helper' if reference else 'driver'
    api_tool = args.helper if reference else driver
    group_dir = out / name
    result = {
        'name': name, 'status': 'failed', 'complete': False,
        'api_kind': api_kind, 'cli': {}, 'api': {},
        'generated_identical': False, 'values': [], 'observations': {},
        'error': None,
    }
    try:
        if cli is None or api_tool is None:
            raise RuntimeError(f'{name}: missing CLI or API entry')
        # Candidate compiles must exec only the fixed candidate entry.  The
        # reference entries are traced too, but are not presented as this
        # candidate's exact whitelist evidence.
        cli_allowed = {str(cli.resolve())} if not reference else None
        api_allowed = {str(api_tool.resolve())} if not reference else None
        cli_dir = group_dir / 'cli'
        api_dir = group_dir / 'api'
        cli_build = check_build(
            cli, manifest, binding, cli_dir, args.timeout, out, log,
            f'{name}-cli', env, strace, cli_allowed)
        result['cli'] = cli_build
        cli_initial = dict(cli_build['entries'])
        result['cli']['generated_initial'] = cli_initial

        api_dir.mkdir(parents=True, exist_ok=True)
        if reference:
            api_record = run_command(
                [api_tool, '--project', manifest, '--out-dir', api_dir,
                 '--std-binding', binding],
                out, args.timeout, out, log, f'{name}-helper', env, strace)
        else:
            api_record = run_command(
                [api_tool, '--project', manifest, '--out-dir', api_dir,
                 '--std-binding', binding, '--format', 'json'],
                out, args.timeout, out, log, f'{name}-api', env, strace,
                api_allowed)
        require_ok(api_record)
        if api_record['stderr']:
            raise RuntimeError(f"{api_record['label']}: unexpected stderr")
        api_entries = entry_hashes(api_dir, f'{name}-api')
        metadata, metadata_document = read_api_metadata(
            api_kind, api_dir, api_record)
        write_json(group_dir / 'api-metadata.json', metadata_document)
        result['api'] = {
            'emit': api_record, 'metadata': metadata,
            'entries': api_entries, 'generated_initial': dict(api_entries),
        }

        # Compare both generated files before the test host is copied into
        # either output directory.  Then both emits become runnable.
        compare_entries(cli_dir, api_dir, name)
        result['generated_identical'] = True
        shutil.copyfile(host, cli_dir / 'strhost.py')
        shutil.copyfile(host, api_dir / 'strhost.py')

        result['cli']['program'] = run_program(
            args.python, cli_dir, args.timeout, out, log,
            f'{name}-cli', env, strace)
        probes = metadata['probe_symbols']
        cli_rows = observe('cli', args.python, cli_dir, probes, args.timeout,
                           out, log, f'{name}-cli', env, strace)
        api_rows = observe('api', args.python, api_dir, probes, args.timeout,
                           out, log, f'{name}-api', env, strace)
        result['observations'] = {'cli': cli_rows, 'api': api_rows}
        result['values'] = api_rows

        cli_final = entry_hashes(cli_dir, f'{name}-cli-final')
        api_final = entry_hashes(api_dir, f'{name}-api-final')
        if cli_final != cli_initial or api_final != api_entries:
            raise RuntimeError(f'{name}: generated files changed after observation')
        result['cli']['generated_final'] = cli_final
        result['api']['generated_final'] = api_final
        result['sources'] = {
            str(path.relative_to(project)): digest(path)
            for path in sorted(project.rglob('*')) if path.is_file()
        }
        result['host_sha256'] = digest(host)
        result['status'] = 'passed'
        result['complete'] = True
    except Exception as error:
        result['status'] = 'failed'
        result['error'] = str(error)
    finally:
        write_json(group_dir / 'result.json', result)
    return result


def stage_driver_project(source_root: Path, driver_source: Path,
                         resources: Path, work: Path) -> Path:
    project = work / 'driver' / 'project'
    (project / 'src').mkdir(parents=True, exist_ok=True)
    wzcc = project / 'src' / 'wzcc'
    std = project / 'src' / 'std'
    for link, target in ((wzcc, source_root / 'wzcc'),
                         (std, resources / 'stdlib')):
        if link.exists() or link.is_symlink():
            link.unlink()
        try:
            link.symlink_to(target, target_is_directory=True)
        except OSError:
            shutil.copytree(target, link)
    shutil.copy2(driver_source / 'src' / 'main.wz', project / 'src' / 'main.wz')
    shutil.copy2(driver_source / 'lang.toml', project / 'lang.toml')
    return project


def check_resources(resources: Path, label: str) -> None:
    for relative in ('stdlib', 'runtime/python/unicode18.py',
                     'runtime/python/langrt.py'):
        if not (resources / relative).exists():
            raise RuntimeError(f'{label}: missing resource {resources / relative}')


def prepare_driver(args, out: Path, resources: Path, log: list[dict],
                   env: dict[str, str]) -> tuple[Path, dict]:
    driver_root = out / 'driver' / 'root'
    driver_root.mkdir(parents=True, exist_ok=True)
    info = {'resources': str(resources)}
    if args.api_driver is not None:
        source_driver = args.api_driver
        info['built'] = False
    else:
        if args.driver_compiler is None:
            raise RuntimeError('candidate needs --api-driver or --driver-compiler')
        driver_source = args.driver_source or (args.root / 'selfhost/tests/pythonobs')
        project = stage_driver_project(
            args.source_root, driver_source, resources, out)
        build_dir = out / 'driver' / 'build'
        build_dir.mkdir(parents=True, exist_ok=True)
        record = run_command(
            [args.driver_compiler, 'build', '--project',
             project / 'lang.toml', '--backend', 'native', '--opt', '0',
             '--out-dir', build_dir, '--format', 'json'],
            project, args.build_timeout, out, log, 'driver-build', env,
            args.strace)
        require_ok(record)
        document = parse_json(record)
        if document.get('status') != 'ok' or document.get('diagnostics'):
            raise RuntimeError(f'driver build failed: {document}')
        source_driver = build_dir / 'program'
        info.update({
            'built': True, 'compiler': str(args.driver_compiler),
            'compiler_sha256': digest(args.driver_compiler),
            'source': str(driver_source), 'project': str(project),
            'build_command': record['label'],
        })
    if not source_driver.is_file():
        raise RuntimeError(f'API driver missing: {source_driver}')
    target = driver_root / 'pythonobs'
    shutil.copy2(source_driver, target)
    target.chmod(0o755)
    for name in ('stdlib', 'runtime'):
        link = driver_root / name
        if link.exists() or link.is_symlink():
            link.unlink()
        try:
            link.symlink_to(resources / name, target_is_directory=True)
        except OSError:
            shutil.copytree(resources / name, link)
    info.update({'path': str(target), 'sha256': digest(target),
                 'initial_sha256': digest(target)})
    return target, info


def snapshot_inputs(args, resources: Path | None,
                    driver_path: Path | None) -> dict[str, str]:
    paths = set()
    out = args.out.resolve()

    def add(path: Path | None) -> None:
        if path is None:
            return
        path = Path(path)
        if not path.is_file():
            return
        try:
            path.resolve().relative_to(out)
            return
        except ValueError:
            pass
        paths.add(path)

    project = args.root / 'tests/python_bindings/string-aggregate'
    for path in project.rglob('*'):
        add(path)
    add(args.root / 'tests/python_bindings/string_aggregate_host.py')
    add(Path(__file__).resolve())
    add(HERE / 'string_binding_controls.py')
    add(args.root / 'tests/python/CMakeLists.txt')
    add(args.root / 'CMakeLists.txt')
    for path in (args.root / 'selfhost/tests/pythonobs').rglob('*'):
        add(path)
    if args.driver_source is not None:
        for path in args.driver_source.rglob('*'):
            add(path)
    for path in args.source_root.rglob('*.wz'):
        add(path)
    for pattern in ('stdlib/**/*.wz', 'runtime/python/*.py',
                    'selfhost/corpus.json', 'selfhost/corpus.py'):
        for path in args.root.glob(pattern):
            add(path)
    if resources is not None:
        for path in (resources / 'stdlib').rglob('*.wz'):
            add(path)
        for path in (resources / 'runtime').rglob('*.py'):
            add(path)
    for tool in (args.cli, args.helper, args.candidate_cli, args.api_driver,
                 driver_path, args.driver_compiler, args.python, args.strace):
        add(tool)
    if args.toolchain_root is not None:
        for relative in (
                'llvm-22.1.8/bin/opt', 'llvm-22.1.8/bin/llc',
                'llvm-22.1.8/bin/clang', 'llvm-22.1.8/bin/clang++',
                'llvm-22.1.8/bin/ld.lld',
                'libxml2-legacy-2.13.9/usr/lib/libxml2.so.2.13.9'):
            add(args.toolchain_root / relative)
    return {str(path): digest(path) for path in sorted(paths)}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, help='reference C++ woozyc CLI')
    parser.add_argument('--helper', type=Path,
                        help='reference C++ lang_python_emit helper')
    parser.add_argument('--candidate-cli', type=Path,
                        help='candidate pure-Wz CLI for --backend python')
    parser.add_argument('--api-driver', type=Path,
                        help='prebuilt Wz pythonobs driver')
    parser.add_argument('--driver-compiler', type=Path,
                        help='C++ seed used once to build the Wz driver at O0')
    parser.add_argument('--driver-source', type=Path,
                        help='Wz pythonobs driver project directory')
    parser.add_argument('--source-root', type=Path,
                        help='frozen Wz source package root (contains wzcc/)')
    parser.add_argument('--candidate-resources', type=Path,
                        help='candidate stdlib+runtime directory')
    parser.add_argument('--strace', type=Path,
                        help='strace 7.2 used for every real command')
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--python', type=Path, required=True)
    parser.add_argument('--timeout', type=finite_positive, default=90.0)
    parser.add_argument('--build-timeout', type=finite_positive, default=1800.0)
    parser.add_argument('--toolchain-root', type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    for key in ('cli', 'helper', 'candidate_cli', 'api_driver',
                'driver_compiler', 'driver_source', 'source_root',
                'candidate_resources', 'strace', 'root', 'out', 'python',
                'toolchain_root'):
        value = getattr(args, key)
        if value is not None:
            if not value.is_absolute():
                found = shutil.which(str(value))
                value = Path(found) if found else value
            setattr(args, key, value.resolve())
    if args.toolchain_root is None:
        args.toolchain_root = args.root.parent / 'toolchains'
    if args.source_root is None:
        args.source_root = args.root / 'selfhost/src'

    reference_requested = args.cli is not None
    candidate_requested = (args.candidate_cli is not None
                           or args.api_driver is not None
                           or args.driver_compiler is not None)
    if reference_requested and args.helper is None:
        raise RuntimeError('reference mode requires --helper')
    if candidate_requested:
        if args.candidate_cli is None:
            raise RuntimeError('candidate mode requires --candidate-cli')
        if args.api_driver is None and args.driver_compiler is None:
            raise RuntimeError(
                'candidate API requires --api-driver or --driver-compiler')
        if args.api_driver is not None and args.driver_compiler is not None:
            raise RuntimeError('use either --api-driver or --driver-compiler')
        if args.strace is None:
            raise RuntimeError('candidate mode requires --strace')
    if not reference_requested and not candidate_requested:
        raise RuntimeError('pass --cli/--helper and/or candidate arguments')
    # The evidence run must be both roles.  The original CMake command keeps
    # its legacy reference-only behaviour and its fixed output directory.
    if candidate_requested and not reference_requested:
        raise RuntimeError('this case requires both reference and candidate roles')

    resources = None
    if candidate_requested:
        resources = args.candidate_resources or args.candidate_cli.parent
        check_resources(resources, 'candidate')
    prepare_output(args.out, strict=candidate_requested)
    env = environment(args.toolchain_root, args.out)
    log: list[dict] = []
    summary = {
        'operation_id': OPERATION_ID,
        'case_operation_id': CASE_OPERATION_ID,
        'status': 'failed',
        'python': None,
        'legacy_cmake_timeout_seconds': LEGACY_CMAKE_TIMEOUT_SECONDS,
        'legacy_cmake_timeout_exceeded': False,
        'elapsed_seconds': 0.0,
        'groups': {}, 'values': [], 'observations': {},
        'generated_identical': False,
        'commands': log, 'driver': {},
        'inputs_before': {}, 'inputs_after': {},
        'sources': {}, 'host_sha256': None,
        'not_run': [], 'error': None,
    }
    started = time.monotonic()
    driver = None
    try:
        summary['inputs_before'] = snapshot_inputs(args, resources, None)
        version = run_command(
            [args.python, '--version'], args.out, args.timeout, args.out,
            log, 'python-version', env, args.strace)
        require_ok(version)
        if version['stdout'].strip() != PYTHON_VERSION:
            raise RuntimeError(
                f'{PYTHON_VERSION} required: {version["stdout"].strip()}')
        if args.strace is not None:
            require_python_trace(version, args.python)
        summary['python'] = version

        if candidate_requested:
            driver, driver_info = prepare_driver(
                args, args.out, resources, log, env)
            summary['driver'] = driver_info
        if reference_requested:
            summary['groups']['reference'] = run_group(
                'reference', args, args.out, None, log, env, args.strace)
        if candidate_requested:
            summary['groups']['candidate'] = run_group(
                'candidate', args, args.out, driver, log, env, args.strace)

        primary = 'candidate' if candidate_requested else 'reference'
        group = summary['groups'][primary]
        summary.update({
            'values': group.get('values', []),
            'observations': group.get('observations', {}),
            'generated_identical': group.get('generated_identical', False),
            'sources': group.get('sources', {}),
            'host_sha256': group.get('host_sha256'),
        })
        statuses = [item['status'] for item in summary['groups'].values()]
        completes = [item['complete'] for item in summary['groups'].values()]
        if any(status != 'passed' for status in statuses):
            summary['status'] = 'failed'
            summary['error'] = '; '.join(
                f"{item['name']}: {item['error']}"
                for item in summary['groups'].values() if item['error'])
        elif not all(completes):
            summary['status'] = 'partial'
        else:
            summary['status'] = 'passed'
        summary['generated_identical_all'] = all(
            item.get('generated_identical') for item in summary['groups'].values())
        if not summary['generated_identical_all']:
            summary['status'] = 'failed'
            summary['error'] = 'at least one group lacked CLI/API byte equality'
    except Exception as error:
        summary['error'] = str(error)
        summary['traceback'] = traceback.format_exc()
        summary['status'] = 'failed'
    finally:
        summary['elapsed_seconds'] = time.monotonic() - started
        summary['legacy_cmake_timeout_exceeded'] = (
            summary['elapsed_seconds'] > LEGACY_CMAKE_TIMEOUT_SECONDS)
        try:
            # The built driver lives under --out and is hashed separately.
            summary['inputs_after'] = snapshot_inputs(
                args, resources, None)
        except Exception as error:
            summary['inputs_after'] = {'error': str(error)}
            summary['status'] = 'failed'
            if summary['error'] is None:
                summary['error'] = f'input snapshot failed: {error}'
        if summary['inputs_after'] != summary['inputs_before']:
            summary['status'] = 'failed'
            message = 'source/resource inputs changed during the run'
            if summary['error'] is None:
                summary['error'] = message
            else:
                summary['error'] += '; ' + message
        if summary['driver'].get('sha256') is not None:
            summary['driver']['final_sha256'] = digest(
                Path(summary['driver']['path']))
            if summary['driver']['final_sha256'] != summary['driver']['sha256']:
                summary['status'] = 'failed'
                summary['error'] = (summary['error'] or '') + '; driver changed'
        path = args.out / 'binding-results.json'
        write_json(path, summary)
        write_json(args.out / 'summary.json', summary)
        print(json.dumps({
            'status': summary['status'],
            'values': len(summary['values']),
            'groups': {name: item['status']
                       for name, item in summary['groups'].items()},
            'elapsed_seconds': summary['elapsed_seconds'],
            'legacy_cmake_timeout_exceeded':
                summary['legacy_cmake_timeout_exceeded'],
            'report': str(path),
        }))
        if summary['error']:
            print(f"error: {summary['error']}", file=sys.stderr)
    return 0 if summary['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
