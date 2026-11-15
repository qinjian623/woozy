#!/usr/bin/env python3
"""Generate, execute and assert T10 Python binding/runtime cases.

The helper uses the real public load/resolve/check/verify/emit API.  Generated
programs are executed only by the explicitly supplied CPython path; this
runner does not import NumPy/PyTorch itself except through the independent
original-04, real-object and reference subprocesses.
"""
import argparse
import ast
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
THREAD_ENV = ('OMP_NUM_THREADS', 'MKL_NUM_THREADS', 'OPENBLAS_NUM_THREADS',
              'NUMEXPR_NUM_THREADS', 'VECLIB_MAXIMUM_THREADS')

# Reuse the public-CLI harness process/trace helpers so candidate compiles and
# observations keep raw streams and fail closed on an unexpected exec.
SELFHOST = Path(__file__).resolve().parents[2] / 'selfhost'
sys.path.insert(0, str(SELFHOST))
from corpus import (TraceError, _output_entry, _run_process_impl,
                    _scan_output_tree, audit_trace)  # noqa: E402

# Fixed registration order of the complete python.t10_bindings target.  The
# three-case python.foreign_ownership target is only a subset of these; it
# never stands in for the complete 20-case run.
NAMED_CASES = [
    'order-objects-errors',
    'foreign-ownership-move-aggregate',
    'py-error-aggregate-storage',
    'foreign-ownership-cleanup',
    'generic-foreign-error-key',
    'generic-error-key-escaping',
    'foreign-ownership-rejections',
    'internal-cross-module',
    'internal-escape',
    'base-keyboard-interrupt',
    'base-system-exit',
    'base-panic',
    'bad-unit-contract',
    'no-throws-external-callable',
    'foreign-value-error',
    'function-foreign-error',
    'missing-host-environment-error',
    'real-ndarray-tensor-identity',
    'real-host-exception',
    'original-04-cpu',
]
FOREIGN_OWNERSHIP_ONLY = [
    'foreign-ownership-move-aggregate',
    'foreign-ownership-cleanup',
    'foreign-ownership-rejections',
]

# All eleven projects inside 'foreign-ownership-rejections'.  The first eight
# are binding/ownership diagnostics; the remaining two are direct runtime-error
# declarations and the last is the 32-bit word-size rejection.
FOREIGN_REJECTION_CASES = [
    ('inout', 'func f(x inout host.Probe) Unit { } '
     'func main() i32 { return 0; }', 'E_CAPABILITY_UNAVAILABLE'),
    ('partial-move', 'struct S { p host.Probe; } '
     'func main() i32 throws py.Error { let s = S(p: host.make()); '
     'let p = move s.p; return 0; }', 'E_CAPABILITY_UNAVAILABLE'),
    ('undeclared-member', 'func main() i32 throws py.Error { '
     'let p = host.make(); p.missing(); return 0; }',
     'E_CAPABILITY_UNAVAILABLE'),
    ('read-after-move', 'func main() i32 throws py.Error { '
     'let p = host.make(); let q = move p; host.same(p, q); '
     'return 0; }', 'E_OWNERSHIP_USE_AFTER_MOVE'),
    ('double-move', 'func main() i32 throws py.Error { '
     'let p = host.make(); let q = move p; let r = move p; '
     'return 0; }', 'E_OWNERSHIP_USE_AFTER_MOVE'),
    ('borrow-alias', 'func f(a host.Probe, b move host.Probe) i32 '
     '{ return 0; } func main() i32 throws py.Error { '
     'let p = host.make(); return f(p, move p); }', 'E_CALL_ARGUMENT'),
    ('error-aggregate-read-after-move', 'struct Saved { value py.Error; } '
     'func main() i32 throws py.Error { try { host.error_source(); } '
     'catch e py.Error { let saved = Saved(value: e); '
     'let moved = move saved; host.inspect(saved.value); } '
     'return 0; }', 'E_OWNERSHIP_USE_AFTER_MOVE'),
    ('move-borrowed-parameter', 'func f(p host.Probe) i32 { '
     'let q = move p; return 0; } func main() i32 throws py.Error { '
     'return f(host.make()); }', 'E_CALL_ARGUMENT'),
]
DIRECT_REJECTION_CASES = [
    ('direct-throws', 'func fail() Unit throws host.Probe { } '
     'func main() i32 { return 0; }'),
    ('direct-catch', 'func fail() Unit throws host.Probe { } '
     'func main() i32 { try { fail(); } '
     'catch e host.Probe { return 0; } return 1; }'),
]
WORD_BITS_REJECTION = 'foreign-reject-32bit'


def rejection_plan():
    """Return the eleven (project, code) rejection projects in order."""
    plan = [(name, code) for name, _, code in FOREIGN_REJECTION_CASES]
    plan.extend((name, 'E_CAPABILITY_UNAVAILABLE')
                for name, _ in DIRECT_REJECTION_CASES)
    plan.append((WORD_BITS_REJECTION, 'E_CAPABILITY_UNAVAILABLE'))
    return plan


def label_name(text):
    return ''.join(
        character if character.isalnum() or character in '-_.' else '_'
        for character in str(text))


def finite_positive(text):
    """Accept only the original per-command budget: finite, positive, <= 180."""
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            'timeout must be finite, positive and at most 180 seconds') from error
    if not math.isfinite(value) or value <= 0 or value > 180:
        raise argparse.ArgumentTypeError(
            'timeout must be finite, positive and at most 180 seconds')
    return value


def dispatch_mode(args):
    """Select legacy or paired candidate mode without starting any process."""
    candidate = getattr(args, 'candidate_cli', None) is not None
    api = getattr(args, 'api_driver', None) is not None
    if api and not candidate:
        raise AssertionError('--api-driver requires --candidate-cli')
    if candidate and not api:
        raise AssertionError(
            'candidate mode requires --api-driver for real PythonEmitter '
            'symbols/entry/HIR')
    if candidate and getattr(args, 'strace', None) is None:
        raise AssertionError(
            'candidate mode requires --strace for compile/exec evidence')
    if candidate and getattr(args, 'helper', None) is None:
        raise AssertionError(
            'paired mode requires --helper as the reference emitter')
    return 'paired' if candidate else 'legacy'


def prepare_output(out, strict):
    out = Path(out)
    if out.exists():
        if not out.is_dir():
            raise AssertionError(f'--out is not a directory: {out}')
        if strict and any(out.iterdir()):
            raise AssertionError(
                f'refusing to overwrite existing evidence in --out: {out}')
    out.mkdir(parents=True, exist_ok=True)


def exec_allowlist(*paths):
    return {str(Path(path).resolve()) for path in paths}


def strict_process(record):
    """Reject truthy stand-ins: exit must be int, flags must be real bools."""
    if type(record.get('exit')) is not int:
        raise AssertionError(
            f'process exit is not a real int: {record.get("exit")!r}')
    for key in ('timed_out', 'cleanup_complete', 'abandoned_pipes'):
        if type(record.get(key)) is not bool:
            raise AssertionError(
                f'process {key} is not a real bool: {record.get(key)!r}')
    return record


def require_clean(record, label='process'):
    """Reject a finished command with a timeout, dirty cleanup or pipes."""
    strict_process(record)
    if (record['timed_out'] or not record['cleanup_complete']
            or record['abandoned_pipes']):
        raise AssertionError(
            f"{label}: process was not clean: exit {record['exit']} "
            f"timed_out={record['timed_out']} "
            f"cleanup_complete={record['cleanup_complete']} "
            f"abandoned_pipes={record['abandoned_pipes']}")
    return record


def require_process(record, expected_exit=0):
    require_clean(record)
    if record['exit'] != expected_exit:
        raise AssertionError(
            f"process exit {record['exit']} != expected {expected_exit}")
    return record


# CPython's platform module probes `uname -p` through PATH while a real
# NumPy/PyTorch observer collects its environment.  Only the fixed Linux uname
# tool may be that optional child, only on the observer/exec whitelist, and its
# identity is read and frozen before the paired Runner starts any real command.
OBSERVER_UNAME_PATH = '/usr/bin/uname'
UNAME_PROBE_ARGV = ['uname', '-p']
_ERRNO = re.compile(r'^(?P<errno>[A-Z][A-Z0-9]*)\b')
_EXECVE = re.compile(r'\bexecve\(("(?:[^"\\]|\\.)*")')
_EXECVEAT = re.compile(r'\bexecveat\(')
_EXEC_LINE = re.compile(
    r'^\s*(?:\[pid\s+)?(?P<pid>\d+)\]?\s+execve\('
    r'(?P<path>"(?:[^"\\]|\\.)*")\s*,\s*'
    r'(?P<rest>.*)\)\s+=\s+(?P<result>-?\d+)(?P<detail>.*)$')


def trace_string(text):
    """Decode one strace string literal the way corpus.parse_trace does."""
    return ast.literal_eval('b' + text).decode(
        sys.getfilesystemencoding(), 'surrogateescape')


def trace_argv(text):
    match = re.match(r'\s*(\[[^\]]*\])', text)
    if match is None:
        raise AssertionError(f'uname probe argv is missing: {text!r}')
    try:
        argv = ast.literal_eval(match.group(1))
    except (SyntaxError, ValueError) as error:
        raise AssertionError(
            f'uname probe argv is not a literal list: {text!r}') from error
    if not isinstance(argv, list) or not all(
            isinstance(item, str) for item in argv):
        raise AssertionError(
            f'uname probe argv is not a string list: {argv!r}')
    return argv


def read_uname_identity(path, path_entries):
    """Read a fixed uname tool's logical/canonical path and content SHA."""
    logical = Path(path)
    if not os.path.isabs(str(logical)):
        raise AssertionError(f'frozen uname path is not absolute: {path!r}')
    canonical = logical.resolve()
    if not canonical.is_file():
        raise AssertionError(f'frozen uname tool is missing: {logical}')
    entries = [str(entry) for entry in path_entries]
    if not entries:
        raise AssertionError('observer PATH is empty; uname identity needs it')
    for entry in entries:
        if not os.path.isabs(entry):
            raise AssertionError(
                f'observer PATH entry is not absolute: {entry!r}')
    return {'path': str(logical), 'canonical': str(canonical),
            'sha256': sha(canonical), 'path_entries': entries}


