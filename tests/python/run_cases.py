#!/usr/bin/env python3
"""Real frontend -> VerifiedHirModule -> C++ emitter -> explicit Python 3.12.

The default mode is the unchanged C++ helper pipeline.  The optional paired
mode uses the public Wz CLI for candidate builds and the real pythonemit API
driver for Verified HIR and published entry/symbol metadata; the C++ helper
remains an independent reference emitter.  No .wz evaluator lives here.
Existing fixtures are read-only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import traceback
from datetime import datetime, timezone

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
COMPILER_ROOT = HERE.parents[1]
SELFHOST = COMPILER_ROOT / 'selfhost'
sys.path.insert(0, str(SELFHOST))
from corpus import (TraceError, _output_entry, _run_process_impl,
                    _scan_output_tree, audit_trace)  # noqa: E402

PAIRED_TIMEOUT_SECONDS = 30
PAIRED_ARGUMENTS = ('candidate_cli', 'api_driver', 'source_root',
                    'candidate_resources', 'strace')

PANIC_EXIT = 101
PANIC_MARKER = 'LANG_PANIC: '
CAUSES = {
    'panic-division-by-zero': 'integer division by zero',
    'panic-invalid-shift': 'invalid shift count',
    'panic-negative-shift': 'invalid shift count',
    'panic-dynamic-conversion': 'integer conversion overflow',
    'panic-remainder-zero': 'integer division by zero',
    'panic-shift-u64-count': 'invalid shift count',
    'panic-unsigned-conversion': 'integer conversion overflow',
}

EXTRA_SOURCES = {
    # Declarations alone are not executable aggregate/generic operations.
    'unused-aggregate-declaration': 'struct Box {value i32;} func main() {}',
    'unused-generic-declaration': 'func f[T](x T) T {return x;} func main() {}',
    'callee-once-before-arguments': '''
func tick(x inout i32) i32 { let old=x; x+=1; return old; }
func pair(a i32,b i32) i32 {return a*10+b;}
func choose(x inout i32) (func(i32,i32) i32) {x+=1; return pair;}
func main() i32 {
    var counter i32=0;
    let value=choose(inout counter)(tick(inout counter),tick(inout counter));
    if value!=12 || counter!=3 {return 1;}
    return 0;
}''',
    'lambda-recursion-discard': '''
const answer i32=42;
func even(x i32) bool {if x==0 {return true;} return odd(x-1);}
func odd(x i32) bool {if x==0 {return false;} return even(x-1);}
func factory() (func(i32) i32) {return func(x i32) i32 {return x*2;};}
func ignored(_ i32,_ i32) {}
func apply(f func(i32) i32,x i32) i32 {return f(x);}
func main() i32 {
    ignored(1,2);
    let discard=func(_ i32,_ i32) {};
    discard(3,4);
    let result=func(x i32) i32 {return x+1;}(41);
    let nested=func() (func(i32) i32) {return func(x i32) i32 {return x;};};
    if result!=answer || factory()(21)!=42 || nested()(17)!=17 {return 1;}
    if apply(func(x i32) i32 {return x+2;},40)!=42 {return 2;}
    if !even(8) || !odd(9) {return 3;}
    return 0;
}''',
    'inout-identity-and-copies': '''
func bump(x inout i32) {x+=1;}
func relay(x inout i32) {bump(inout x);}
func copied(x i32) i32 {var local=x; relay(inout local); return local;}
func flip(x inout bool) {x=!x;}
func mutate_then_panic(x inout i32) {x+=1;var zero i32=0;let result=x/zero;}
func main() i32 {
    var x i32=3;
    if copied(x)!=4 || x!=3 {return 1;}
    relay(inout x);
    if x!=4 {return 2;}
    let f=func(x inout i32) {bump(inout x);};
    f(inout x);
    var b bool=false;
    flip(inout b);
    if x!=5 || !b {return 3;}
    return 0;
}''',
    'integer-and-bool-comparisons': '''
func signed(low i64, high i64) bool {
    return low < high && low <= high && high > low && high >= high &&
           low != high && low == low && !(high < low) && !(low > low);
}
func unsigned(low u64, high u64) bool {
    return low < high && low <= high && high > low && high >= high &&
           low != high && low == low && !(high <= low) && !(low >= high);
}
func word(low isize, high isize, small usize, large usize) bool {
    return low < high && large > small && !(large == small);
}
func flags(a bool, b bool) bool {return a != b && a == true && b == false;}
func main() i32 {
    if !signed(-9223372036854775808, 9223372036854775807) {return 1;}
    if !unsigned(9223372036854775807, 18446744073709551615) {return 2;}
    if !word(-1, 0, 0, 4294967296) {return 3;}
    if !flags(true, false) {return 4;}
    return 0;
}''',
    'short-circuit-effects': '''
func hit(x inout i32) bool {x+=1;return true;}
func main() i32 {
    var x i32=0;
    let a=false && hit(inout x);
    let b=true || hit(inout x);
    let c=true && hit(inout x);
    let d=false || hit(inout x);
    let e=(false && hit(inout x)) || (true && hit(inout x));
    if a || !b || !c || !d || !e || x!=3 {return 1;}
    return 0;
}''',
    'nested-loop-step-and-early-return': '''
func early() i32 {for {for {return 42;}}}
func main() i32 {
    var total i32=0;
    var steps i32=0;
    for var outer i32=0;outer<3;outer+=1 {
        for var inner i32=0;inner<5;inner+=1 {
            if inner==1 {continue;}
            if inner==3 {break;}
            total+=inner;
        }
        steps+=1;
        if outer==1 {continue;}
        total+=10;
    }
    if total!=26 || steps!=3 || early()!=42 {return 1;}
    return 0;
}''',
    'unit-main-and-variable-function': '''
func ping() {}
func pong() {}
func main() {
    var f=ping;
    f();
    f=pong;
    f();
    if true {} else {}
}''',
    'panic-remainder-zero': '''
func main() i32 {var a i64=-9223372036854775808;var b i64=0;let x=a%b;return 0;}
''',
    'panic-shift-u64-count': '''
func main() i32 {var x u8=1;var n u64=18446744073709551615;let y=x>>n;return 0;}
''',
    'panic-unsigned-conversion': '''
func main() i32 {var x i64=-1;let y=u64(x);return 0;}
''',
    'ordinary-nonzero': 'func main() i32 {return 101;}',
    'ordinary-other-nonzero': 'func main() i32 {var x i32=1;return x+6;}',
    'python-keywords': '''
func with(def i32,lambda i32) i32 {return def+lambda;}
func main() i32 {
    let __name__ i32=40;
    let _rt i32=2;
    if with(lambda:_rt,def:__name__)!=42 {return 1;}
    return 0;
}''',
}

UNSUPPORTED_SOURCES = {
    'float-arithmetic': 'func main() i32 {var x f32=1.0;let y=x+x;return 0;}',
    'float-comparison': 'func main() i32 {var x f64=1.0;let y=x==x;return 0;}',
    'string': 'func main() {let x="hello";}',
    'vec': 'func main() {let x=[1,2];}',
}

SOURCE_REJECTIONS = {
    'ordinary-parameter-rebinding': (
        'func f(x i32) {x=2;} func main() {}', 'E_TYPE_MISMATCH'),
}


# Keep the old source bytes and execute the now-supported operations.
for _ident in ('float-arithmetic', 'float-comparison', 'vec'):
    EXTRA_SOURCES[_ident] = UNSUPPORTED_SOURCES.pop(_ident)

# Capturing lambdas are now emitted and executed by the real Python backend.
EXTRA_SOURCES['capturing-lambda'] = '''
func main() i32 {
    var x i32=1;
    let f=func() i32 {return x;};
    x=2;
    if f()!=1 {return 1;}
    return 0;
}'''

def command(argv, **kwargs):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    # Avoid inherited import hooks changing generated-program behavior.
    env.pop('PYTHONPATH', None)
    env.pop('PYTHONOPTIMIZE', None)
    p = subprocess.run([str(a) for a in argv], text=True, capture_output=True,
                       timeout=30, env=env, **kwargs)
    return {'command': [str(a) for a in argv], 'returncode': p.returncode,
            'stdout': p.stdout, 'stderr': p.stderr}


def classify(result):
    if (result['returncode'] == PANIC_EXIT and not result['stdout'] and
            result['stderr'].startswith(PANIC_MARKER) and
            len(result['stderr'].splitlines()) == 1):
        return 'panic'
    if result['returncode'] == 0:
        return 'return'
    if 'SyntaxError:' in result['stderr']:
        return 'syntax_error'
    if 'Traceback (most recent call last):' in result['stderr']:
        return 'python_exception'
    return 'ordinary_nonzero'


def source_project(out, ident, body, *, bits=64):
    root = out / 'sources' / ident
    (root / 'src').mkdir(parents=True, exist_ok=True)
    (root / 'src/main.wz').write_text('package app;\n' + body + '\n')
    (root / 'lang.toml').write_text(
        'schema_version = 1\n[package]\nname = "python_test"\n'
        'source_root = "src"\nentry = "app"\n[build]\n'
        f'backend = "python"\nword_bits = {bits}\n')
    return root / 'lang.toml'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def integer_width_project():
    statements = []
    error = 1
    for signed in (True, False):
        for typ, bits in [(('i' if signed else 'u') + str(n), n) for n in (8, 16, 32, 64)] + [
                ('isize' if signed else 'usize', 64)]:
            lower = -(1 << (bits - 1)) if signed else 0
            upper = (1 << (bits - int(signed))) - 1
            statements.append(f'{{var high {typ}={upper};var low {typ}={lower};'
                              f'if high+1!={lower} || low-1!={upper} {{return {error};}}')
            if signed:
                statements.append(f'var neg {typ}=-1;if low/neg!={lower} || low%neg!=0 '
                                  f'{{return {error + 1};}}')
            statements.append('}')
            error += 2
    statements += ['var a i64=9223372036854775807;var b i64=3;',
                   f'if a/b!=3074457345618258602 || a%b!=1 {{return {error};}}',
                   'var u u64=18446744073709551615;var two u64=2;',
                   f'if u/two!=9223372036854775807 || u%two!=1 {{return {error + 1};}}',
                   'return 0;']
    return 'func main() i32 {' + '\n'.join(statements) + '}'


# Direct target-precision literals and value/inout transmission. Float
# observations use struct.pack on generated functions, never source evaluation.
FLOAT_LITERALS = {
    'zero32': ('f32', '0.0', '00000000'),
    'one32': ('f32', '1.0', '3f800000'),
    'tie32': ('f32', '1.000000059604644775390625', '3f800000'),
    'sub32': ('f32', '1.401298464324817070923729583289916131280e-45', '00000001'),
    'under32': ('f32', '1e-100', '00000000'),
    'over32': ('f32', '1e100', '7f800000'),
    'zero64': ('f64', '0.0', '0000000000000000'),
    'one64': ('f64', '1.0', '3ff0000000000000'),
    'tie64': ('f64', '1.00000000000000011102230246251565404236316680908203125', '3ff0000000000000'),
    'sub64': ('f64', '4.940656458412465441765687928682213723651e-324', '0000000000000001'),
    'under64': ('f64', '1e-1000', '0000000000000000'),
    'over64': ('f64', '1e1000', '7ff0000000000000'),
}


def float_project():
    parts = ['func pass32(x f32) f32 {return x;}',
             'func pass64(x f64) f64 {return x;}',
             'func set32(x inout f32) {x=1.5;}',
             'func set64(x inout f64) {x=1.5;}',
             'func probe32() f32 {var x f32=0.0;set32(inout x);return x;}',
             'func probe64() f64 {var x f64=0.0;set64(inout x);return x;}']
    for ident, (typ, literal, _) in FLOAT_LITERALS.items():
        bits = typ[1:]
        parts.append(f'func {ident}() {typ} {{let x {typ}={literal};'
                     f'var copy=x;copy=pass{bits}(copy);return copy;}}')
    parts.append('func main() i32 {return 0;}')
    return '\n'.join(parts)


def label_name(text):
    return ''.join(
        character if character.isalnum() or character in '-_.' else '_'
        for character in str(text))


def legacy_projection(record, paired):
    if not paired:
        return record
    return {
        'command': list(record['argv']),
        'returncode': record['exit'],
        'stdout': record['stdout'],
        'stderr': record['stderr'],
        'process': record,
    }


def json_document(text, label):
    if not text.strip():
        raise RuntimeError(f'{label}: command produced no stdout')
    try:
        document = json.loads(
            text,
            parse_constant=lambda value: (_ for _ in ()).throw(
                ValueError(f'non-standard JSON constant {value}')))
    except (json.JSONDecodeError, ValueError) as error:
        raise RuntimeError(f'{label}: malformed JSON stdout: {error}') from error
    if not isinstance(document, dict):
        raise RuntimeError(f'{label}: JSON stdout is not an object')
    return document


def metadata_rows(document, label):
    rows = document.get('symbols')
    if not isinstance(rows, list) or not rows:
        raise RuntimeError(f'{label}: metadata has no symbols array')
    selected = []
    by_symbol = {}
    by_name = {}
    generated_names = set()
    for item in rows:
        if not isinstance(item, dict):
            raise RuntimeError(f'{label}: symbol row is not an object: {item!r}')
        symbol = item.get('symbol')
        source_name = item.get('source_name')
        generated = item.get('generated')
        if type(symbol) is not int or symbol < 0:
            raise RuntimeError(
                f'{label}: symbol id is not a non-negative int: {item!r}')
        if not isinstance(source_name, str):
            raise RuntimeError(
                f'{label}: source_name is not a string: {item!r}')
        if not isinstance(generated, str) or not generated:
            raise RuntimeError(
                f'{label}: generated name is not a non-empty string: {item!r}')
        if symbol in by_symbol:
            raise RuntimeError(f'{label}: duplicate symbol id {symbol}')
        if generated in generated_names:
            raise RuntimeError(
                f'{label}: duplicate generated name {generated!r}')
        row = {'symbol': symbol, 'source_name': source_name,
               'generated': generated}
        selected.append(row)
        by_symbol[symbol] = row
        by_name.setdefault(source_name, []).append(row)
        generated_names.add(generated)
    return selected, by_symbol, by_name


def metadata_entry(document, label):
    entry = document.get('entry_symbol')
    if type(entry) is not int or entry < 0:
        raise RuntimeError(
            f'{label}: entry_symbol is not a non-negative int: {entry!r}')
    _, by_symbol, _ = metadata_rows(document, label)
    if entry not in by_symbol:
        raise RuntimeError(
            f'{label}: entry {entry} is absent from metadata symbols')
    row = by_symbol[entry]
    if row['source_name'] != 'main':
        raise RuntimeError(
            f"{label}: entry {entry} has source_name "
            f"{row['source_name']!r}, not main")
    return row['generated'], entry


def named_generated(document, names, label):
    _, _, by_name = metadata_rows(document, label)
    result = {}
    for name in names:
        rows = by_name.get(name, [])
        if len(rows) != 1:
            raise RuntimeError(
                f'{label}: expected exactly one {name!r} symbol, '
                f'found {len(rows)}')
        result[name] = rows[0]['generated']
    return result


def read_metadata_file(directory, label):
    path = Path(directory) / 'metadata.json'
    if not path.is_file():
        raise RuntimeError(f'{label}: no metadata.json in {directory}')
    try:
        document = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f'{label}: bad metadata.json: {error}') from error
    if not isinstance(document, dict):
        raise RuntimeError(f'{label}: metadata.json is not an object')
    metadata_rows(document, label)
    return document


def api_metadata(document, label):
    if document.get('status') != 'ok':
        raise RuntimeError(
            f"{label}: API status is not ok: {document.get('status')!r}")
    entry = document.get('entry')
    if type(entry) is not int or entry < 0:
        raise RuntimeError(
            f'{label}: API entry is not a non-negative int: {entry!r}')
    rows, by_symbol, _ = metadata_rows(document, label)
    if entry not in by_symbol:
        raise RuntimeError(f'{label}: API entry {entry} is absent from symbols')
    origin = by_symbol[entry]
    if origin['source_name'] != 'main':
        raise RuntimeError(
            f"{label}: API entry {entry} has source_name "
            f"{origin['source_name']!r}")
    hir = document.get('verified_hir')
    if not isinstance(hir, str) or not hir.strip():
        raise RuntimeError(f'{label}: real Verified HIR text is missing or empty')
    if ' scalar-v1 module=' not in hir:
        raise RuntimeError(f'{label}: verified_hir is not a real HIR dump')
    metadata = {
        'contract': 'compiler-dev/1',
        'backend': 'python',
        'word_bits': 64,
        'entry_symbol': entry,
        'symbols': rows,
    }
    return metadata, hir


def artifact_hashes(directory, label):
    hashes = {}
    for name in ('program.py', 'langrt.py', 'verified.hir', 'metadata.json'):
        path = Path(directory) / name
        if not path.is_file():
            raise RuntimeError(f'{label}: missing generated {name}')
        hashes[name] = sha(path)
    return hashes


def require_entry_files(directory, label):
    directory = Path(directory)
    for name in ('program.py', 'langrt.py'):
        if not (directory / name).is_file():
            raise RuntimeError(f'{label}: did not publish {name}')
    return directory


def compare_entries(first, second, label):
    for name in ('program.py', 'langrt.py'):
        left = sha(Path(first) / name)
        right = sha(Path(second) / name)
        if left != right:
            raise RuntimeError(
                f'{label}: CLI/API differ on {name} ({left} != {right})')


def output_tree(directory):
    directory = Path(directory)
    entries = _scan_output_tree(directory)
    if directory.exists() or directory.is_symlink():
        entries['.'] = _output_entry(directory, '.')[1]
    return entries


def write_evidence(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.partial')
    if isinstance(data, bytes):
        temporary.write_bytes(data)
    else:
        temporary.write_text(data, encoding='utf-8')
    os.replace(temporary, path)


def parse_process_json(record, label):
    if record.get('stderr'):
        raise RuntimeError(
            f"{label}: unexpected stderr: {record['stderr']!r}")
    return json_document(record.get('stdout', ''), label)


def require_ok_document(record, label):
    document = parse_process_json(record, label)
    if document.get('status') != 'ok' or document.get('diagnostics'):
        raise RuntimeError(
            f'{label}: expected ok without diagnostics: {document}')
    return document


def diagnostic_present(document, code, source=None, phase=None):
    if document.get('status') != 'error':
        return False
    for item in document.get('diagnostics', []):
        if not isinstance(item, dict) or item.get('code') != code:
            continue
        if phase is not None and item.get('phase') != phase:
            continue
        if source is None:
            return True
        primary = item.get('primary')
        if not isinstance(primary, dict):
            continue
        try:
            path = Path(primary['path']).resolve()
            start = primary.get('start_byte')
            end = primary.get('end_byte')
        except (KeyError, TypeError, OSError):
            continue
        if type(start) is not int or type(end) is not int:
            continue
        if (path == Path(source).resolve()
                and 0 <= start < end <= Path(source).stat().st_size):
            return True
    return False


def require_diagnostic(record, code, source, label, phase=None):
    document = parse_process_json(record, label)
    if not diagnostic_present(document, code, source, phase):
        raise RuntimeError(
            f'{label}: expected diagnostic {code} with the source span: '
            f'{document}')
    return document


def prepare_output(out, strict):
    out = Path(out)
    if out.exists():
        if not out.is_dir():
            raise RuntimeError(f'--out is not a directory: {out}')
        if strict and any(out.iterdir()):
            raise RuntimeError(
                f'refusing to overwrite existing evidence in --out: {out}')
    out.mkdir(parents=True, exist_ok=True)


def paired_environment(toolchain_root, out):
    env = dict(os.environ)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PYTHONNOUSERSITE'] = '1'
    env.pop('PYTHONPATH', None)
    env.pop('PYTHONOPTIMIZE', None)
    if toolchain_root is not None and Path(toolchain_root).is_dir():
        paths = []
        llvm = Path(toolchain_root) / 'llvm-22.1.8' / 'lib'
        libxml = Path(toolchain_root) / 'libxml2-legacy-2.13.9' / 'usr' / 'lib'
        if llvm.is_dir():
            paths.append(str(llvm))
        if libxml.is_dir():
            paths.append(str(libxml))
        if env.get('LD_LIBRARY_PATH'):
            paths.append(env['LD_LIBRARY_PATH'])
        env['LD_LIBRARY_PATH'] = ':'.join(paths)
    tmp = Path(out) / 'tmp'
    tmp.mkdir(parents=True, exist_ok=True)
    env['TMPDIR'] = str(tmp)
    return env


RESOURCE_PATHS = (
    'stdlib',
    'runtime',
    'runtime/python',
    'runtime/python/unicode18.py',
    'runtime/python/langrt.py',
)
RESOURCE_DIRECTORIES = ('stdlib', 'runtime', 'runtime/python')


def check_resources(resources, label='candidate'):
    resources = Path(resources)
    for relative in RESOURCE_PATHS:
        path = resources / relative
        if relative in RESOURCE_DIRECTORIES:
            if not path.is_dir():
                raise RuntimeError(f'{label} resources missing directory {path}')
        elif not path.is_file():
            raise RuntimeError(f'{label} resources missing file {path}')
    return resources


def resource_targets(root, label):
    root = Path(root)
    check_resources(root, label)
    return {relative: str((root / relative).resolve())
            for relative in RESOURCE_PATHS}


def verify_paired_resources(args):
    """The CLI reads stdlib/runtime next to its resolved executable.

    --candidate-resources is what the API driver receives.  Both roots must
    expose the same required directories/files through the same resolved
    targets, so a symlinked resource layout is accepted but a different
    directory or content set is rejected before the first command.
    """
    cli_root = Path(args.candidate_cli).resolve().parent
    api_root = Path(args.candidate_resources).resolve()
    cli_targets = resource_targets(cli_root, 'CLI')
    api_targets = resource_targets(api_root, 'API --candidate-resources')
    differences = {
        relative: {'cli': cli_targets[relative], 'api': api_targets[relative]}
        for relative in RESOURCE_PATHS
        if cli_targets[relative] != api_targets[relative]
    }
    if differences:
        raise RuntimeError(
            'candidate CLI resources and --candidate-resources resolve '
            f'differently: {differences}')
    return {'cli_root': str(cli_root), 'api_root': str(api_root),
            'targets': cli_targets}


def resource_snapshot(roots, label):
    """Capture both resource roots, including required directories and bytes."""
    entries = {}
    for root in roots:
        root = Path(root)
        for relative in ('stdlib', 'runtime'):
            base = root / relative
            if not base.is_dir():
                raise RuntimeError(
                    f'{label}: required resource directory missing {base}')
            pending = [base]
            pending.extend(base.rglob('*'))
            for path in pending:
                key = str(path)
                _, entry = _output_entry(path, key)
                entry['resolved'] = str(path.resolve())
                entries[key] = entry
    return entries


class PairedRunner:
    def __init__(self, args, resources):
        self.args = args
        self.resources = Path(resources)
        self.env = paired_environment(args.toolchain_root, args.out)
        self.commands = []
        self.driver = None
        (Path(args.out) / 'logs').mkdir(parents=True, exist_ok=True)
        (Path(args.out) / 'traces').mkdir(parents=True, exist_ok=True)

    def command(self, argv, cwd, label, allowed):
        timeout = PAIRED_TIMEOUT_SECONDS
        logs = Path(self.args.out) / 'logs'
        traces = Path(self.args.out) / 'traces'
        logs.mkdir(parents=True, exist_ok=True)
        traces.mkdir(parents=True, exist_ok=True)
        stem = f'{len(self.commands):03d}-{label_name(label)}'
        stdout_path = logs / f'{stem}.stdout'
        stderr_path = logs / f'{stem}.stderr'
        trace_path = traces / f'{stem}.trace'
        tool_argv = [str(part) for part in argv]
        real_argv = [str(self.args.strace), '--seccomp-bpf', '-f', '-qq',
                     '-s', '16384', '-e', 'trace=execve,execveat', '-o',
                     str(trace_path), *tool_argv]
        started = time.monotonic()
        process = _run_process_impl(real_argv, str(cwd), self.env, timeout)
        elapsed = time.monotonic() - started
        stdout_path.write_bytes(process['stdout'])
        stderr_path.write_bytes(process['stderr'])
        record = {
            'label': label,
            'argv': tool_argv,
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
        # Persist the raw streams and record before any assertion can fail.
        self.commands.append(record)
        with (Path(self.args.out) / 'commands.jsonl').open(
                'a', encoding='utf-8') as stream:
            stream.write(json.dumps(record, ensure_ascii=False,
                                    sort_keys=True) + '\n')
        if type(record['exit']) is not int:
            raise RuntimeError(f'{label}: exit is not a real int')
        for key in ('timed_out', 'cleanup_complete', 'abandoned_pipes'):
            if type(record[key]) is not bool:
                raise RuntimeError(f'{label}: {key} is not a real bool')
        if (record['timed_out'] or not record['cleanup_complete']
                or record['abandoned_pipes']):
            raise RuntimeError(
                f"{label}: process was not clean: exit {record['exit']} "
                f"timed_out={record['timed_out']} "
                f"cleanup_complete={record['cleanup_complete']} "
                f"abandoned_pipes={record['abandoned_pipes']}")
        if not trace_path.is_file():
            raise RuntimeError(f'{label}: command was not traced')
        allowed = {str(Path(item).resolve()) for item in allowed}
        try:
            audit = audit_trace(trace_path, allowed)
        except TraceError as error:
            raise RuntimeError(
                f'{label}: exact exec whitelist rejected trace: '
                f'{error}') from error
        if set(audit['observed']) != allowed:
            raise RuntimeError(
                f"{label}: expected exactly {sorted(allowed)}, "
                f"observed {audit['observed']}")
        record['trace_audit'] = audit
        return record

    def require_exit(self, record, expected, label):
        if record['exit'] != expected:
            raise RuntimeError(
                f"{label}: exit {record['exit']} != expected {expected}\n"
                f"stdout:\n{record['stdout']}\nstderr:\n{record['stderr']}")
        return record

    def candidate_check(self, manifest, label):
        return self.command(
            [self.args.candidate_cli, 'check', '--project', manifest,
             '--backend', 'python', '--format', 'json'],
            Path(manifest).parent, label, {self.args.candidate_cli})

    def candidate_build(self, manifest, directory, label):
        return self.command(
            [self.args.candidate_cli, 'build', '--project', manifest,
             '--backend', 'python', '--out-dir', directory,
             '--format', 'json'],
            Path(manifest).parent, label, {self.args.candidate_cli})

    def api_emit(self, manifest, directory, label):
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        return self.command(
            [self.driver, '--project', manifest, '--out-dir', directory,
             '--format', 'json', '--emit-hir'],
            Path(manifest).parent, label, {self.driver})

    def reference_emit(self, manifest, directory, label):
        return self.command(
            [self.args.helper, '--project', manifest, '--out-dir', directory],
            Path(manifest).parent, label, {self.args.helper})

    def python(self, argv, cwd, label):
        if (not argv
                or Path(argv[0]).resolve() != Path(self.args.python).resolve()):
            raise RuntimeError(
                f'{label}: only the explicit Python interpreter may run')
        return self.command(argv, cwd, label, {self.args.python})

    def prepare_driver(self):
        root = Path(self.args.out) / 'driver' / 'root'
        root.mkdir(parents=True, exist_ok=True)
        target = root / 'pythonobs'
        shutil.copy2(self.args.api_driver, target)
        target.chmod(0o755)
        for name in ('stdlib', 'runtime'):
            link = root / name
            if link.exists() or link.is_symlink():
                link.unlink()
            try:
                link.symlink_to(self.resources / name, target_is_directory=True)
            except OSError:
                shutil.copytree(self.resources / name, link)
        source_sha = sha(self.args.api_driver)
        staged_sha = sha(target)
        if source_sha != staged_sha:
            raise RuntimeError('staged API driver differs from --api-driver')
        self.driver = target
        return {
            'source': str(self.args.api_driver),
            'source_sha256': source_sha,
            'path': str(target),
            'sha256': staged_sha,
            'resources': str(self.resources),
        }


def snapshot_inputs(args, resources, published, cases_file,
                    resource_roots=None):
    paths = set()
    out = Path(args.out).resolve()

    def add(path):
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

    add(cases_file)
    for case in published['cases']:
        manifest = args.root / case['manifest']
        add(manifest)
        for path in manifest.parent.rglob('*'):
            add(path)
    for ident in ('core', 'numeric'):
        for path in (args.root / f'tests/fixtures/{ident}').rglob('*'):
            add(path)
    add(Path(__file__).resolve())
    add(COMPILER_ROOT / 'selfhost/corpus.py')
    add(args.root / 'tests/python/CMakeLists.txt')
    for path in (COMPILER_ROOT / 'selfhost/tests/pythonobs').rglob('*'):
        add(path)
    for path in Path(args.source_root).rglob('*.wz'):
        add(path)
    roots = ([Path(resources)] if resource_roots is None
             else [Path(root) for root in resource_roots])
    for root in roots:
        check_resources(root, 'snapshot')
        for relative in ('stdlib', 'runtime'):
            base = root / relative
            add(base)
            for path in base.rglob('*'):
                add(path)
    for tool in (args.helper, args.candidate_cli, args.api_driver, args.python,
                 args.strace):
        add(tool)
    if args.toolchain_root is not None:
        for relative in (
                'llvm-22.1.8/bin/opt', 'llvm-22.1.8/bin/llc',
                'llvm-22.1.8/bin/clang', 'llvm-22.1.8/bin/clang++',
                'llvm-22.1.8/bin/ld.lld',
                'libxml2-legacy-2.13.9/usr/lib/libxml2.so.2.13.9'):
            add(Path(args.toolchain_root) / relative)
    return {str(path): sha(path) for path in sorted(paths)}


class PairedFailure(RuntimeError):
    pass


class LegacyExecutor:
    paired = False

    def __init__(self, args, report):
        self.args = args
        self.report = report

    def python(self, argv, cwd, label):
        return command(argv, cwd=cwd)

    def metadata(self, directory, label):
        return read_metadata_file(directory, label)

    def compile_case(self, ident, manifest, group, expected='return', status=0):
        directory = self.args.out / 'generated' / ident
        built = command([self.args.helper, '--project', manifest,
                         '--out-dir', directory])
        row = {'id': ident, 'manifest': str(manifest), 'expected': expected,
               'expected_returncode': PANIC_EXIT if expected == 'panic' else status,
               'compile': built,
               'pipeline': 'load_project -> resolve_project -> check_project -> hir::verify -> python::emit'}
        self.report[group].append(row)
        if built['returncode']:
            row['status'] = 'failed'
            self.report['failures'].append(ident + ': frontend/emitter failed')
            return None
        result = command([self.args.python, '-s', directory / 'program.py'])
        row.update(execute=result, actual=classify(result))
        row['generated_sha256'] = {
            name: sha(directory / name)
            for name in ('program.py', 'langrt.py', 'verified.hir',
                         'metadata.json')}
        if expected == 'panic':
            passed = (row['actual'] == 'panic' and
                      result['stderr'] == PANIC_MARKER + CAUSES[ident] + '\n')
        else:
            passed = (row['actual'] in ('return', 'ordinary_nonzero') and
                      result['returncode'] == status and not result['stdout']
                      and not result['stderr'])
        row['status'] = 'passed' if passed else 'failed'
        if not passed:
            self.report['failures'].append(
                ident + ': unexpected generated-program behavior')
        return directory

    def compile_unsupported(self, ident, manifest):
        directory = self.args.out / 'generated' / ('unsupported-' + ident)
        result = command([self.args.helper, '--project', manifest,
                          '--out-dir', directory])
        source = manifest.parent / 'src/main.wz'
        parsed = json.loads(result['stdout'])
        passed = (result['returncode'] == 1
                  and diagnostic_present(
                      parsed, 'E_CAPABILITY_UNAVAILABLE', source)
                  and not directory.exists())
        self.report['unsupported'].append(
            {'id': ident, 'compile': result,
             'status': 'unsupported' if passed else 'failed'})
        if not passed:
            self.report['failures'].append('unsupported diagnostic: ' + ident)

    def compile_source_rejection(self, ident, manifest, expected_code):
        directory = self.args.out / 'generated' / ('rejected-' + ident)
        result = command([self.args.helper, '--project', manifest,
                          '--out-dir', directory])
        source = manifest.parent / 'src/main.wz'
        parsed = json.loads(result['stdout'])
        passed = (result['returncode'] == 1
                  and diagnostic_present(parsed, expected_code, source)
                  and not directory.exists())
        self.report['source_rejections'].append(
            {'id': ident, 'expected_code': expected_code, 'compile': result,
             'status': 'rejected' if passed else 'failed'})
        if not passed:
            self.report['failures'].append('source rejection: ' + ident)

    def compile_profile32(self, manifest):
        directory = self.args.out / 'generated' / 'profile32'
        rejected = command([self.args.helper, '--project', manifest,
                            '--out-dir', directory])
        parsed = json.loads(rejected['stdout'])
        passed = (rejected['returncode'] == 1
                  and any(d['phase'] == 'codegen'
                          and d['code'] == 'E_CAPABILITY_UNAVAILABLE'
                          for d in parsed['diagnostics']))
        self.report['unsupported'].append(
            {'id': 'python32-profile', 'compile': rejected,
             'status': 'unsupported' if passed else 'failed'})
        if not passed:
            self.report['failures'].append('Python32 emitter diagnostic')


class PairedExecutor:
    paired = True

    def __init__(self, args, report, runner):
        self.args = args
        self.report = report
        self.runner = runner

    def python(self, argv, cwd, label):
        return legacy_projection(
            self.runner.python(argv, cwd, label), paired=True)

    def metadata(self, directory, label):
        return read_metadata_file(directory, label)

    def compile_case(self, ident, manifest, group, expected='return', status=0):
        directory = self.args.out / 'generated' / ident
        reference = self.args.out / 'reference' / ident
        api_dir = self.args.out / 'api' / ident
        row = {'id': ident, 'manifest': str(manifest), 'expected': expected,
               'expected_returncode': PANIC_EXIT if expected == 'panic' else status,
               'compile': None,
               'pipeline': 'public-cli build --backend python -> API VerifiedHIR/emit -> explicit Python 3.12',
               'paired_pipeline': (
                   'candidate-cli check -> candidate-cli build -> '
                   'pythonobs --emit-hir -> byte compare -> metadata/HIR -> '
                   'helper reference -> explicit Python')}
        self.report[group].append(row)
        try:
            check = self.runner.candidate_check(manifest, f'{ident}-check')
            row['candidate_check'] = check
            self.runner.require_exit(check, 0, f'{ident}-check')
            require_ok_document(check, f'{ident}-check')

            build = self.runner.candidate_build(
                manifest, directory, f'{ident}-build')
            row['compile'] = legacy_projection(build, paired=True)
            row['candidate'] = {'check': check, 'build': build}
            self.runner.require_exit(build, 0, f'{ident}-build')
            require_ok_document(build, f'{ident}-build')
            require_entry_files(directory, f'{ident}-build')
            row['candidate']['entries'] = {
                name: sha(directory / name)
                for name in ('program.py', 'langrt.py')}

            api = self.runner.api_emit(manifest, api_dir, f'{ident}-api')
            row['api'] = {'emit': api}
            self.runner.require_exit(api, 0, f'{ident}-api')
            document = parse_process_json(api, f'{ident}-api')
            # CLI and API bytes must be identical before the API entry,
            # symbols or HIR text are trusted and published.
            compare_entries(directory, api_dir, f'{ident} CLI/API')
            metadata, hir = api_metadata(document, f'{ident}-api')
            write_evidence(directory / 'metadata.json',
                           json.dumps(metadata, ensure_ascii=False,
                                      indent=2) + '\n')
            write_evidence(directory / 'verified.hir', hir)
            row['api'].update({
                'entry_symbol': metadata['entry_symbol'],
                'symbols': metadata['symbols'],
                'metadata_sha256': sha(directory / 'metadata.json'),
                'verified_hir_sha256': sha(directory / 'verified.hir'),
            })
            row['candidate']['snapshot'] = output_tree(directory)
            row['generated_sha256'] = artifact_hashes(
                directory, f'{ident}-build')

            helper = self.runner.reference_emit(
                manifest, reference, f'{ident}-reference')
            row['reference'] = {'emit': helper}
            self.runner.require_exit(helper, 0, f'{ident}-reference')
            require_ok_document(helper, f'{ident}-reference')
            row['reference']['generated_sha256'] = artifact_hashes(
                reference, f'{ident}-reference')
            row['reference']['snapshot'] = output_tree(reference)

            result = self.python(
                [self.args.python, '-s', directory / 'program.py'],
                directory, f'{ident}-program')
            candidate_tree = output_tree(directory)
            if candidate_tree != row['candidate']['snapshot']:
                raise PairedFailure(
                    f'{ident}-program: generated output tree changed')
            row['candidate']['snapshot_after'] = candidate_tree
            reference_tree = output_tree(reference)
            if reference_tree != row['reference']['snapshot']:
                raise PairedFailure(
                    f'{ident}-program: reference output tree changed')
            row['reference']['snapshot_after'] = reference_tree
            row.update(execute=result, actual=classify(result))
            if expected == 'panic':
                passed = (row['actual'] == 'panic'
                          and result['stderr'] ==
                          PANIC_MARKER + CAUSES[ident] + '\n')
            else:
                passed = (row['actual'] in ('return', 'ordinary_nonzero')
                          and result['returncode'] == status
                          and not result['stdout']
                          and not result['stderr'])
            row['status'] = 'passed' if passed else 'failed'
            if not passed:
                self.report['failures'].append(
                    ident + ': unexpected generated-program behavior')
        except BaseException as error:
            row['status'] = 'failed'
            row['error'] = str(error)
            self.report['failures'].append(f'{ident}: {error}')
            raise PairedFailure(f'{ident}: {error}') from error
        return directory

    def _require_no_artifacts(self, directory, before, label):
        directory = Path(directory)
        for name in ('program.py', 'langrt.py'):
            if (directory / name).exists():
                raise PairedFailure(f'{label}: rejection published {name}')
        after = output_tree(directory)
        if before is None:
            if after:
                raise PairedFailure(
                    f'{label}: rejection created an output tree: {after}')
        elif after != before:
            raise PairedFailure(
                f'{label}: rejection changed the existing output directory')

    def _rejection(self, ident, manifest, expected_code, group, status,
                   directory, row_id, *, check_expected=True, span=True,
                   phase=None):
        directory = Path(directory)
        reference = self.args.out / 'reference' / directory.name
        before = output_tree(directory) if directory.exists() else None
        before_reference = (output_tree(reference)
                            if reference.exists() else None)
        row = {'id': row_id, 'compile': None, 'status': 'failed'}
        self.report[group].append(row)
        source = Path(manifest).parent / 'src/main.wz'
        try:
            if check_expected:
                check = self.runner.candidate_check(
                    manifest, f'{row_id}-check')
                row['candidate_check'] = check
                self.runner.require_exit(check, 1, f'{row_id}-check')
                require_diagnostic(
                    check, expected_code, source if span else None,
                    f'{row_id}-check', phase=phase)
            build = self.runner.candidate_build(
                manifest, directory, f'{row_id}-build')
            row['compile'] = legacy_projection(build, paired=True)
            row['candidate'] = {'build': build}
            if check_expected:
                row['candidate']['check'] = row['candidate_check']
            self.runner.require_exit(build, 1, f'{row_id}-build')
            require_diagnostic(
                build, expected_code, source if span else None,
                f'{row_id}-build', phase=phase)
            self._require_no_artifacts(directory, before, f'{row_id}-build')

            helper = self.runner.reference_emit(
                manifest, reference, f'{row_id}-reference')
            row['reference'] = helper
            self.runner.require_exit(helper, 1, f'{row_id}-reference')
            require_diagnostic(
                helper, expected_code, source if span else None,
                f'{row_id}-reference', phase=phase)
            self._require_no_artifacts(
                reference, before_reference, f'{row_id}-reference')
            row['status'] = status
        except BaseException as error:
            row['status'] = 'failed'
            row['error'] = str(error)
            self.report['failures'].append(f'{row_id}: {error}')
            raise PairedFailure(f'{row_id}: {error}') from error

    def compile_unsupported(self, ident, manifest):
        self._rejection(
            ident, manifest, 'E_CAPABILITY_UNAVAILABLE', 'unsupported',
            'unsupported', self.args.out / 'generated' / ('unsupported-' + ident),
            ident)

    def compile_source_rejection(self, ident, manifest, expected_code):
        self._rejection(
            ident, manifest, expected_code, 'source_rejections', 'rejected',
            self.args.out / 'generated' / ('rejected-' + ident), ident)

    def compile_profile32(self, manifest):
        self._rejection(
            'profile32', manifest, 'E_CAPABILITY_UNAVAILABLE', 'unsupported',
            'unsupported', self.args.out / 'generated' / 'profile32',
            'python32-profile', check_expected=False, span=False,
            phase='codegen')

    def verify_trees(self):
        for group in ('cases', 'extra', 'negative_controls'):
            for row in self.report.get(group, []):
                candidate = row.get('candidate')
                if not candidate or 'snapshot_after' not in candidate:
                    continue
                current = output_tree(self.args.out / 'generated' / row['id'])
                if current != candidate['snapshot_after']:
                    raise PairedFailure(
                        f"{row['id']}: candidate output tree changed after "
                        f'observations')
                reference = row.get('reference') or {}
                if 'snapshot_after' not in reference:
                    continue
                current = output_tree(self.args.out / 'reference' / row['id'])
                if current != reference['snapshot_after']:
                    raise PairedFailure(
                        f"{row['id']}: reference output tree changed after "
                        f'observations')


def run_suite(executor, published=None):
    args = executor.args
    report = executor.report
    cases_file = args.root / 'tests/backend_common/cases.json'
    if published is None:
        published = json.loads(cases_file.read_text())
    if published['case_count'] != 15 or len(published['cases']) != 15:
        raise RuntimeError('expected all 15 published backend cases')
    immutable = [cases_file]
    for case in published['cases']:
        manifest = args.root / case['manifest']
        immutable.extend([manifest, manifest.parent / 'src/main.wz'])
    for ident in ('core', 'numeric'):
        immutable.extend([args.root / f'tests/fixtures/{ident}/lang.toml',
                          args.root / f'tests/fixtures/{ident}/src/main.wz'])
    before = {str(path): sha(path) for path in immutable}
    report['fixture_sha256'] = before

    for case in published['cases']:
        executor.compile_case(case['id'], args.root / case['manifest'],
                              'cases', case['expected']['status'])
        report['cases'][-1]['published_expected'] = case['expected']
    for ident in ('core', 'numeric'):
        executor.compile_case(ident,
                              args.root / f'tests/fixtures/{ident}/lang.toml',
                              'cases')
    sources = dict(EXTRA_SOURCES, **{
        'string': UNSUPPORTED_SOURCES['string'],
        'all-integer-widths': integer_width_project(),
        'float-bits-and-transmission': float_project()})
    generated = {}
    for ident, body in sources.items():
        expected = 'panic' if ident in CAUSES else 'return'
        status = 101 if ident == 'ordinary-nonzero' else 7 if ident == 'ordinary-other-nonzero' else 0
        group = 'negative_controls' if status else 'extra'
        generated[ident] = executor.compile_case(
            ident, source_project(args.out, ident, body), group, expected,
            status)

    module_manifest = source_project(args.out, 'cross-module', '''
import "app/math";
func main() i32 {if math.twice(21)!=42 {return 1;}return 0;}
''')
    math_dir = module_manifest.parent / 'src/math'
    math_dir.mkdir(parents=True, exist_ok=True)
    (math_dir / 'value.wz').write_text(
        'package math; pub func twice(x i32) i32 {return x*2;}\n')
    (module_manifest.parent / 'src/other.wz').write_text(
        'package app; func unused() {}\n')
    executor.compile_case('cross-module', module_manifest, 'extra')

    # Actual generated module + runtime, with an external harness injecting a
    # host bug. This must remain a Python traceback and cannot count as Panic.
    normal = generated['ordinary-other-nonzero']
    if normal:
        metadata_document = executor.metadata(normal, 'host-injection')
        entry_name, _ = metadata_entry(metadata_document, 'host-injection')
        injected = executor.python(
            [args.python, '-s', '-c',
             'import program, langrt\n'
             'def host_bug(*args):\n'
             '    raise RuntimeError("controlled host bug")\n'
             f'langrt.binary=host_bug\nlangrt.run(program.{entry_name})'],
            normal, 'non-panic-python-exception')
        passed = (classify(injected) == 'python_exception' and
                  injected['returncode'] == 1 and
                  'RuntimeError: controlled host bug' in injected['stderr'] and
                  PANIC_MARKER not in injected['stderr'])
        report['negative_controls'].append(
            {'id': 'non-panic-python-exception', 'execute': injected,
             'actual': classify(injected),
             'status': 'passed' if passed else 'failed'})
        if not passed:
            report['failures'].append(
                'host Python exception misclassified')
        syntax = executor.python(
            [args.python, '-s', '-c', 'def broken('], normal,
            'python-syntax-error')
        passed = classify(syntax) != 'panic' and 'SyntaxError' in syntax['stderr']
        report['negative_controls'].append(
            {'id': 'python-syntax-error', 'execute': syntax,
             'actual': classify(syntax),
             'status': 'passed' if passed else 'failed'})
        if not passed:
            report['failures'].append('syntax error misclassified')
        controls = executor.python(
            [args.python, '-s', '-c',
             "import langrt; assert issubclass(langrt.Panic, BaseException); "
             "assert not issubclass(langrt.Panic, Exception)"], normal,
            'panic-outside-exception')
        report['negative_controls'].append(
            {'id': 'panic-outside-exception', 'execute': controls,
             'status': 'passed' if controls['returncode'] == 0 else 'failed'})
        if controls['returncode']:
            report['failures'].append('Panic Exception inheritance')

    inout = generated['inout-identity-and-copies']
    if inout:
        metadata_document = executor.metadata(inout, 'inout-mutation')
        mutate = named_generated(
            metadata_document, ['mutate_then_panic'],
            'inout-mutation')['mutate_then_panic']
        observation = executor.python(
            [args.python, '-s', '-c',
             'import program, langrt\nx=langrt.Cell(4)\n'
             f'try:\n    program.{mutate}(x)\n'
             'except langrt.Panic:\n    assert x.value==5\n'
             'else:\n    raise AssertionError("missing Panic")'],
            inout, 'inout-mutation-before-panic')
        report['extra'].append(
            {'id': 'inout-mutation-before-panic', 'execute': observation,
             'status': 'passed' if observation['returncode'] == 0 else 'failed'})
        if observation['returncode']:
            report['failures'].append(
                'inout copies back only on normal return')

    floats = generated['float-bits-and-transmission']
    if floats:
        metadata_document = executor.metadata(floats, 'float-observation')
        checks = dict(FLOAT_LITERALS, probe32=('f32', '', '3fc00000'),
                      probe64=('f64', '', '3ff8000000000000'))
        functions = named_generated(
            metadata_document, list(checks), 'float-observation')
        probes = ["import program, struct"]
        for ident, (typ, _, hex_bits) in checks.items():
            fmt = '>f' if typ == 'f32' else '>d'
            probes.append(
                f"assert struct.pack('{fmt}', program.{functions[ident]}()).hex() == '{hex_bits}', '{ident}'")
        observation = executor.python(
            [args.python, '-s', '-c', '\n'.join(probes)], floats,
            'float-exact-bits-observed')
        report['extra'].append(
            {'id': 'float-exact-bits-observed', 'execute': observation,
             'checks': {key: value[2] for key, value in checks.items()},
             'status': 'passed' if observation['returncode'] == 0 else 'failed'})
        if observation['returncode']:
            report['failures'].append('float bits/transmission')

    foreign = source_project(args.out, 'unsupported-foreign',
                             'import "py" as p;func main() {p.run();}')
    with foreign.open('a') as file:
        file.write('[dependencies.py]\nkind="python"\nbinding="binding.json"\nhost_package="host"\n')
    foreign.parent.joinpath('binding.json').write_text(json.dumps({
        'schema_version': 1, 'contract': 'compiler-dev/1',
        'review_status': 'draft_requires_conformance', 'allowed_backends': ['python'], 'conformance_required': [],
        'package_requirements': [{'package': 'host', 'version_policy': 'test fixture only'}],
        'modules': [{'logical_path': 'py', 'host_module': 'host', 'provider': 'python_package'}],
        'types': [], 'values': [], 'callables': [
            {'module': 'py', 'name': 'run', 'call_kind': 'function', 'id': 'host.run', 'owner_type': None, 'host_attribute': ['run'], 'parameters': [], 'return_type': 'Unit', 'error_type': None, 'ownership': {'return': 'unit', 'storage': 'no_tensor_storage'}, 'effects': [], 'receiver_escapes': 'not_applicable'}]}))
    unavailable = [(ident, source_project(args.out, 'unsupported-' + ident, body))
                   for ident, body in UNSUPPORTED_SOURCES.items()
                   if ident != 'string'] + [('foreign', foreign)]
    for ident, manifest in unavailable:
        executor.compile_unsupported(ident, manifest)

    for ident, (body, expected_code) in SOURCE_REJECTIONS.items():
        manifest = source_project(args.out, 'rejected-' + ident, body)
        executor.compile_source_rejection(ident, manifest, expected_code)

    # The checker supports Python/32, but this emitter's first profile is 64.
    width32 = source_project(args.out, 'profile32', 'func main() {}', bits=32)
    executor.compile_profile32(width32)

    if getattr(executor, 'paired', False):
        executor.verify_trees()

    if before != {str(path): sha(path) for path in immutable}:
        report['failures'].append('published fixture changed')


def finish_report(report, args):
    report['status'] = 'failed' if report['failures'] else 'passed'
    report['summary'] = {
        group: {status: sum(row['status'] == status for row in report.get(group, []))
                for status in ('passed', 'failed', 'unsupported', 'rejected')}
        for group in ('cases', 'extra', 'negative_controls', 'unsupported',
                      'source_rejections')}
    path = Path(args.out) / 'results.json'
    path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    for group in ('cases', 'extra', 'negative_controls', 'unsupported',
                  'source_rejections'):
        for row in report.get(group, []):
            print(f"{group}/{row['id']}: {row['status']}")
    print(json.dumps({'status': report['status'],
                      'summary': report['summary'],
                      'failures': report['failures'],
                      'report': str(path)}, ensure_ascii=False))
    return 1 if report['failures'] else 0


def build_parser():
    parser = argparse.ArgumentParser()
    for arg in ('helper', 'root', 'out', 'python'):
        parser.add_argument('--' + arg, type=Path, required=True)
    parser.add_argument('--candidate-cli', type=Path,
                        help='candidate public Wz CLI (paired mode)')
    parser.add_argument('--api-driver', type=Path,
                        help='prebuilt Wz pythonobs driver (paired mode)')
    parser.add_argument('--source-root', type=Path,
                        help='frozen Wz source package root (paired provenance)')
    parser.add_argument('--candidate-resources', type=Path,
                        help='candidate stdlib+runtime directory (paired mode)')
    parser.add_argument('--strace', type=Path,
                        help='fixed strace used by every paired command')
    parser.add_argument('--toolchain-root', type=Path,
                        help='optional LLVM/libxml2 root for paired processes')
    return parser


def resolve_tool(value):
    if value is None:
        return None
    value = Path(value)
    if not value.is_absolute():
        found = shutil.which(str(value))
        value = Path(found) if found else value
    return value.resolve()


def paired_requested(args):
    return any(getattr(args, name) is not None for name in PAIRED_ARGUMENTS)


def report_platform(paired):
    if not paired:
        return platform.platform()
    try:
        info = os.uname()
        return f'{sys.platform} {info.release} {info.machine}'
    except (AttributeError, OSError):
        return sys.platform


def report_header(args, version, paired, source_head):
    report = {
        'task': 'T06',
        'operation_id': 'language-t06-python-scalars-20261103',
        'contract': 'compiler-dev/1',
        'profile': 'python64',
        'checked_at': datetime.now(timezone.utc).isoformat(),
        'python': version,
        'platform': report_platform(paired),
        'source_head': source_head,
        'fixture_sha256': {},
        'cases': [], 'extra': [], 'negative_controls': [],
        'unsupported': [], 'source_rejections': [], 'failures': [],
        'not_run': ['official build CLI integration (integrator)',
                    'native backend comparison (T05)',
                    'ARC/leak checks (T09)',
                    'foreign NumPy/PyTorch (T10)'],
    }
    if paired:
        report['mode'] = 'paired'
        report['source_head_status'] = (
            'not_run: paired mode does not execute git')
    return report


def main():
    args = build_parser().parse_args()
    args.helper = args.helper.resolve()
    args.root = args.root.resolve()
    args.python = args.python.resolve()
    args.out = args.out.resolve()
    for key in ('candidate_cli', 'api_driver', 'source_root',
                'candidate_resources', 'strace', 'toolchain_root'):
        setattr(args, key, resolve_tool(getattr(args, key)))

    paired = paired_requested(args)
    if paired:
        missing = [name for name in ('candidate_cli', 'api_driver', 'strace')
                   if getattr(args, name) is None]
        if missing:
            raise RuntimeError(
                'paired mode requires ' + ', '.join('--' + name.replace('_', '-')
                                                    for name in missing))
        if args.source_root is None:
            args.source_root = args.root / 'selfhost/src'
        if args.candidate_resources is None:
            args.candidate_resources = args.candidate_cli.parent
        args.source_root = args.source_root.resolve()
        args.candidate_resources = args.candidate_resources.resolve()
        prepare_output(args.out, strict=True)
    else:
        args.out.mkdir(parents=True, exist_ok=True)
        resources = None

    if paired:
        report = report_header(args, None, True, None)
        report.update({
            'inputs_before': {},
            'paired': {
                'resources': str(args.candidate_resources),
                'cli_resources': None,
                'source_root': str(args.source_root),
                'candidate_cli': str(args.candidate_cli),
                'api_driver': str(args.api_driver),
                'strace': str(args.strace),
                'timeout_seconds': PAIRED_TIMEOUT_SECONDS,
                'stdio': {
                    'stdin': None,
                    'policy': 'no stdin file; bounded transport uses the caller stdin',
                },
            },
        })
        runner = None
        resources = None
        resource_roots = None
        resource_before = None
        inputs_snapshot_taken = False
        try:
            cases_file = args.root / 'tests/backend_common/cases.json'
            published = json.loads(cases_file.read_text())
            if published['case_count'] != 15 or len(published['cases']) != 15:
                raise RuntimeError('expected all 15 published backend cases')
            for name in ('helper', 'candidate_cli', 'api_driver', 'python',
                         'strace'):
                if not Path(getattr(args, name)).is_file():
                    raise RuntimeError(
                        f'paired mode missing --{name.replace("_", "-")}: '
                        f'{getattr(args, name)}')
            if not args.source_root.is_dir():
                raise RuntimeError(
                    f'--source-root is not a directory: {args.source_root}')

            resource_info = verify_paired_resources(args)
            resources = check_resources(args.candidate_resources)
            resource_roots = [Path(resource_info['cli_root']),
                              Path(resources)]
            resource_before = resource_snapshot(resource_roots, 'before')
            inputs_before = snapshot_inputs(
                args, resources, published, cases_file, resource_roots)
            inputs_snapshot_taken = True
            report['inputs_before'] = inputs_before
            report['paired'].update({
                'resources': str(resources),
                'cli_resources': resource_info['cli_root'],
                'resource_targets': resource_info['targets'],
                'resources_before': resource_before,
            })

            runner = PairedRunner(args, resources)
            driver_info = runner.prepare_driver()
            report['driver'] = driver_info
            version_process = runner.command(
                [args.python, '--version'], args.out, 'python-version',
                {args.python})
            runner.require_exit(version_process, 0, 'python-version')
            if version_process['stderr']:
                raise RuntimeError(
                    f"python-version: unexpected stderr: "
                    f"{version_process['stderr']!r}")
            if not version_process['stdout'].startswith('Python 3.12.'):
                raise RuntimeError(
                    f'Python 3.12 required: {version_process["stdout"]}')
            version = legacy_projection(version_process, paired=True)
            report['python'] = version
            executor = PairedExecutor(args, report, runner)
            run_suite(executor, published)
        except BaseException as error:
            report['error'] = str(error)
            report['traceback'] = traceback.format_exc()
            report['failures'].append(f'paired run aborted: {error}')

        try:
            if resource_roots is not None:
                inputs_after = snapshot_inputs(
                    args, resources, published, cases_file, resource_roots)
                report['inputs_after'] = inputs_after
                if (inputs_snapshot_taken
                        and inputs_after != report.get('inputs_before')):
                    report['failures'].append(
                        'source/resource/script inputs changed during the run')
                resource_after = resource_snapshot(resource_roots, 'after')
                report['paired']['resources_after'] = resource_after
                if (resource_before is not None
                        and resource_after != resource_before):
                    report['failures'].append(
                        'CLI/API resource inputs changed during the run')
            else:
                report['inputs_after'] = {
                    'status': 'not_run: resources were not verified before '
                              'any command'}
        except BaseException as error:
            report['inputs_after'] = {'error': str(error)}
            report['failures'].append(
                f'input/resource snapshot failed: {error}')
        driver = report.get('driver') or {}
        if driver.get('path'):
            try:
                driver['final_sha256'] = sha(Path(driver['path']))
                if driver['final_sha256'] != driver['sha256']:
                    report['failures'].append(
                        'staged API driver changed during the run')
            except OSError as error:
                report['failures'].append(
                    f'staged API driver disappeared: {error}')
        report['paired']['command_count'] = (
            len(runner.commands) if runner is not None else 0)
        return finish_report(report, args)

    version = command([args.python, '--version'])
    if (version['returncode']
            or not version['stdout'].startswith('Python 3.12.')):
        raise RuntimeError(f'Python 3.12 required: {version}')
    report = report_header(
        args, version, paired,
        command(['git', '-C', args.root, 'rev-parse', 'HEAD'])['stdout'].strip())
    executor = LegacyExecutor(args, report)
    run_suite(executor)
    return finish_report(report, args)


if __name__ == '__main__':
    sys.exit(main())
