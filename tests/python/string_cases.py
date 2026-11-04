"""Independent expectations and CLI rejection checks for literal-backed strings."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


EXPECTED = {
    'probe_empty': b'',
    'probe_ascii': b'hello',
    'probe_chinese': '你好，世界'.encode('utf-8'),
    'probe_escapes': bytes.fromhex('5c220a0d090041'),
    'probe_unicode': bytes.fromhex('007fc280e0a080efbfbff0908080f48fbfbf'),
    'probe_raw': '第一行\n第二行\\n\\0'.encode('utf-8'),
    'probe_copy': '原值'.encode('utf-8'),
    'probe_rebind': '新值'.encode('utf-8'),
    'probe_local': '局部\0之后'.encode('utf-8'),
    'probe_shadow': '外层'.encode('utf-8'),
    'probe_parameter': '按值'.encode('utf-8'),
    'probe_generic': '泛型'.encode('utf-8'),
    'probe_inferred': '推断'.encode('utf-8'),
    'probe_function_value': '函数值'.encode('utf-8'),
    'probe_lambda': '字面函数'.encode('utf-8'),
    'probe_module': '跨模块'.encode('utf-8'),
    'probe_package': '跨包'.encode('utf-8'),
    'probe_recursion': '递归\0尾部'.encode('utf-8'),
    'probe_order': '第二'.encode('utf-8'),
    'probe_named_order': '第三'.encode('utf-8'),
}
CAP = 'E_CAPABILITY_UNAVAILABLE'
TYPE = 'E_TYPE_MISMATCH'
REJECTED = {
    'struct-string': ('struct S {s string;} func main() {let x=S(s:"x");}', CAP),
    'enum-string': ('enum E {A(s string);} func main() {let x=E.A(s:"x");}', CAP),
    'generic-string-field': ('struct S[T] {x T;} func main() {let x=S[string](x:"x");}', CAP),
    'vec': ('func main() {let x Vec[string]=["x"];}', CAP),
    'concat': ('func main() {let a="x";let b="y";let x=a+b;}', TYPE),
    'index': ('func main() {let x="x";let y=x[0];}', CAP),
    'inout': ('func f(x inout string) {} func main() {}', CAP),
    'move': ('func f(x move string) {} func main() {}', CAP),
    'foreign': ('import "py" as p;func main() {p.run();}', CAP),
    'parameter-reassign': ('func f(x string) {x="y";} func main() {}', CAP),
    'let-reassign': ('func main() {let x="x";x="y";}', TYPE),
    'string-as-integer': ('func main() {let x i32="x";}', TYPE),
    'integer-as-string': ('func f() string {return 1;} func main() {}', TYPE),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, **kwargs):
    result = subprocess.run(list(map(str, argv)), capture_output=True, timeout=60, **kwargs)
    row = dict(argv=list(map(str, argv)), returncode=result.returncode,
               stdout_hex=result.stdout.hex(), stderr=result.stderr.decode('utf-8'))
    return row


def succeeded(row):
    assert row['returncode'] == 0 and not row['stderr'], row


def rejection_checks(cli, backend, out):
    rows = []
    for ident, (source, code) in REJECTED.items():
        project = out / 'rejected' / ident
        (project / 'src').mkdir(parents=True, exist_ok=True)
        manifest = project / 'lang.toml'
        manifest.write_text('schema_version=1\n[package]\nname="app"\n'
                            'source_root="src"\nentry="app"\n[build]\n'
                            'backend="' + backend + '"\nword_bits=64\n')
        path = project / 'src/main.lang'
        path.write_text('package app;' + source, encoding='utf-8')
        if ident == 'foreign':
            with manifest.open('a') as file:
                file.write('[dependencies.py]\nkind="python"\nbinding="binding.json"\nhost_package="host"\n')
            (project / 'binding.json').write_text(json.dumps({
                'schema_version': 1, 'contract': 'compiler-dev/1',
                'package_requirements': [{'package': 'host'}],
                'modules': [{'logical_path': 'py', 'host_module': 'host'}],
                'types': [], 'values': [], 'callables': [
                    {'module': 'py', 'name': 'run', 'call_kind': 'function', 'id': 'host.run'}]}))
        output = Path(tempfile.mkdtemp(prefix='build-', dir=project)) / 'output'
        row = dict(id=ident, expected=code, source_sha256=sha(path), status='failed')
        rows.append(row)
        for phase in ('check', 'build'):
            argv = [cli, phase, '--project', manifest, '--backend', backend, '--format', 'json']
            if phase == 'build':
                argv += ['--out-dir', output]
            row[phase] = result = command(argv)
            parsed = json.loads(bytes.fromhex(result['stdout_hex']))
            assert result['returncode'] == 1 and not result['stderr'], result
            assert parsed['status'] == 'error' and any(
                d['code'] == code and d.get('primary') and
                Path(d['primary']['path']).resolve() == path.resolve() and
                0 <= d['primary']['start_byte'] < d['primary']['end_byte'] <= path.stat().st_size
                for d in parsed['diagnostics']), parsed
            assert not output.exists(), 'failed build created output'
        output.mkdir()
        for name in ('program', 'program.o', 'program.py', 'langrt.py', 'unrelated'):
            (output / name).write_text('preserve-' + name)
        before = {p.name: sha(p) for p in output.iterdir()}
        row['existing'] = result = command([cli, 'build', '--project', manifest,
            '--backend', backend, '--format', 'json', '--out-dir', output])
        assert result['returncode'] == 1 and not result['stderr'], result
        assert before == {p.name: sha(p) for p in output.iterdir()}
        row['status'] = 'unsupported' if code == CAP else 'passed'
    return rows