def freeze_observer_uname(path=OBSERVER_UNAME_PATH, search_path=None):
    """Freeze the fixed uname tool and the observer PATH before any command."""
    if not isinstance(search_path, str) or not search_path:
        raise AssertionError('observer PATH is required to freeze uname')
    entries = [entry for entry in search_path.split(os.pathsep) if entry]
    return read_uname_identity(path, entries)


def verify_uname_identity(frozen):
    """Re-read the frozen tool now and refuse any identity change."""
    if not isinstance(frozen, dict):
        raise AssertionError('frozen uname identity is missing')
    current = read_uname_identity(
        frozen.get('path'), frozen.get('path_entries') or [])
    if (current['canonical'] != frozen.get('canonical')
            or current['sha256'] != frozen.get('sha256')):
        raise AssertionError(
            'frozen uname identity changed: '
            f"{frozen.get('path')!r} was {frozen.get('sha256')} at "
            f"{frozen.get('canonical')}, now {current['sha256']} at "
            f"{current['canonical']}")
    return {'path': frozen['path'], 'canonical': frozen['canonical'],
            'sha256_before': frozen['sha256'],
            'sha256_at_audit': current['sha256'],
            'path_entries': list(frozen['path_entries']),
            'unchanged': True}


def trace_uname_probes(trace_path, required, frozen):
    """Collect verified optional `uname -p` probes from one retained trace.

    The observer policy accepts only the frozen Linux uname tool, only as a
    child of a successfully executed required tool, and only with argv
    ``["uname", "-p"]``.  Failed lookups must be the next explicit entries of
    the frozen observer PATH, use a real ENOENT errno token, and be followed by
    the frozen tool from the same child pid.  ``execveat`` is rejected because
    this policy cannot verify that form.  Every required path needs at least
    one complete successful execve record with a real pid.  Returns the probe
    records and the pids that successfully ran ``required``.
    """
    lookup = [str((Path(entry) / 'uname').resolve())
              for entry in frozen['path_entries']]
    canonical = frozen['canonical']
    probes = []
    required_pids = set()
    required_success = 0
    pending = {}
    failed = set()
    executed = set()
    for line in Path(trace_path).read_text(
            encoding='utf-8', errors='surrogateescape').splitlines():
        if _EXECVEAT.search(line):
            raise AssertionError(
                'observer exec policy does not accept execveat: ' + line)
        match = _EXECVE.search(line)
        if match is None:
            continue
        raw = trace_string(match.group(1))
        executable = str(Path(raw).resolve())
        fields = _EXEC_LINE.match(line)
        if executable in required:
            if fields is None:
                raise AssertionError(
                    'unparseable required execve record: ' + line)
            result = int(fields.group('result'))
            detail = fields.group('detail').strip()
            if result != 0 or detail:
                raise AssertionError(
                    'required observer tool did not successfully execve: '
                    + line)
            required_pids.add(int(fields.group('pid')))
            required_success += 1
            continue
        if os.path.basename(executable) != 'uname':
            continue
        if fields is None:
            raise AssertionError(f'unparseable uname execve record: {line}')
        if not os.path.isabs(raw):
            raise AssertionError(
                f'uname probe pathname is relative: {raw!r}')
        argv = trace_argv(fields.group('rest'))
        if argv != UNAME_PROBE_ARGV:
            raise AssertionError(
                f'uname probe argv {argv!r} != {UNAME_PROBE_ARGV!r}')
        pid = int(fields.group('pid'))
        result = int(fields.group('result'))
        detail = fields.group('detail').strip()
        if result == 0:
            if detail:
                raise AssertionError(
                    f'uname probe {executable}: unexpected success detail '
                    f'{detail!r}')
            if executable != canonical:
                raise AssertionError(
                    f'uname probe {executable} is not the frozen tool '
                    f'{canonical}')
            if executable in failed:
                raise AssertionError(
                    f'uname probe {executable}: reported failed lookup and '
                    'success')
            failures = pending.pop(pid, [])
            expected = lookup[:len(failures) + 1]
            actual = [item['path'] for item in failures] + [executable]
            if actual != expected:
                raise AssertionError(
                    'uname -p lookup sequence is not the frozen PATH prefix: '
                    f'{actual} != {expected}')
            probes.append({'pid': pid, 'argv': list(UNAME_PROBE_ARGV),
                           'lookup': failures, 'executable': executable,
                           'result': result})
            executed.add(executable)
        elif result == -1:
            errno = _ERRNO.match(detail)
            if errno is None or errno.group('errno') != 'ENOENT':
                raise AssertionError(
                    f'uname probe {executable}: lookup failure errno is not '
                    f'ENOENT: {detail!r}')
            if executable in executed:
                raise AssertionError(
                    f'uname probe {executable}: lookup failed after success')
            failures = pending.setdefault(pid, [])
            index = len(failures)
            if index >= len(lookup) or lookup[index] != executable:
                raise AssertionError(
                    f'uname probe lookup {executable} is not frozen PATH '
                    f'entry {lookup[index] if index < len(lookup) else None}')
            failures.append({'path': executable, 'result': 'ENOENT'})
            failed.add(executable)
        else:
            raise AssertionError(
                f'uname probe {executable}: unexpected execve result {result}')
    if pending:
        raise AssertionError(
            'uname -p lookup failed without an executed child: '
            + ', '.join(sorted(failed - executed)))
    if required_success == 0:
        raise AssertionError(
            'required observer tool has no successful execve record')
    return probes, required_pids


def require_trace(record, allowed, label='trace', allow_uname=None):
    path = record.get('trace_path')
    if not isinstance(path, str) or not path:
        raise AssertionError(f'{label}: command was not traced')
    allowed = {str(Path(value).resolve()) for value in allowed}
    probes = []
    identity = None
    if allow_uname is not None and allow_uname is not False:
        if not isinstance(allow_uname, dict):
            raise AssertionError(
                f'{label}: observer uname policy needs the frozen identity')
        try:
            identity = verify_uname_identity(allow_uname)
            probes, required_pids = trace_uname_probes(
                Path(path), allowed, allow_uname)
        except AssertionError as error:
            raise AssertionError(
                f'{label}: optional uname -p policy rejected trace: '
                f'{error}') from error
        for probe in probes:
            if probe['pid'] in required_pids:
                raise AssertionError(
                    f'{label}: uname -p probe reused the traced tool pid '
                    f'{probe["pid"]}')
            allowed |= {item['path'] for item in probe['lookup']}
            allowed.add(probe['executable'])
    try:
        audit = audit_trace(Path(path), allowed)
    except TraceError as error:
        raise AssertionError(
            f'{label}: exact exec whitelist rejected trace: {error}') from error
    if set(audit['observed']) != allowed:
        raise AssertionError(
            f'{label}: expected exactly {sorted(allowed)}, '
            f'observed {audit["observed"]}')
    if identity is not None:
        identity['probe_count'] = len(probes)
        audit['uname_identity'] = identity
    if probes:
        audit['optional_uname'] = probes
    record['trace_audit'] = audit
    return audit


def require_hir(document, label='api'):
    """Accept only the actual Verified-path hir dump text, never a fake."""
    text = document.get('verified_hir')
    if not isinstance(text, str) or not text.strip():
        raise AssertionError(
            f'{label}: real Verified HIR text is missing, non-string or empty')
    if ' scalar-v1 module=' not in text:
        raise AssertionError(f'{label}: HIR text is not a real hir dump')
    return text


def api_projection(document):
    """Project real PythonEmitter entry/symbols; never synthesize _f/_s."""
    if document.get('status') != 'ok':
        raise AssertionError(
            f'API status is not ok: {document.get("status")!r}')
    entry = document.get('entry')
    symbols = document.get('symbols')
    if type(entry) is not int or entry < 0:
        raise AssertionError(f'API entry is not a non-negative int: {entry!r}')
    if not isinstance(symbols, list) or not symbols:
        raise AssertionError('API metadata has no symbols array')
    by_symbol = {}
    generated_names = set()
    mains = 0
    projected = []
    for item in symbols:
        if not isinstance(item, dict):
            raise AssertionError(f'API symbol is not an object: {item!r}')
        symbol = item.get('symbol')
        source_name = item.get('source_name')
        generated = item.get('generated')
        if type(symbol) is not int or symbol < 0:
            raise AssertionError(f'API symbol id is not an int: {item!r}')
        if not isinstance(source_name, str) or not source_name:
            raise AssertionError(f'API source_name is not a string: {item!r}')
        if not isinstance(generated, str) or not generated:
            raise AssertionError(f'API generated name is not a string: {item!r}')
        if symbol in by_symbol:
            raise AssertionError(f'duplicate API symbol id: {symbol}')
        if generated in generated_names:
            raise AssertionError(f'duplicate API generated name: {generated}')
        if source_name == 'main':
            mains += 1
        by_symbol[symbol] = item
        generated_names.add(generated)
        projected.append({'symbol': symbol, 'source_name': source_name,
                          'generated': generated})
    if mains > 1:
        raise AssertionError('ambiguous API entry: more than one main symbol')
    if entry not in by_symbol:
        raise AssertionError(
            f'API entry {entry} is absent from the published symbols')
    origin = by_symbol[entry]
    if origin['source_name'] != 'main':
        raise AssertionError(
            f'API entry {entry} has source_name {origin["source_name"]!r}')
    if origin['generated'] != '_f' + str(entry):
        raise AssertionError(
            f'published entry name {origin["generated"]!r} does not satisfy '
            f'the unchanged observer naming for entry {entry}')
    return {'contract': 'compiler-dev/1', 'backend': 'python',
            'word_bits': 64, 'entry_symbol': entry, 'symbols': projected}


def compare_generated(first, second, label='candidate'):
    """CLI and API bytes must be identical before any metadata is trusted."""
    for name in ('program.py', 'langrt.py'):
        left = sha(Path(first) / name)
        right = sha(Path(second) / name)
        if left != right:
            raise AssertionError(
                f'{label}: CLI/API differ on {name} ({left} != {right})')


