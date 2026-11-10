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
CONST_EXPECTED = {
    'probe_const_top': '顶层\0常量'.encode('utf-8'),
    'probe_const_alias': '顶层\0常量'.encode('utf-8'),
    'probe_const_raw': '常量原始\n第二行\\n\\0'.encode('utf-8'),
    'probe_const_unicode': bytes.fromhex('00f48fbfbff09f9880'),
    'probe_const_empty': b'',
    'probe_const_local': '局部\0常量'.encode('utf-8'),
    'probe_const_local_inferred': '推断常量'.encode('utf-8'),
    'probe_const_alias_local': '别名\0链'.encode('utf-8'),
    'probe_const_shadow': '外层常量'.encode('utf-8'),
    'probe_const_module': '跨模块常量\0尾'.encode('utf-8'),
    'probe_const_package': '跨包常量\0尾'.encode('utf-8'),
}

AGGREGATE_EXPECTED = {
    'probe_box': '泛型\0盒😀'.encode('utf-8'),
    'probe_nested': '嵌套\0😀'.encode('utf-8'),
    'probe_enum': '原始\n多行\\0😀'.encode('utf-8'),
    'probe_empty': b'',
    'probe_parameter': '普通\0参数'.encode('utf-8'),
    'probe_branch': '分支\n假'.encode('utf-8'),
    'probe_move': '整体\0移动😀'.encode('utf-8'),
    'probe_move_box': '盒\0移动'.encode('utf-8'),
    'probe_reinit': '新\0值'.encode('utf-8'),
}
VEC_EXPECTED = {
    'probe_alias_replace': '替\0换'.encode('utf-8'),
    'probe_slice': '末\0😀'.encode('utf-8'),
    'probe_for_in': '末\0😀'.encode('utf-8'),
    'probe_element_copy': '空'.encode('utf-8'),
    'probe_release': '末\0😀'.encode('utf-8'),
    'probe_move_string': '本地\0移动'.encode('utf-8'),
    'probe_move_vec': '向\0量移动'.encode('utf-8'),
    'probe_bundle': '项\0目'.encode('utf-8'),
}

STRING_FIXTURES = {
    "aggregate-constants": {
        'probe_control': b'control',
        'probe_payload': '聚合\0😀'.encode('utf-8'),
        'probe_alias_field': '跨包\0😀'.encode('utf-8'),
        'probe_local_shadow': '外层\0值'.encode('utf-8'),
        'probe_local_field_const': '局部\0字段'.encode('utf-8'),
        'probe_error': '错误\0载荷'.encode('utf-8'),
    },
    "strings": EXPECTED,
    "string-constants": CONST_EXPECTED,
    "string-aggregates": AGGREGATE_EXPECTED,
    "string-vec-aggregates": VEC_EXPECTED,
}
STRING_FIXTURE_PATHS = {
    "aggregate-constants": "tests/sema/aggregate-constants",
    "strings": "tests/sema/strings",
    "string-constants": "tests/sema/string-constants",
    "string-aggregates": "tests/sema/string-aggregates",
    "string-vec-aggregates": "tests/native/ownership/string-vec-aggregates",
}

CAP = 'E_CAPABILITY_UNAVAILABLE'
TYPE = 'E_TYPE_MISMATCH'
REJECTED = {
    'struct-string-inout': ('struct S {s string;} func f(x inout S) {} func main() {}', CAP),
    'vec-string-inout': ('func f(x inout Vec[string]) {} func main() {}', CAP),
    'concat': ('func main() {let a="x";let b="y";let x=a+b;}', TYPE),
    'index': ('func main() {let x="x";let y=x[0];}', CAP),
    'slice': ('func main() {let x="x";let y=x[:];}', CAP),
    'inout': ('func f(x inout string) {} func main() {}', CAP),
    'move-after-read': ('func main() {let x="x";let y=move x;let z=x;}',
                        'E_OWNERSHIP_USE_AFTER_MOVE'),
    'ordinary-parameter-move': ('func take(x move string) string {return move x;} '
                                'func f(p string) {let y=take(move p);} func main() {}',
                                'E_CALL_ARGUMENT'),
    'partial-field-move': ('struct S {s string;n i32;} func main() '
                           '{var x=S(s:"x",n:1);let y=move x.s;}', CAP),
    'partial-index-move': ('func main() {let x Vec[string]=["x"];let y=move x[0];}', CAP),
    'foreign': ('import "py" as p;func main() {p.run();}', CAP),
    'parameter-reassign': ('func f(x string) {x="y";} func main() {}', TYPE),
    'let-reassign': ('func main() {let x="x";x="y";}', TYPE),
    'string-as-integer': ('func main() {let x i32="x";}', TYPE),
    'integer-as-string': ('func f() string {return 1;} func main() {}', TYPE),
    'const-runtime': ('func main() {let x="x";const C=x;}', CAP),
    'const-call': ('func s() string {return "x";} const C=s(); func main() {}', CAP),
    'const-concat': ('const C="x"+"y"; func main() {}', TYPE),
    'const-type': ('const C i32="x"; func main() {}', TYPE),
    'const-cycle': ('const A=B;const B=A;func main() {}', 'E_CONSTANT_CYCLE'),
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
        path = project / 'src/main.wz'
        path.write_text('package app;' + source, encoding='utf-8')
        if ident == 'foreign':
            with manifest.open('a') as file:
                file.write('[dependencies.py]\nkind="python"\nbinding="binding.json"\nhost_package="host"\n')
            (project / 'binding.json').write_text(json.dumps({
                'schema_version': 1, 'contract': 'compiler-dev/1',
                'review_status': 'draft_requires_conformance', 'allowed_backends': ['python'], 'conformance_required': [],
                'package_requirements': [{'package': 'host', 'version_policy': 'test fixture only'}],
                'modules': [{'logical_path': 'py', 'host_module': 'host', 'provider': 'python_package'}],
                'types': [], 'values': [], 'callables': [
                    {'module': 'py', 'name': 'run', 'call_kind': 'function', 'id': 'host.run', 'owner_type': None, 'host_attribute': ['run'], 'parameters': [], 'return_type': 'Unit', 'error_type': None, 'ownership': {'return': 'unit', 'storage': 'no_tensor_storage'}, 'effects': [], 'receiver_escapes': 'not_applicable'}]}))
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