def require_rejection(record, out, codes, label='rejection'):
    """A rejected Source keeps its original output and publishes no program."""
    if record.get('exit') != 1:
        raise AssertionError(
            f'{label}: expected a Source rejection (exit 1), '
            f'got {record.get("exit")!r}')
    if not record.get('stdout'):
        raise AssertionError(f'{label}: rejection output was not retained')
    try:
        data = json.loads(record['stdout'])
    except json.JSONDecodeError as error:
        raise AssertionError(
            f'{label}: rejection output is not JSON: {error}') from error
    present = {item.get('code') for item in data.get('diagnostics', [])
               if isinstance(item, dict)}
    for code in codes:
        if code not in present:
            raise AssertionError(
                f'{label}: rejection lost code {code}: '
                f'{sorted(map(str, present))}')
    for name in ('program.py', 'langrt.py'):
        if (Path(out) / name).exists():
            raise AssertionError(f'{label}: rejection published {name}')
    return data


def rejection_snapshot(out):
    entries = _scan_output_tree(out)
    if out.exists() or out.is_symlink():
        entries['.'] = _output_entry(out, '.')[1]
    return entries


def validate_registration(order):
    if list(order) != list(NAMED_CASES):
        raise AssertionError(
            'registered cases differ from the fixed 20-case plan: '
            f'{list(order)!r}')
    return True


def write_evidence(path, data):
    """Publish one evidence file by writing a sibling then replacing it."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.partial')
    if isinstance(data, bytes):
        temporary.write_bytes(data)
    else:
        temporary.write_text(data)
    os.replace(temporary, path)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


class Runner:
    def __init__(self, args):
        self.args = args
        self.mode = dispatch_mode(args)
        self.rows = []
        self.registered = set()
        self.registered_order = []
        self.commands = []
        self.env = dict(os.environ)
        for name in THREAD_ENV:
            self.env[name] = '1'
        self.env['PYTHONDONTWRITEBYTECODE'] = '1'
        self.env['PYTHONNOUSERSITE'] = '1'
        self.env.pop('PYTHONPATH', None)
        # Freeze the fixed observer uname tool and the observer PATH before any
        # real paired command starts; legacy mode never reads this new path.
        self.uname_identity = None
        if self.mode == 'paired':
            self.uname_identity = freeze_observer_uname(
                OBSERVER_UNAME_PATH, self.env.get('PATH', ''))

    def command(self, row, argv, cwd, timeout=180, audit_allowed=None,
                label='command', allow_uname=None):
        argv = [str(value) for value in argv]
        if audit_allowed is not None:
            return self.traced(row, argv, cwd, timeout, audit_allowed, label,
                               allow_uname=allow_uname)
        # Legacy helper path: unchanged argv, streams and record shape.
        proc = subprocess.run(argv, cwd=str(cwd), env=self.env,
                              capture_output=True, text=True, timeout=timeout)
        record = {
            'argv': argv,
            'cwd': str(Path(cwd).resolve()),
            'exit': proc.returncode,
            'stdout': proc.stdout,
            'stderr': proc.stderr,
        }
        row['commands'].append(record)
        self.commands.append(record)
        return record

    def traced(self, row, argv, cwd, timeout, audit_allowed, label,
               allow_uname=None):
        if not (math.isfinite(timeout) and timeout > 0):
            raise AssertionError(f'{label}: timeout must be finite and positive')
        if self.args.strace is None:
            raise AssertionError(f'{label}: traced command requires --strace')
        logs = self.args.out / 'logs'
        traces = self.args.out / 'traces'
        logs.mkdir(parents=True, exist_ok=True)
        traces.mkdir(parents=True, exist_ok=True)
        stem = f'{len(self.commands):03d}-{label_name(label)}'
        stdout_path = logs / f'{stem}.stdout'
        stderr_path = logs / f'{stem}.stderr'
        trace_path = traces / f'{stem}.trace'
        real_argv = [str(self.args.strace), '--seccomp-bpf', '-f', '-qq',
                     '-s', '16384', '-e', 'trace=execve,execveat',
                     '-o', str(trace_path), *argv]
        started = time.monotonic()
        process = _run_process_impl(real_argv, str(cwd), self.env, timeout)
        elapsed = time.monotonic() - started
        stdout_path.write_bytes(process['stdout'])
        stderr_path.write_bytes(process['stderr'])
        record = {
            'label': label,
            'argv': argv,
            'tool_argv': argv,
            'cwd': str(Path(cwd).resolve()),
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
            'trace_path': str(trace_path),
            'trace_sha256': sha(trace_path) if trace_path.is_file() else '',
        }
        # Retain the raw streams and process record before any check can
        # fail, then reject a timeout/dirty cleanup/dirty pipes uniformly and
        # only afterwards audit the exec whitelist.
        row['commands'].append(record)
        self.commands.append(record)
        require_clean(record, label)
        require_trace(record, audit_allowed, label, allow_uname=allow_uname)
        return record

    def project(self, name, files, binding):
        root = self.args.out / 'projects' / name
        if root.exists():
            shutil.rmtree(root)
        for relative, text in files.items():
            write(root / relative, text)
        write(root / 'lang.toml', f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
[dependencies.t10host]
kind = "python"
binding = "{binding}"
host_package = "t10host"
''')
        return root

    def observer_trace(self):
        # Compile and observer/exec whitelists stay separate: the candidate
        # compile may only exec the candidate tool, never the observed Python.
        if self.mode == 'legacy':
            return None
        return exec_allowlist(self.args.python)

    def require_success(self, record, expected_exit=0):
        """Paired records use the real process checks; legacy stays as before."""
        if self.mode == 'paired':
            return require_process(record, expected_exit)
        assert record['exit'] == expected_exit, record
        return record

    def emit(self, row, project, binding, expect_success=True):
        if self.mode == 'paired':
            return self.emit_paired(row, project, binding, expect_success)
        return self.emit_legacy(row, project, binding, expect_success)

    def emit_legacy(self, row, project, binding, expect_success=True):
        out = project / 'out'
        record = self.command(
            row,
            [self.args.helper, '--project', project / 'lang.toml',
             '--out-dir', out, '--std-binding', binding],
            project)
        if expect_success:
            assert record['exit'] == 0, record
            assert (out / 'program.py').exists(), record
            assert (out / 'langrt.py').exists(), record
            # The declared host module is copied alongside generated files.
            host = self.args.root / 'tests/python/t10_host.py'
            shutil.copyfile(host, out / 't10host.py')
            row['generated_files'] = {
                'program.py': sha(out / 'program.py'),
                'langrt.py': sha(out / 'langrt.py'),
                'verified.hir': sha(out / 'verified.hir'),
                'metadata.json': sha(out / 'metadata.json'),
            }
            code = (out / 'program.py').read_text()
            assert '_LANG_BINDING_INFO =' in code
            assert 'import std.py' not in code
        return out, record

    def emit_paired(self, row, project, binding, expect_success=True):
        """Candidate bytes come only from the public CLI; helper is reference."""
        manifest = project / 'lang.toml'
        out = project / 'out'
        previous = (rejection_snapshot(out)
                    if not expect_success and (out.exists() or out.is_symlink())
                    else None)
        cli_allow = exec_allowlist(self.args.candidate_cli)
        cli = self.command(
            row,
            [self.args.candidate_cli, 'build', '--project', manifest,
             '--backend', 'python', '--std-binding', binding,
             '--out-dir', out, '--format', 'json'],
            project, timeout=self.args.compile_timeout,
            audit_allowed=cli_allow, label='candidate-cli')
        row['candidate_cli'] = {
            'argv': cli['argv'], 'exit': cli['exit'],
            'trace_path': cli['trace_path'],
            'stdout_sha256': cli['stdout_sha256'],
            'stderr_sha256': cli['stderr_sha256'],
        }
        # A failed CLI aborts immediately: the reference helper below is never
        # consulted and can never supply candidate bytes.  A source rejection
        # must leave neither generated file, checked before the reference run.
        if expect_success:
            require_process(cli, 0)
        else:
            require_rejection(cli, out, (), 'candidate-cli')
            if previous is not None:
                require(rejection_snapshot(out) == previous,
                        'candidate-cli: rejection changed existing output directory')
        # The C++ helper is the reference emitter only; it is never used for
        # candidate bytes, metadata or HIR observation.  Paired mode runs it
        # through the same bounded+trace transport so its record carries the
        # real strict cleanup fields; legacy mode is unchanged.
        reference = project / 'reference-out'
        helper = self.command(
            row,
            [self.args.helper, '--project', manifest,
             '--out-dir', reference, '--std-binding', binding],
            project, timeout=self.args.compile_timeout,
            audit_allowed=exec_allowlist(self.args.helper),
            label='reference-helper')
        row['reference_emit'] = {
            'argv': helper['argv'], 'exit': helper['exit'],
            'stderr': helper['stderr'], 'trace_path': helper['trace_path'],
        }
        if not expect_success:
            # The formal CLI rejection is the assertion target; the helper
            # keeps its original rejection behaviour and retained output.
            require_process(helper, 1)
            require_rejection(helper, reference, (), 'reference-helper')
            return out, cli
        require_process(helper, 0)
        require((out / 'program.py').exists(), cli)
        require((out / 'langrt.py').exists(), cli)
        require((reference / 'program.py').exists(), helper)
        require((reference / 'langrt.py').exists(), helper)
        require(json.loads(cli['stdout']).get('status') == 'ok', cli)

        api_out = project / 'api-out'
        api = self.command(
            row,
            [self.args.api_driver, '--project', manifest,
             '--out-dir', api_out, '--std-binding', binding,
             '--format', 'json', '--emit-hir'],
            project, timeout=self.args.compile_timeout,
            audit_allowed=exec_allowlist(self.args.api_driver),
            label='candidate-api')
        row['candidate_api'] = {
            'argv': api['argv'], 'exit': api['exit'],
            'trace_path': api['trace_path'],
            'stdout_sha256': api['stdout_sha256'],
            'stderr_sha256': api['stderr_sha256'],
        }
        require_process(api, 0)
        document = json.loads(api['stdout'])
        metadata = api_projection(document)
        hir_text = require_hir(document)
        # Metadata and HIR are only published after the CLI and real API
        # generated bytes are byte-for-byte identical.
        compare_generated(out, api_out, 'candidate CLI/API')
        write_evidence(out / 'metadata.json',
                       json.dumps(metadata, ensure_ascii=False, indent=2) + '\n')
        write_evidence(out / 'verified.hir', hir_text)
        # The declared host module is copied alongside generated files.
        host = self.args.root / 'tests/python/t10_host.py'
        shutil.copyfile(host, out / 't10host.py')
        row['generated_files'] = {
            'program.py': sha(out / 'program.py'),
            'langrt.py': sha(out / 'langrt.py'),
            'verified.hir': sha(out / 'verified.hir'),
            'metadata.json': sha(out / 'metadata.json'),
        }
        code = (out / 'program.py').read_text()
        assert '_LANG_BINDING_INFO =' in code
        assert 'import std.py' not in code
        row['candidate_metadata'] = {
            'entry_symbol': metadata['entry_symbol'],
            'symbol_count': len(metadata['symbols']),
        }
        return out, cli

    def formal_cli(self, row, project, binding, out):
        if self.args.langc is None:
            row['cli_comparison'] = {'status': 'not_run',
                                     'reason': '--woozyc not supplied'}
            return
        cli_out = project / 'cli-out'
        audit = (exec_allowlist(self.args.langc)
                 if self.mode == 'paired' else None)
        record = self.command(
            row,
            [self.args.langc, 'build', '--project', project / 'lang.toml',
             '--backend', 'python', '--format', 'json',
             '--std-binding', binding, '--out-dir', cli_out],
            project, audit_allowed=audit, label='formal-cli')
        self.require_success(record, 0)
        for name in ('program.py', 'langrt.py'):
            require(sha(out / name) == sha(cli_out / name),
                    'formal CLI differs from helper for ' + name)
        row['cli_comparison'] = {
            'status': 'passed',
            'langc': str(self.args.langc),
            'program.py': sha(out / 'program.py'),
            'langrt.py': sha(out / 'langrt.py'),
        }

    def observe(self, row, out):
        record = self.command(row, [self.args.python, self.args.observe, out],
                              out, audit_allowed=self.observer_trace(),
                              label='observe', allow_uname=self.uname_identity)
        self.require_success(record, 0)
        lines = record['stdout'].splitlines()
        assert lines, record
        observed = json.loads(lines[-1])
        events = [line[len('T10HOST '):] for line in lines[:-1]
                  if line.startswith('T10HOST ')]
        return observed, events

    def real_observe(self, row, out, mode):
        record = self.command(
            row, [self.args.python, self.args.real_observer, out, mode], out,
            audit_allowed=self.observer_trace(), label='real-observe',
            allow_uname=self.uname_identity)
        self.require_success(record, 0)
        lines = record['stdout'].splitlines()
        assert lines, record
        return json.loads(lines[-1])

    def direct(self, row, out, timeout=180):
        return self.command(row, [self.args.python, '-s', out / 'program.py'],
                            out, timeout=timeout,
                            audit_allowed=self.observer_trace(),
                            label='program')

    def add(self, name, fn):
        require(name in NAMED_CASES, 'case is not in the fixed 20: ' + name)
        self.registered.add(name)
        self.registered_order.append(name)
        if self.args.only and name not in self.args.only:
            return None
        row = {'name': name, 'status': 'failed', 'commands': []}
        self.rows.append(row)
        try:
            fn(row)
            row['status'] = 'passed'
        except BaseException as error:
            row['error'] = {
                'type': type(error).__module__ + '.' + type(error).__qualname__,
                'message': str(error),
            }
        return row


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def non_error_events(events):
    return [event for event in events if not event.startswith('ERR ')]


def error_events(events):
    return [event for event in events if event.startswith('ERR ')]


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--woozyc', dest="langc", type=Path)
    parser.add_argument('--candidate-cli', type=Path,
                        help='real public Wz CLI for candidate Python builds')
    parser.add_argument('--api-driver', type=Path,
                        help='test-only pythonobs driver using the real '
                             'PythonEmitter API')
    parser.add_argument('--strace', type=Path,
                        help='fixed strace used to audit candidate compiles '
                             'and observations')
    parser.add_argument('--compile-timeout', type=finite_positive, default=180.0,
                        help='per-command candidate CLI/API/reference budget, '
                             'at most the original 180 seconds')
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--python', type=Path, required=True)
    parser.add_argument('--binding', type=Path, required=True)
    parser.add_argument('--example-binding', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--observe', type=Path, required=True)
    parser.add_argument('--original-observer', type=Path, required=True)
    parser.add_argument('--reference-observer', type=Path, required=True)
    parser.add_argument('--real-observer', type=Path)
    parser.add_argument('--only', nargs='+', default=[],
                        help='run only these named cases in this invocation')
    args = parser.parse_args(argv)
    # Validate the role/budget/output before any external process starts.
    mode = dispatch_mode(args)
    prepare_output(args.out, strict=(mode == 'paired'))
    # Keep the venv interpreter path itself: resolving its symlink selects the
    # base CPython and loses the dedicated NumPy/PyTorch site-packages.
    args.python = Path(os.path.abspath(str(args.python)))
    for key in ('helper', 'root', 'out', 'binding', 'example_binding',
                'reference', 'observe', 'original_observer', 'reference_observer',
                'candidate_cli', 'api_driver', 'strace'):
        value = getattr(args, key)
        if value is not None:
            setattr(args, key, value.resolve())
    if args.langc is not None:
        args.langc = args.langc.resolve()
    if args.real_observer is None:
        args.real_observer = args.root / 'tests/python/t10_real_objects.py'
    else:
        args.real_observer = args.real_observer.resolve()
    r = Runner(args)
    require(r.mode == mode, 'runner mode changed after dispatch')
    binding = str(args.binding)
    example_binding = str(args.example_binding)

    order_source = '''package main;
import "std/py" as py;
import "t10host" as host;
func pass(p host.Probe) host.Probe { return p; }
func main() i32 throws py.Error {
    let p = host.make();
    let q = p;
    host.set_value(q, 42);
    if host.get_value(p) != 42 { return 1; }
    host.same(p, q);
    let r = pass(p);
    host.same(p, r);
    host.optional(1, c: 3);
    host.optional(c: 3, a: 1);
    host.optional(7);
    let m = host.marker;
    host.check_marker(m);
    if p(0) != 42 { return 2; }
    host.return_none();
    host.make().ping(host.arg(1));
    try { host.make_bad().broken(); return 10; } catch e py.Error { let saved = e; host.inspect(saved); }
    try { host.make().ping(host.arg_fail(2)); return 11; } catch e py.Error { host.inspect(e); }
    try { host.make().fail_call(); return 12; } catch e py.Error { host.inspect(e); }
    try { host.error_source(); return 13; } catch e py.Error { host.inspect(e); }
    return 0;
}
'''
    expected_nonerror = [
        'MAKE', 'SET 42', 'GET 42', 'SAME True', 'SAME True',
        'OPTIONAL a=1 b=MISSING c=3', 'OPTIONAL a=1 b=MISSING c=3',
        'OPTIONAL a=7 b=MISSING c=MISSING', 'MARKER Probe True',
        'CALL call 0',
        'MAKE', 'READ ping', 'ARG 1', 'CALL ping 1',
        'MAKE_BAD', 'READ broken', 'READFAIL broken',
        'MAKE', 'READ ping', 'ARG_FAIL 2',
        'MAKE', 'READ fail_call', 'CALL fail_call',
    ]
    expected_errors = [
        ('t10host.HostOuter', 't10host.Probe.broken',
         't10host.HostInner:inner read cause', 'traceback=yes'),
        ('builtins.ValueError', 't10host.arg_fail', 'cause=None',
         'traceback=no'),
        ('builtins.RuntimeError', 't10host.Probe.fail_call', 'cause=None',
         'traceback=no'),
        ('t10host.HostOuter', 't10host.error_source',
         't10host.HostInner:inner cause', 'traceback=yes'),
    ]

    def order_case(row):
        project = r.project('order-objects-errors', {'src/main.wz': order_source}, binding)
        out, _ = r.emit(row, project, binding)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(observed['frames'] >= 1, observed)
        require(non_error_events(events) == expected_nonerror, events)
        errors = error_events(events)
        require(len(errors) == len(expected_errors), events)
        for event, (kind, operation, cause, traceback) in zip(errors, expected_errors):
            require('type=' + kind in event, event)
            require('operation=' + operation in event, event)
            require(cause in event, event)
            require(traceback in event, event)
            require('file=' in event and event.split('begin=')[1].split()[0].isdigit(),
                    event)
        info = observed['binding_info']
        bundle = info['bundles'][0]
        require(info['runtime_version'] == 'compiler-dev/1-python-values/10', info)
        require(bundle['contract'] == 'compiler-dev/1', bundle)
        require(bundle['sha256'] == sha(args.binding), bundle)
        require(bundle['review_status'] == 'draft_requires_conformance', bundle)
        require(any(m['logical_path'] == 't10host' and m['host_module'] == 't10host'
                    for m in bundle['modules']), bundle)
        require(any(r['package'] == 't10host' for r in bundle['package_requirements']),
                bundle)
        row['observation'] = observed
        row['events'] = events

    r.add('order-objects-errors', order_case)

    foreign_ownership_source = '''package main;
import "std/py" as py;
import "t10host" as host;

struct Box[T] { value T; }
struct Inner { p host.Probe; }
struct Outer { inner Inner; v Vec[i32]; }
enum Choice { One(p host.Probe); None; }
enum Maybe[T] { Some(value T); None; }
trait Sink { func keep(self Self, p move host.Probe) host.Probe; }
struct Holder { n i32; }
impl Sink for Holder {
  func keep(self Holder, p move host.Probe) host.Probe { return p; }
}
struct GenericHolder { n i32; }
impl GenericHolder {
  func keep[T](self GenericHolder, value move T) T { return move value; }
}
struct GenericBox[T] { value T; }
impl[T] GenericBox[T] {
  func keep[U](self Self, value move U) U { return move value; }
}
func pass(p host.Probe) host.Probe { return p; }
func take(p move host.Probe) host.Probe { return p; }
func relay[T](value move T) T { return move value; }
func apply(cb func(move host.Probe) host.Probe, p move host.Probe) host.Probe {
  return cb(move p);
}
func wrap(p host.Probe) Outer {
  return Outer(inner: Inner(p: p), v: [4]);
}
func pack(p host.Probe) Vec[host.Probe] {
  return [p];
}
func main() i32 throws py.Error {
  let x = host.make();
  let y = move x;
  if host.same(y, y) != true { return 1; }
  let values Vec[host.Probe] = [y];
  if host.same(y, values[0]) != true { return 2; }
  host.set_value(values[0], 42);
  if host.get_value(y) != 42 { return 3; }
  let part = values[0:1];
  if host.same(y, part[0]) != true { return 4; }
  let inner = Inner(p: pass(y));
  let outer = Outer(inner: inner, v: [1]);
  let wrapped = wrap(y);
  if host.same(wrapped.inner.p, y) != true { return 28; }
  let packed = pack(y);
  if host.same(packed[0], y) != true { return 29; }
  if host.same(y, outer.inner.p) != true { return 5; }
  host.set_value(outer.inner.p, 7);
  if host.get_value(y) != 7 { return 6; }
  var changed = outer;
  let q = host.make();
  changed.inner.p = q;
  if host.same(changed.inner.p, q) != true { return 7; }
  if host.get_value(y) != 7 { return 8; }
  if changed.v[0] != 1 { return 9; }
  var outer_move = outer;
  let moved_outer = move outer_move;
  if host.same(y, moved_outer.inner.p) != true { return 19; }
  var vec_move = values;
  let moved_vec = move vec_move;
  if host.same(y, moved_vec[0]) != true { return 20; }
  let direct_outer = move Outer(inner: Inner(p: y), v: [3]);
  if host.same(y, direct_outer.inner.p) != true { return 21; }
  let made = move host.make();
  host.set_value(made, 3);
  if host.get_value(made) != 3 { return 22; }
  let made_vec Vec[host.Probe] = [move made];
  if host.get_value(made_vec[0]) != 3 { return 23; }
  let choice = Choice.One(p: y);
  switch choice {
  case Choice.One(p):
    if host.same(p, y) != true { return 10; }
  case Choice.None: return 11;
  }
  let maybe = Maybe[host.Probe].Some(value: y);
  switch maybe {
  case Maybe.Some(p):
    if host.same(p, y) != true { return 26; }
  case Maybe.None: return 27;
  }
  let boxed = Box[host.Probe](value: y);
  if host.same(boxed.value, y) != true { return 12; }
  let moved = take(move q);
  if host.same(moved, moved) != true { return 13; }
  let relayed = relay[host.Probe](move moved);
  if host.same(relayed, relayed) != true { return 14; }
  let held = Holder(n: 1);
  let kept = held.keep(move relayed);
  if host.same(kept, kept) != true { return 15; }
  let temp = pass(y);
  let functional = apply(take, move temp);
  if host.same(functional, y) != true { return 16; }
  let result = apply(take, move functional);
  if host.same(result, y) != true { return 17; }
  for item in values {
    if host.same(item, y) != true { return 18; }
  }
  let gh = GenericHolder(n: 2);
  let generic_kept = gh.keep[host.Probe](move result);
  if host.same(generic_kept, y) != true { return 24; }
  let gb = GenericBox[host.Probe](value: y);
  let gb_kept = gb.keep[host.Probe](move generic_kept);
  if host.same(gb_kept, y) != true { return 25; }
  return 0;
}
'''

    def foreign_ownership_case(row):
        project = r.project('foreign-ownership',
                            {'src/main.wz': foreign_ownership_source}, binding)
        out, _ = r.emit(row, project, binding)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(not error_events(events), events)
        metadata = json.loads((out / 'metadata.json').read_text())
        entry = '_f' + str(metadata['entry_symbol'])
        x_states = [state for state in observed['owner_states']
                    if state['function'] == entry and state['name'] == 'x']
        require(any(state['active'] for state in x_states), x_states)
        require(x_states and not x_states[-1]['active'], x_states)
        require(len(observed['foreign_copies']) > 0, observed)
        require(len(observed['foreign_drops']) > 0, observed)
        makes = [op for op in observed['operations'] if op == ['host', 'MAKE']]
        require(len(makes) == 3, observed['operations'])
        for event in (['host', 'SET', 42], ['host', 'GET', 42],
                      ['host', 'SET', 7], ['host', 'GET', 7]):
            require(event in observed['operations'], (event, observed['operations']))
        row['observation'] = observed

    r.add('foreign-ownership-move-aggregate', foreign_ownership_case)

    error_aggregate_source = '''package main;
import "std/py" as py;
import "t10host" as host;

// py.Error storage keeps the same host exception payload in every copy.
struct Saved { value py.Error; n i32; }
struct Nested { saved Saved; values Vec[py.Error]; }
enum Slot { One(value py.Error); Empty; }
struct Box[T] { value T; }

func wrap(e py.Error) Saved { return Saved(value: e, n: 3); }
func pass_saved(s Saved) Saved { return s; }
func choose(slot Slot, fallback py.Error) py.Error {
  switch slot {
  case Slot.One(value): return value;
  case Slot.Empty: return fallback;
  }
}

func exercise(seed py.Error) i32 throws py.Error {
  var outside = Saved(value: seed, n: 0);
  try { host.error_source(); return 1; } catch e py.Error {
    host.inspect(e);
    let saved = Saved(value: e, n: 1);
    let copied = saved;
    host.inspect(copied.value);
    let moved = move copied;
    host.inspect(moved.value);
    let values Vec[py.Error] = [e, saved.value];
    host.inspect(values[0]);
    host.inspect(values[1]);
    let copy_values = values;
    host.inspect(copy_values[1]);
    let part = values[0:2];
    host.inspect(part[0]);
    let saved_values Vec[Saved] = [saved];
    host.inspect(saved_values[0].value);
    let nested = Nested(saved: saved, values: values);
    host.inspect(nested.saved.value);
    host.inspect(nested.values[0]);
    let slot = Slot.One(value: e);
    switch slot {
    case Slot.One(value):
      host.inspect(value);
    case Slot.Empty: return 2;
    }
    let boxed = Box[py.Error](value: e);
    host.inspect(boxed.value);
    let wrapped = wrap(e);
    host.inspect(wrapped.value);
    let returned = pass_saved(wrapped);
    host.inspect(returned.value);
    let picked = choose(slot, seed);
    host.inspect(picked);
    outside = Saved(value: e, n: 4);
  }
  host.inspect(outside.value);
  let moved_outside = move outside;
  host.inspect(moved_outside.value);
  try { throw moved_outside.value; return 3; } catch e py.Error {
    host.inspect(e);
  }
  let slot2 = Slot.One(value: moved_outside.value);
  try {
    switch slot2 {
    case Slot.One(value): throw value;
    case Slot.Empty: return 4;
    }
  } catch e py.Error { host.inspect(e); }
  let values2 Vec[py.Error] = [seed];
  try { throw values2[0]; return 5; } catch e py.Error { host.inspect(e); }
  return 0;
}

func main() i32 throws py.Error {
  try { host.error_source(); return 1; } catch e py.Error {
    return exercise(e);
  }
  return 0;
}
'''

    def error_aggregate_case(row):
        project = r.project('py-error-aggregate-storage',
                            {'src/main.wz': error_aggregate_source}, binding)
        out, _ = r.emit(row, project, binding)
        r.formal_cli(row, project, binding, out)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(not non_error_events(events), events)
        errors = error_events(events)
        # The first nineteen inspects share the exception caught in exercise.
        # The final throw/catch reuses the seed caught in main.
        require(len(errors) == 20, errors)
        keys = ('payload_id', 'exception_id', 'cause_id', 'traceback_id',
                'cause_traceback_id')
        def identity(event):
            found = {}
            for token in event.split():
                name, separator, value = token.partition('=')
                if separator and name in keys:
                    found[name] = value
            require(set(found) == set(keys), event)
            return found
        shared = identity(errors[0])
        for event in errors[1:19]:
            require(identity(event) == shared, (event, shared))
        seed = identity(errors[19])
        require(seed['exception_id'] != shared['exception_id'], (seed, shared))
        require(shared['cause_id'] != 'none' and shared['traceback_id'] != 'none'
                and shared['cause_traceback_id'] != 'none', shared)
        require(seed['cause_id'] != 'none' and seed['traceback_id'] != 'none'
                and seed['cause_traceback_id'] != 'none', seed)
        for event in errors:
            require('traceback=yes' in event, event)
            require('cause=t10host.HostInner:inner cause' in event, event)
        row['observation'] = observed
        row['events'] = events
        row['shared_identity'] = shared

    r.add('py-error-aggregate-storage', error_aggregate_case)

    foreign_cleanup_source = '''package main;
import "std/py" as py;
import "t10host" as host;

enum Failure { Bad(value Vec[host.Probe]); }
enum Bad { Stop; }
struct Pair { p host.Probe; n i32; }
func consume(p move host.Probe, n i64) i64 throws py.Error {
  if host.same(p, p) != true { return -1; }
  return n;
}
func fail_move(p move host.Probe) i32 throws Bad { throw Bad.Stop; }
func fail_agg(seed host.Probe) Vec[host.Probe] throws Failure {
  let values Vec[host.Probe] = [seed];
  throw Failure.Bad(value: values);
}
func fail_partial() i32 throws Bad { throw Bad.Stop; }
func keep_deferred(p move host.Probe) host.Probe throws py.Error {
  defer { try { host.same(p, p); host.note(7); } catch e py.Error { } }
  return p;
}
func main() i32 throws py.Error {
  let a = host.make();
  if consume(move a, host.arg(1)) != 1 { return 1; }
  let b = host.make();
  try { consume(move b, host.arg_fail(2)); return 2; }
  catch e py.Error { host.note(2); }
  let c = host.make();
  try { fail_move(move c); return 3; }
  catch e Bad { host.note(3); }
  try { let values = fail_agg(host.make()); return 4; }
  catch e Failure {
    switch e {
    case Failure.Bad(values):
      if host.same(values[0], values[0]) != true { return 5; }
    }
  }
  try { let pair = Pair(p: host.make(), n: fail_partial()); return 6; }
  catch e Bad { host.note(6); }
  let d = host.make();
  let deferred = keep_deferred(move d);
  if host.same(deferred, deferred) != true { return 7; }
  host.note(9);
  return 0;
}
'''

    def foreign_cleanup_case(row):
        project = r.project('foreign-cleanup',
                            {'src/main.wz': foreign_cleanup_source}, binding)
        out, _ = r.emit(row, project, binding)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(not error_events(events), events)
        metadata = json.loads((out / 'metadata.json').read_text())
        entry = '_f' + str(metadata['entry_symbol'])
        for name in ('a', 'b', 'c', 'd'):
            states = [state for state in observed['owner_states']
                      if state['function'] == entry and state['name'] == name]
            require(any(state['active'] for state in states), states)
            require(states and not states[-1]['active'], states)
        operations = [op for op in observed['operations']
                      if op[0] in ('host', 'foreign_drop')]
        def index_of(value, start=0):
            for index in range(start, len(operations)):
                if operations[index] == value:
                    return index
            raise AssertionError((value, operations))
        failed_arg = index_of(['host', 'ARG_FAIL', 2])
        note_after_arg = index_of(['host', 'NOTE', 2], failed_arg)
        require(any(op == ['foreign_drop']
                    for op in operations[failed_arg + 1:note_after_arg]),
                operations)
        made_b = index_of(['host', 'MAKE'], index_of(['host', 'NOTE', 2]))
        note_after_c = index_of(['host', 'NOTE', 3], made_b)
        require(any(op == ['foreign_drop']
                    for op in operations[made_b + 1:note_after_c]), operations)
        note_after_partial = index_of(['host', 'NOTE', 6], note_after_c)
        partial_make = max(index for index, op in enumerate(operations)
                           if op == ['host', 'MAKE'] and index < note_after_partial)
        require(any(op == ['foreign_drop']
                    for op in operations[partial_make + 1:note_after_partial]),
                operations)
        note_after_defer = index_of(['host', 'NOTE', 7], note_after_partial)
        defer_make = max(index for index, op in enumerate(operations)
                         if op == ['host', 'MAKE'] and index < note_after_defer)
        require(['host', 'SAME', True] in operations[defer_make + 1:note_after_defer],
                operations)
        note_after_return = index_of(['host', 'NOTE', 9], note_after_defer)
        require(any(op == ['foreign_drop']
                    for op in operations[note_after_defer + 1:note_after_return]),
                operations)
        require(len(observed['foreign_copies']) > 0, observed)
        require(len(observed['foreign_drops']) >= 5, observed)
        row['observation'] = observed

    r.add('foreign-ownership-cleanup', foreign_cleanup_case)

    generic_error_kinds = '''package kinds;
import "t10host" as host;

pub enum Failure[T] { Bad(value T); }
pub enum Phantom[T] { Bad; }

pub func fail(p host.Probe) Unit throws Failure[host.Probe] {
  throw Failure[host.Probe].Bad(value: p);
}
pub func fail_vec(p host.Probe) Unit throws Failure[Vec[host.Probe]] {
  let values Vec[host.Probe] = [p];
  throw Failure[Vec[host.Probe]].Bad(value: values);
}
pub func fail_phantom() Unit throws Phantom[host.Probe] {
  throw Phantom[host.Probe].Bad;
}
pub func fail_i32(value i32) Unit throws Failure[i32] {
  throw Failure[i32].Bad(value: value);
}
pub func unused() Unit throws Failure[Vec[host.Probe]] { }
'''

    # The enum lives in another module.  Every use below names it through an
    # import alias while the thrown identity stays the helper module symbol.
    # Wrong catch clauses come first so a key that dropped or confused a type
    # argument would take the wrong branch and return a nonzero code.
    generic_error_source = '''package main;
import "std/py" as py;
import "t10host" as host;
import "app/kinds" as k;
import "app/kinds" as alias;

struct Box[T] { value T; }
enum Concrete { Bad(value host.Probe); }

func fail_box(p host.Probe) Unit throws k.Failure[Box[host.Probe]] {
  throw k.Failure[Box[host.Probe]].Bad(value: Box[host.Probe](value: p));
}
func fail_error(e py.Error) Unit throws alias.Failure[py.Error] {
  throw alias.Failure[py.Error].Bad(value: e);
}

func main() i32 throws py.Error {
  var seen i32 = 0;
  let p = host.make();
  try { k.fail(p); return 1; }
  catch e k.Failure[Box[host.Probe]] { return 2; }
  catch e alias.Failure[host.Probe] {
    switch e {
    case k.Failure.Bad(value):
      if host.same(value, p) != true { return 3; }
      let copied = e;
      let moved = move copied;
      switch moved {
      case alias.Failure.Bad(inner):
        if host.same(inner, p) != true { return 4; }
        host.set_value(inner, 41);
      }
    }
    seen += 1;
    host.note(1);
  }
  if host.get_value(p) != 41 { return 5; }

  let q = host.make();
  try { k.fail_vec(q); return 6; }
  catch e k.Failure[host.Probe] { return 7; }
  catch e k.Failure[Vec[host.Probe]] {
    switch e {
    case k.Failure.Bad(values):
      if host.same(values[0], q) != true { return 8; }
      let copy = values;
      if host.same(copy[0], q) != true { return 9; }
    }
    seen += 2;
    host.note(2);
  }

  let r = host.make();
  try { fail_box(r); return 10; }
  catch e k.Failure[host.Probe] { return 11; }
  catch e k.Failure[Box[host.Probe]] {
    switch e {
    case k.Failure.Bad(boxed):
      if host.same(boxed.value, r) != true { return 12; }
    }
    seen += 4;
    host.note(4);
  }

  try { k.fail_phantom(); return 13; }
  catch e k.Phantom[host.Probe] {
    seen += 8;
    host.note(8);
  }

  try { k.fail_i32(7); return 14; }
  catch e k.Failure[i32] {
    switch e {
    case k.Failure.Bad(value):
      if value != 7 { return 15; }
    }
    seen += 16;
    host.note(16);
  }

  let s = host.make();
  try { throw Concrete.Bad(value: s); return 17; }
  catch e Concrete {
    switch e {
    case Concrete.Bad(value):
      if host.same(value, s) != true { return 18; }
    }
    seen += 32;
    host.note(32);
  }

  try { host.error_source(); return 19; }
  catch err py.Error {
    host.inspect(err);
    let nested = host.make();
    try { fail_error(err); return 20; }
    catch e k.Failure[host.Probe] { return 21; }
    catch e k.Failure[py.Error] {
      switch e {
      case k.Failure.Bad(value):
        host.inspect(value);
      }
      seen += 64;
      host.note(64);
    }
    if host.same(nested, nested) != true { return 22; }
  }

  if seen != 127 { return 23; }
  return 0;
}
'''

    def generic_error_key_case(row):
        project = r.project('generic-foreign-error-key',
                            {'src/main.wz': generic_error_source,
                             'src/kinds/main.wz': generic_error_kinds},
                            binding)
        out, _ = r.emit(row, project, binding)
        r.formal_cli(row, project, binding, out)
        # The stable keys must retain the full ordered concrete arguments and
        # the defining module, not a local alias or a session/binding number.
        keys = {node.value.decode('utf-8')
                for node in ast.walk(ast.parse((out / 'program.py').read_text()))
                if isinstance(node, ast.Constant)
                and isinstance(node.value, bytes)}
        expected_keys = {
            'app/kinds.Failure[@13:t10host.Probe]',
            'app/kinds.Failure[Vec[@13:t10host.Probe]]',
            'app/kinds.Failure[app.Box[@13:t10host.Probe]]',
            'app/kinds.Phantom[@13:t10host.Probe]',
            'app/kinds.Failure[i32]',
            'app/kinds.Failure[lang.py.Error]',
            'app.Concrete',
        }
        require(expected_keys <= keys, sorted(expected_keys - keys))
        row['typed_error_keys'] = sorted(expected_keys)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(non_error_events(events) == [
            'MAKE', 'SAME True', 'SAME True', 'SET 41', 'NOTE 1', 'GET 41',
            'MAKE', 'SAME True', 'SAME True', 'NOTE 2',
            'MAKE', 'SAME True', 'NOTE 4', 'NOTE 8', 'NOTE 16',
            'MAKE', 'SAME True', 'NOTE 32', 'MAKE', 'NOTE 64', 'SAME True',
        ], events)
        errors = error_events(events)
        require(len(errors) == 2, events)
        identity_keys = ('payload_id', 'exception_id', 'cause_id',
                         'traceback_id', 'cause_traceback_id')
        def identity(event):
            found = {}
            for token in event.split():
                name, separator, value = token.partition('=')
                if separator and name in identity_keys:
                    found[name] = value
            require(set(found) == set(identity_keys), event)
            return found
        # Inspecting the caught py.Error and the same payload re-thrown inside
        # Failure[py.Error] must keep the host exception's stable identity.
        require(identity(errors[0]) == identity(errors[1]), errors)
        for event in errors:
            require('traceback=yes' in event, event)
            require('cause=t10host.HostInner:inner cause' in event, event)
            require('operation=t10host.error_source' in event, event)
        # Catch bindings install owned slots; all must be cleared on exit.
        e_states = [state for state in observed['owner_states']
                    if state['name'] == 'e']
        require(any(state['active'] for state in e_states), e_states)
        require(e_states and not e_states[-1]['active'], e_states)
        require(len(observed['foreign_copies']) > 0, observed)
        require(len(observed['foreign_drops']) > 0, observed)
        row['observation'] = observed
        row['events'] = events

    r.add('generic-foreign-error-key', generic_error_key_case)

    # Ordinary host ids are opaque text.  These ids collide with source types
    # under the old plain format, and the comma/prefix-like ids also collide
    # with each other under an unescaped-prefix format.  Wrong catches come
    # first, so taking one returns a nonzero code.
    generic_error_key_escaping_source = '''package main;
import "t10host" as host;

struct Box[T] { value T; }
struct X[A,B] { a A; b B; }
enum E[T] { Bad; }
enum Pair[L,R] { Bad; }

func main() i32 {
  var seen i32 = 0;
  try { throw E[host.Other].Bad; }
  catch wrong E[Box[i32]] { return 7; }
  catch right E[host.Other] { seen += 1; }

  try { throw E[Box[i32]].Bad; }
  catch wrong E[host.Other] { return 8; }
  catch right E[Box[i32]] { seen += 2; }

  try { throw E[host.VecLike].Bad; }
  catch wrong E[Vec[i32]] { return 9; }
  catch right E[host.VecLike] { seen += 4; }

  try { throw E[host.FuncLike].Bad; }
  catch wrong E[func(i32) i32] { return 10; }
  catch right E[host.FuncLike] { seen += 8; }

  try { throw E[host.XLike].Bad; }
  catch wrong E[X[i32,i32]] { return 11; }
  catch right E[host.XLike] { seen += 16; }

  try { throw Pair[host.AB, host.C].Bad; }
  catch wrong Pair[host.A, host.BC] { return 12; }
  catch wrong2 Pair[host.C, host.AB] { return 13; }
  catch right Pair[host.AB, host.C] { seen += 32; }
  if seen != 63 { return 14; }
  return 0;
}
'''

    def generic_error_key_escaping_case(row):
        variant = json.loads(args.binding.read_text())
        for name, identity in (
                ('Other', 'app.Box[i32]'),
                ('A', 'a'),
                ('AB', 'a,@b'),
                ('BC', 'b,@c'),
                ('C', 'c'),
                ('VecLike', 'Vec[i32]'),
                ('FuncLike', 'func(i32)->(i32)'),
                ('XLike', 'app.X[i32,i32]')):
            variant['types'].append({
                'id': identity, 'module': 't10host', 'name': name,
                'kind': 'host_reference',
                'host_attribute': ['HostInner'],
                'copy': 'shared_reference',
            })
        variant_path = (args.out / 'variants' /
                        'key-escaping-binding.json')
        variant_path.parent.mkdir(parents=True, exist_ok=True)
        variant_path.write_text(
            json.dumps(variant, ensure_ascii=False, indent=2) + '\n')
        project = r.project('generic-error-key-escaping',
                            {'src/main.wz': generic_error_key_escaping_source},
                            variant_path)
        out, _ = r.emit(row, project, variant_path)
        r.formal_cli(row, project, variant_path, out)
        keys = {node.value.decode('utf-8')
                for node in ast.walk(ast.parse((out / 'program.py').read_text()))
                if isinstance(node, ast.Constant)
                and isinstance(node.value, bytes)}
        expected_keys = {
            'app.E[@12:app.Box[i32]]',
            'app.E[app.Box[i32]]',
            'app.E[@8:Vec[i32]]',
            'app.E[Vec[i32]]',
            'app.E[@16:func(i32)->(i32)]',
            'app.E[func(i32)->(i32)]',
            'app.E[@14:app.X[i32,i32]]',
            'app.E[app.X[i32,i32]]',
            'app.Pair[@4:a,@b,@1:c]',
            'app.Pair[@1:a,@4:b,@c]',
            'app.Pair[@1:c,@4:a,@b]',
        }
        require(expected_keys <= keys, sorted(expected_keys - keys))
        row['typed_error_keys'] = sorted(expected_keys)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(not events, events)
        require(observed['foreign_copies'] == [], observed)
        require(observed['foreign_drops'] == [], observed)
        row['observation'] = observed

    r.add('generic-error-key-escaping', generic_error_key_escaping_case)

    def foreign_rejections_case(row):
        cases = FOREIGN_REJECTION_CASES
        for name, body, code in cases:
            source = ('package main; import "std/py" as py; '
                      'import "t10host" as host; ' + body)
            project = r.project('foreign-reject-' + name,
                                {'src/main.wz': source}, binding)
            out, result = r.emit(row, project, binding, expect_success=False)
            require(result['exit'] == 1, result)
            data = json.loads(result['stdout'])
            codes = [d['code'] for d in data['diagnostics']]
            require(code in codes, (name, data))
            require(not (out / 'program.py').exists(), result)
            require(not (out / 'langrt.py').exists(), result)
        # A source enum's ordinary host type argument is not an error-root
        # foreign type, but declaring, throwing or catching host.Probe itself
        # must stay rejected.
        direct_cases = DIRECT_REJECTION_CASES
        for name, body in direct_cases:
            source = ('package main; import "std/py" as py; '
                      'import "t10host" as host; ' + body)
            project = r.project('foreign-reject-' + name,
                                {'src/main.wz': source}, binding)
            out, result = r.emit(row, project, binding, expect_success=False)
            require(result['exit'] == 1, result)
            data = json.loads(result['stdout'])
            require(any(d['code'] == 'E_CAPABILITY_UNAVAILABLE' and
                        'runtime_error declaration' in d['message']
                        for d in data['diagnostics']), data)
            require(not (out / 'program.py').exists(), result)
            require(not (out / 'langrt.py').exists(), result)
        source = ('package main; import "std/py" as py; '
                  'import "t10host" as host; '
                  'func main() i32 throws py.Error { let p = host.make(); '
                  'let values Vec[host.Probe] = [p]; return 0; }')
        project = r.project(WORD_BITS_REJECTION,
                            {'src/main.wz': source}, binding)
        manifest = (project / 'lang.toml').read_text().replace(
            'word_bits = 64', 'word_bits = 32')
        (project / 'lang.toml').write_text(manifest)
        out, result = r.emit(row, project, binding, expect_success=False)
        require(result['exit'] == 1, result)
        data = json.loads(result['stdout'])
        require(any(d['code'] == 'E_CAPABILITY_UNAVAILABLE'
                    for d in data['diagnostics']), data)
        require(not (out / 'program.py').exists(), result)
        require(not (out / 'langrt.py').exists(), result)
        row['rejections'] = cases
        row['rejection_plan'] = rejection_plan()

    r.add('foreign-ownership-rejections', foreign_rejections_case)

    internal_helper = '''package helper;
import "std/py" as py;
import "t10host" as host;
pub func fail() Unit throws py.Error { host.error_source(); }
'''
    internal_main = '''package main;
import "std/py" as py;
import "t10host" as host;
import "app/helper" as helper;
enum E { Bad; }
func bad_i64() i64 throws E { throw E.Bad; }
func main() i32 throws py.Error {
    var seen i32 = 0;
    try { host.take_i64(bad_i64()); return 1; } catch e E { host.note(1); seen += 1; }
    try { helper.fail(); return 2; } catch e py.Error { let copy = e; host.inspect(copy); seen += 2; }
    if seen != 3 { return 3; }
    return 0;
}
'''

    def internal_case(row):
        project = r.project('internal-cross-module',
                            {'src/main.wz': internal_main,
                             'src/helper/main.wz': internal_helper}, binding)
        out, _ = r.emit(row, project, binding)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(non_error_events(events) == ['NOTE 1'], events)
        errors = error_events(events)
        require(len(errors) == 1, events)
        require('type=t10host.HostOuter' in errors[0], errors)
        require('operation=t10host.error_source' in errors[0], errors)
        require('cause=t10host.HostInner:inner cause' in errors[0], errors)
        require('/src/helper/main.wz' in errors[0], errors)
        require('TAKE' not in non_error_events(events), events)
        row['observation'] = observed
        row['events'] = events

    r.add('internal-cross-module', internal_case)

    escape_main = '''package main;
import "std/py" as py;
import "t10host" as host;
func internal_escape() Unit throws py.Error { host.raise_internal(0); }
func main() i32 {
    try { internal_escape(); } catch e py.Error { return 1; }
    return 0;
}
'''

    def internal_escape_case(row):
        project = r.project('internal-escape', {'src/main.wz': escape_main}, binding)
        out, _ = r.emit(row, project, binding)
        observed, events = r.observe(row, out)
        require('typed' in observed, observed)
        require(observed['typed']['type'] == 't10.internal', observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        row['observation'] = observed

    r.add('internal-escape', internal_escape_case)

    def base_case(row, kind, expected_exit, marker, forbidden=()):
        source = f'''package main;
import "std/py" as py;
import "t10host" as host;
func main() Unit throws py.Error {{ host.raise_base({kind}); }}
'''
        project = r.project('base-' + str(kind), {'src/main.wz': source}, binding)
        out, _ = r.emit(row, project, binding)
        result = r.direct(row, out)
        if expected_exit is not None:
            require(result['exit'] == expected_exit, result)
        require(marker in result['stderr'], result)
        require('LANG_TYPED_ERROR' not in result['stderr'], result)
        for text in forbidden:
            require(text not in result['stderr'], result)
        row['result'] = result

    def keyboard_case(row):
        base_case(row, 0, None, 'KeyboardInterrupt')
        require(row['result']['exit'] != 0, row['result'])
    r.add('base-keyboard-interrupt', keyboard_case)
    r.add('base-system-exit', lambda row: base_case(
        row, 1, 7, '', forbidden=('LANG_PANIC',)))
    r.add('base-panic', lambda row: base_case(
        row, 2, 101, 'LANG_PANIC: host panic'))

    def bad_unit_case(row):
        source = '''package main;
import "std/py" as py;
import "t10host" as host;
func main() Unit throws py.Error { host.return_bad_unit(); }
'''
        project = r.project('bad-unit', {'src/main.wz': source}, binding)
        out, _ = r.emit(row, project, binding)
        result = r.direct(row, out)
        require(result['exit'] == 101, result)
        require('LANG_PANIC: foreign callable declared Unit returned a non-None value'
                in result['stderr'], result)
        require('LANG_TYPED_ERROR' not in result['stderr'], result)
        row['result'] = result

    r.add('bad-unit-contract', bad_unit_case)

    def no_throws_case(row):
        variant = json.loads(args.binding.read_text())
        for callable in variant['callables']:
            if callable['id'] == 't10host.note':
                callable['error_type'] = None
        variant_path = args.out / 'variants' / 'no-throws-binding.json'
        variant_path.parent.mkdir(parents=True, exist_ok=True)
        variant_path.write_text(json.dumps(variant, ensure_ascii=False, indent=2) + '\n')
        source = '''package main;
import "std/py" as py;
import "t10host" as host;
func main() Unit throws py.Error { host.note(1); }
'''
        project = r.project('no-throws', {'src/main.wz': source}, variant_path)
        out, result = r.emit(row, project, variant_path, expect_success=False)
        require(result['exit'] == 1, result)
        data = json.loads(result['stdout'])
        codes = [d['code'] for d in data['diagnostics']]
        messages = [d['message'] for d in data['diagnostics']]
        require('E_CAPABILITY_UNAVAILABLE' in codes, data)
        require(any('requires declared lang.py.Error' in message for message in messages),
                data)
        require(not (out / 'program.py').exists(), result)
        require(not (out / 'langrt.py').exists(), result)
        row['diagnostics'] = data['diagnostics']

    r.add('no-throws-external-callable', no_throws_case)

    for name, declaration in [
        ('foreign-value-error', 'func fail(x host.Probe) Unit throws host.Probe { throw x; }'),
        ('function-foreign-error', 'func fail(g func() host.Probe) Unit throws func() host.Probe { throw g; }'),
    ]:
        def unsupported_error_case(row, name=name, declaration=declaration):
            source = ('package main; import "std/py" as py; '
                      'import "t10host" as host; ' + declaration + ' func main() Unit {}')
            project = r.project(name, {'src/main.wz': source}, binding)
            out, result = r.emit(row, project, binding, expect_success=False)
            require(result['exit'] == 1, result)
            data = json.loads(result['stdout'])
            require(any(d['code'] == 'E_CAPABILITY_UNAVAILABLE' and d['primary'] is not None
                        and 'runtime_error declaration' in d['message']
                        for d in data['diagnostics']), data)
            require(not (out / 'program.py').exists() and not (out / 'langrt.py').exists(), result)
            row['diagnostics'] = data['diagnostics']
        r.add(name, unsupported_error_case)

    def missing_host_case(row):
        variant = json.loads(args.binding.read_text())
        variant['package_requirements'].append(
            {'package': 't10_missing_host', 'version_policy': 'test-only missing host'})
        variant['modules'].append(
            {'logical_path': 'missinghost', 'host_module': 't10_missing_host',
             'provider': 'python_package'})
        variant['callables'].append({
            'id': 'missinghost.ping', 'module': 'missinghost', 'name': 'ping',
            'call_kind': 'function', 'owner_type': None,
            'host_attribute': ['ping'], 'parameters': [],
            'return_type': 'Unit', 'error_type': 'lang.py.Error',
            'ownership': {'return': 'unit', 'storage': 'no_tensor_storage'},
            'effects': [], 'receiver_escapes': 'not_applicable'})
        variant_path = args.out / 'variants' / 'missing-host-binding.json'
        variant_path.parent.mkdir(parents=True, exist_ok=True)
        variant_path.write_text(json.dumps(variant, ensure_ascii=False, indent=2) + '\n')
        source = '''package main;
import "std/py" as py;
import "missinghost" as missing;
func main() Unit throws py.Error { missing.ping(); }
'''
        project = r.project('missing-host', {'src/main.wz': source}, variant_path)
        with (project / 'lang.toml').open('a') as manifest:
            manifest.write(f'''[dependencies.missinghost]
kind = "python"
binding = "{variant_path}"
host_package = "t10_missing_host"
''')
        out, _ = r.emit(row, project, variant_path)
        result = r.direct(row, out)
        require(result['exit'] != 0, result)
        require('HostEnvironmentError' in result['stderr'], result)
        require("cannot import declared host module 't10_missing_host'" in result['stderr'],
                result)
        require('LANG_TYPED_ERROR' not in result['stderr'], result)
        require('LANG_PANIC' not in result['stderr'], result)
        row['result'] = result

    r.add('missing-host-environment-error', missing_host_case)

    real_objects_source = '''package main;
import "std/py" as py;
import "numpy" as np;
import "torch" as torch;

// Ordinary host references: let copy and return must preserve identity.
func pass_array(value np.ndarray) np.ndarray {
    let copy = value;
    return copy;
}
func pass_tensor(value torch.Tensor) torch.Tensor {
    let copy = value;
    return copy;
}
func main() Unit throws py.Error {
    let values = np.linspace(0.0, 1.0, num: 3, dtype: np.float32);
    let array_copy = pass_array(values);
    let tensor = torch.tensor(array_copy, dtype: torch.float32, device: "cpu");
    let tensor_copy = pass_tensor(tensor);
    return;
}
'''
    real_error_source = '''package main;
import "std/py" as py;
import "numpy" as np;

func fail_negative_num() Unit throws py.Error {
    let values = np.linspace(0.0, 1.0, num: -1, dtype: np.float32);
    return;
}
func main() Unit throws py.Error {
    try { fail_negative_num(); } catch e py.Error { let saved = e; return; }
    return;
}
'''

    def real_project(name, source, packages):
        project = args.out / 'projects' / name
        if project.exists():
            shutil.rmtree(project)
        write(project / 'src/main.wz', source)
        dependencies = ''.join(f'''[dependencies.{package}]
kind = "python"
binding = "{args.example_binding}"
host_package = "{package}"
''' for package in packages)
        write(project / 'lang.toml', f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
''' + dependencies)
        return project

    def real_objects_case(row):
        project = real_project('real-objects', real_objects_source,
                               ('numpy', 'torch'))
        out, _ = r.emit(row, project, args.example_binding)
        observed = r.real_observe(row, out, 'identity')
        require(observed['mode'] == 'identity', observed)
        require(observed['status'] == 'passed', observed)
        require(all(observed['acceptance'].values()), observed)
        require(observed['generated']['pass_array'] != observed['generated']['pass_tensor'],
                observed)
        row['observation'] = observed

    r.add('real-ndarray-tensor-identity', real_objects_case)

    def real_error_case(row):
        project = real_project('real-host-exception', real_error_source,
                               ('numpy',))
        out, _ = r.emit(row, project, args.example_binding)
        observed = r.real_observe(row, out, 'error')
        require(observed['mode'] == 'error', observed)
        require(observed['status'] == 'passed', observed)
        require(all(observed['acceptance'].values()), observed)
        require(observed['payload']['type_key'] == 'lang.py.Error', observed)
        require(observed['payload']['type_name'] == 'builtins.ValueError', observed)
        require(observed['payload']['operation'] == 'numpy.linspace.scalar', observed)
        row['observation'] = observed

    r.add('real-host-exception', real_error_case)

    def original04_case(row):
        source = args.root / 'baseline/samples/04_numpy_torch.wz'
        expected_sha = 'beb672124e6ccc697c8c129d2f7aa3bf21640de0b4af7a058eefa907fc6e6797'
        require(sha(source) == expected_sha, sha(source))
        project = args.out / 'projects' / 'original-04'
        if project.exists():
            shutil.rmtree(project)
        write(project / 'src/main.wz', source.read_text())
        write(project / 'lang.toml', f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
[dependencies.numpy]
kind = "python"
binding = "{example_binding}"
host_package = "numpy"
[dependencies.torch]
kind = "python"
binding = "{example_binding}"
host_package = "torch"
''')
        out, emit_result = r.emit(row, project, args.example_binding)
        generated = json.loads((out / 'metadata.json').read_text())
        names = {s['source_name']: s['generated'] for s in generated['symbols']}
        require('train' in names, names)
        generated_result = r.command(
            row, [args.python, args.original_observer, out], out, timeout=300,
            audit_allowed=r.observer_trace(), label='original-observer',
            allow_uname=r.uname_identity)
        r.require_success(generated_result, 0)
        generated_row = json.loads(generated_result['stdout'].splitlines()[-1])
        require(generated_row['status'] == 'passed', generated_row)
        require(all(generated_row['acceptance'].values()), generated_row)
        reference_result = r.command(
            row, [args.python, args.reference_observer, args.reference], out,
            timeout=300, audit_allowed=r.observer_trace(),
            label='reference-observer', allow_uname=r.uname_identity)
        r.require_success(reference_result, 0)
        reference_row = json.loads(reference_result['stdout'].splitlines()[-1])
        require(reference_row['status'] == 'passed', reference_row)
        require(all(reference_row['acceptance'].values()), reference_row)
        require(reference_row['sha256'] == sha(args.reference), reference_row)
        generated_loss = generated_row['result']['value']
        reference_loss = reference_row['result']['value']
        require(math.isclose(generated_loss, reference_loss, rel_tol=1e-5,
                             abs_tol=1e-6),
                (generated_loss, reference_loss))
        row['source_sha256'] = expected_sha
        row['reference_sha256'] = reference_row['sha256']
        row['generated_observation'] = generated_row
        row['reference'] = reference_row
        row['comparison'] = {
            'generated_loss': generated_loss,
            'reference_loss': reference_loss,
            'rtol': 1e-5,
            'atol': 1e-6,
            'passed': True,
        }

    r.add('original-04-cpu', original04_case)

    # The fixed 20-case plan must be registered even on a subset run; --only
    # never changes the plan or lets a three-case wrapper stand in for it.
    validate_registration(r.registered_order)

    # shortcut: matched cases may run before an unknown-name error; collect
    # cases first if rejection before execution is needed.
    unknown_only = [name for name in args.only if name not in r.registered]
    if unknown_only:
        parser.error('unknown --only case(s): ' + ', '.join(unknown_only))

    status = 'passed' if all(row['status'] == 'passed' for row in r.rows) else 'failed'
    summary = {
        'status': status,
        'cases': r.rows,
        'commands': r.commands,
        'environment': {
            'python': str(args.python),
            'helper': str(args.helper),
            'binding_sha256': sha(args.binding),
            'example_binding_sha256': sha(args.example_binding),
            'reference_sha256': sha(args.reference),
        },
    }
    (args.out / 'results.json').write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'status': status,
                      'cases': {row['name']: row['status'] for row in r.rows},
                      'results': str(args.out / 'results.json')},
                     ensure_ascii=False))
    return 0 if status == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
