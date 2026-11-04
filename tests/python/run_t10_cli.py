#!/usr/bin/env python3
"""Verify the public langc CLI standard-binding path.

The official `langc check/build --project` commands are exercised with the
existing `ProjectOptions.std_binding_bundle` plumbing.  The API helper is used
only to locate the generated `train` symbol and to prove byte identity; the
executed programs are the files produced by the CLI itself.
"""
import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
import traceback
from pathlib import Path

sys.dont_write_bytecode = True
THREAD_ENV = ('OMP_NUM_THREADS', 'MKL_NUM_THREADS', 'OPENBLAS_NUM_THREADS',
              'NUMEXPR_NUM_THREADS', 'VECLIB_MAXIMUM_THREADS')
ORIGINAL04_SHA = 'beb672124e6ccc697c8c129d2f7aa3bf21640de0b4af7a058eefa907fc6e6797'
BINDING_SHA = '44924c3c33c5acff0e604f126fa1dcb346472fa0a7966de14dd0a5b4a10adb5f'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def toml_path(path):
    return json.dumps(str(path), ensure_ascii=False)


def files(path):
    path = Path(path)
    if not path.exists():
        return []
    return sorted(str(p.relative_to(path)) for p in path.rglob('*') if p.is_file())


class Runner:
    def __init__(self, args):
        self.args = args
        self.rows = []
        self.commands = []
        self.env = dict(os.environ)
        for name in THREAD_ENV:
            self.env[name] = '1'
        self.env['PYTHONDONTWRITEBYTECODE'] = '1'
        self.env['PYTHONNOUSERSITE'] = '1'
        self.env.pop('PYTHONPATH', None)
        self.env.pop('PYTHONOPTIMIZE', None)

    def command(self, row, argv, cwd, timeout=300):
        argv = [str(value) for value in argv]
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

    def fresh(self, path):
        path = Path(path)
        if path.exists():
            shutil.rmtree(path)
        return path

    def add(self, name, fn):
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
            row['traceback'] = traceback.format_exc(limit=20)
        return row


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def diagnostics(data):
    return [item.get('code') for item in data.get('diagnostics', [])]


def assert_json_ok(record):
    require(record['exit'] == 0, record)
    require(record['stderr'] == '', record)
    data = json.loads(record['stdout'])
    require(data.get('status') == 'ok', data)
    return data


def assert_json_error(record, exit_code, code):
    require(record['exit'] == exit_code, record)
    require(record['stderr'] == '', record)
    data = json.loads(record['stdout'])
    require(data.get('status') == 'error', data)
    require(data.get('status') != 'unsupported', data)
    require(code in diagnostics(data), data)
    return data


def assert_driver_argument(record):
    require(record['exit'] == 2, record)
    require('E_DRIVER_ARGUMENT' in record['stderr'], record)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--langc', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--python', type=Path, required=True)
    parser.add_argument('--binding', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--original-observer', type=Path, required=True)
    parser.add_argument('--reference-observer', type=Path, required=True)
    parser.add_argument('--native-helper', type=Path)
    args = parser.parse_args()
    # Preserve the dedicated venv path instead of resolving the symlink to the
    # base interpreter without NumPy/PyTorch.
    args.python = Path(os.path.abspath(str(args.python)))
    for key in ('langc', 'helper', 'root', 'out', 'binding', 'reference',
                'original_observer', 'reference_observer'):
        setattr(args, key, getattr(args, key).resolve())
    if args.native_helper:
        args.native_helper = args.native_helper.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    require(sha(args.binding) == BINDING_SHA, sha(args.binding))
    require(sha(args.root / 'baseline/samples/04_numpy_torch.lang') == ORIGINAL04_SHA,
            'original 04 changed')
    r = Runner(args)

    binding = str(args.binding)
    original = args.out / 'projects' / 'original-04'
    r.fresh(original)
    write(original / 'src/main.lang',
          (args.root / 'baseline/samples/04_numpy_torch.lang').read_text())
    write(original / 'lang.toml', f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
[dependencies.numpy]
kind = "python"
binding = {toml_path(args.binding)}
host_package = "numpy"
[dependencies.torch]
kind = "python"
binding = {toml_path(args.binding)}
host_package = "torch"
''')

    def ordinary_project(name, source, binding=None, extra_manifest=''):
        root = r.fresh(args.out / 'projects' / name)
        write(root / 'src/main.lang', source)
        dependency = ''
        if binding is not None:
            dependency = f'''[dependencies.numpy]
kind = "python"
binding = {toml_path(binding)}
host_package = "numpy"
'''
        write(root / 'lang.toml', f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
{dependency}{extra_manifest}''')
        return root

    def build(row, project, out, std_binding=None, backend='python', json=True,
              timeout=300):
        argv = [args.langc, 'build', '--project', project / 'lang.toml',
                '--backend', backend, '--out-dir', out]
        if std_binding is not None:
            argv.extend(['--std-binding', std_binding])
        if json:
            argv.extend(['--format', 'json'])
        return r.command(row, argv, project, timeout=timeout)

    def check(row, project, std_binding=None, backend='python', json=True):
        argv = [args.langc, 'check', '--project', project / 'lang.toml']
        if backend:
            argv.extend(['--backend', backend])
        if std_binding is not None:
            argv.extend(['--std-binding', std_binding])
        if json:
            argv.extend(['--format', 'json'])
        return r.command(row, argv, project, timeout=300)

    def argument_errors(row):
        manifest = original / 'lang.toml'
        for argv in (
            [args.langc, 'check', '--project', manifest, '--std-binding'],
            [args.langc, 'check', '--project', manifest, '--std-binding', ''],
            [args.langc, 'check', '--project', manifest, '--std-binding', binding,
             '--std-binding', binding],
            [args.langc, 'check', '--project', manifest, '--std-binding=' + binding],
            [args.langc, 'check', '--syntax-only',
             args.root / 'baseline/samples/04_numpy_torch.lang',
             '--std-binding', binding],
            [args.langc, 'build', '--project', manifest, '--out-dir',
             args.out / 'argument-errors', '--std-binding'],
        ):
            assert_driver_argument(r.command(row, argv, original))
        help_record = r.command(row, [args.langc, '--help'], original)
        require(help_record['exit'] == 0, help_record)
        require(help_record['stdout'].count('--std-binding') >= 2, help_record)

    r.add('argument-errors', argument_errors)

    def original04_case(row):
        no_binding_out = args.out / 'runs' / 'original-04-no-binding'
        r.fresh(no_binding_out)
        missing_std = check(row, original)
        assert_json_error(missing_std, 1, 'E_IMPORT_NOT_FOUND')
        missing_build = build(row, original, no_binding_out)
        assert_json_error(missing_build, 1, 'E_IMPORT_NOT_FOUND')
        require(files(no_binding_out) == [], files(no_binding_out))

        checked = check(row, original, binding)
        assert_json_ok(checked)
        cli_out = args.out / 'runs' / 'original-04-cli'
        r.fresh(cli_out)
        built = build(row, original, cli_out, binding, timeout=600)
        assert_json_ok(built)
        require(files(cli_out) == ['langrt.py', 'program.py'], files(cli_out))

        api_out = args.out / 'runs' / 'original-04-api'
        r.fresh(api_out)
        api = r.command(
            row,
            [args.helper, '--project', original / 'lang.toml',
             '--out-dir', api_out, '--std-binding', binding],
            original)
        require(api['exit'] == 0, api)
        metadata = json.loads((api_out / 'metadata.json').read_text())
        names = {item['source_name']: item['generated']
                 for item in metadata['symbols']}
        require('train' in names, names)
        require(names['train'].startswith('_f'), names)
        require(f"def {names['train']}(" in (cli_out / 'program.py').read_text(),
                names)
        api_code = (api_out / 'program.py').read_bytes()
        api_runtime = (api_out / 'langrt.py').read_bytes()
        cli_code = (cli_out / 'program.py').read_bytes()
        cli_runtime = (cli_out / 'langrt.py').read_bytes()
        require(cli_code == api_code, 'CLI program.py differs from API output')
        require(cli_runtime == api_runtime, 'CLI langrt.py differs from API output')
        row['api_metadata_train'] = names['train']
        row['generated'] = {
            'cli_program.py': sha(cli_out / 'program.py'),
            'cli_langrt.py': sha(cli_out / 'langrt.py'),
            'api_program.py': sha(api_out / 'program.py'),
            'api_langrt.py': sha(api_out / 'langrt.py'),
            'byte_identical': True,
        }

        # The observer discovers the generated name from API metadata only.
        # The two generated files in this scratch directory are the CLI bytes;
        # API program.py/langrt.py are never copied into a CLI output.
        observed = r.fresh(args.out / 'runs' / 'original-04-observe-cli')
        observed.mkdir(parents=True)
        shutil.copyfile(cli_out / 'program.py', observed / 'program.py')
        shutil.copyfile(cli_out / 'langrt.py', observed / 'langrt.py')
        (observed / 'metadata.json').write_text(json.dumps(
            {'entry_symbol': metadata['entry_symbol'],
             'symbols': [item for item in metadata['symbols']
                         if item['source_name'] == 'train']},
            ensure_ascii=False) + '\n')
        require(sha(observed / 'program.py') == sha(cli_out / 'program.py'),
                'observer program is not the CLI program')
        generated = r.command(
            row, [args.python, args.original_observer, observed], observed,
            timeout=600)
        require(generated['exit'] == 0, generated)
        generated_row = json.loads(generated['stdout'].splitlines()[-1])
        require(generated_row['status'] == 'passed', generated_row)
        require(all(generated_row['acceptance'].values()), generated_row)
        require(generated_row['counts'] == {'forward': 201, 'mse': 201,
                                            'backward': 200, 'zero_grad': 200,
                                            'step': 200}, generated_row)

        reference = r.command(
            row, [args.python, args.reference_observer, args.reference],
            args.out, timeout=600)
        require(reference['exit'] == 0, reference)
        reference_row = json.loads(reference['stdout'].splitlines()[-1])
        require(reference_row['status'] == 'passed', reference_row)
        require(all(reference_row['acceptance'].values()), reference_row)
        require(reference_row['sha256'] == sha(args.reference), reference_row)
        generated_loss = generated_row['result']['value']
        reference_loss = reference_row['result']['value']
        require(math.isclose(generated_loss, reference_loss, rel_tol=1e-5,
                             abs_tol=1e-6),
                (generated_loss, reference_loss))

        standalone = r.command(
            row, [args.python, '-s', cli_out / 'program.py'], cli_out,
            timeout=600)
        require(standalone['exit'] == 0, standalone)
        require(standalone['stdout'] == '' and standalone['stderr'] == '',
                standalone)
        require(files(cli_out) == ['langrt.py', 'program.py'], files(cli_out))
        row['observation'] = generated_row
        row['reference'] = reference_row
        row['comparison'] = {
            'generated_loss': generated_loss,
            'reference_loss': reference_loss,
            'rtol': 1e-5,
            'atol': 1e-6,
            'passed': True,
        }
        row['standalone'] = standalone

    r.add('original-04-cli-standard-binding', original04_case)

    def spaced_path_case(row):
        project = r.fresh(args.out / 'projects' / '项目 space 04')
        write(project / 'src/main.lang',
              'package app;\nimport "std/py" as py;\nfunc main() {}\n')
        spaces = r.fresh(args.out / 'bindings 目录')
        spaces.mkdir(parents=True)
        spaced_binding = spaces / '标准 binding.json'
        shutil.copyfile(args.binding, spaced_binding)
        write(project / 'lang.toml', f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
''')
        checked = check(row, project, spaced_binding)
        assert_json_ok(checked)
        relative = os.path.relpath(spaced_binding, project)
        require(' ' in relative and '标准' in relative, relative)
        relative_check = check(row, project, relative)
        assert_json_ok(relative_check)
        cli_out = args.out / '构建 输出'
        r.fresh(cli_out)
        built = build(row, project, cli_out, spaced_binding)
        assert_json_ok(built)
        require(files(cli_out) == ['langrt.py', 'program.py'], files(cli_out))
        api_out = r.fresh(args.out / '构建 API 输出')
        api = r.command(
            row,
            [args.helper, '--project', project / 'lang.toml',
             '--out-dir', api_out, '--std-binding', spaced_binding],
            project)
        require(api['exit'] == 0, api)
        require((cli_out / 'program.py').read_bytes() ==
                (api_out / 'program.py').read_bytes(), 'spaced CLI code differs')
        require((cli_out / 'langrt.py').read_bytes() ==
                (api_out / 'langrt.py').read_bytes(), 'spaced CLI runtime differs')
        ran = r.command(row, [args.python, '-s', cli_out / 'program.py'],
                        cli_out)
        require(ran['exit'] == 0, ran)
        require(ran['stdout'] == '' and ran['stderr'] == '', ran)
        row['paths'] = {
            'project': str(project),
            'binding': str(spaced_binding),
            'relative_binding': relative,
            'output': str(cli_out),
        }

    r.add('spaced-and-chinese-paths', spaced_path_case)

    def std_shadow_case(row):
        project = r.fresh(args.out / 'projects' / 'std-shadow')
        write(project / 'src/main.lang', 'package app;\nimport "std/py";\nfunc main() {}\n')
        write(project / 'lang.toml', f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
[dependencies.std]
kind = "python"
binding = {toml_path(args.binding)}
host_package = "numpy"
''')
        out = args.out / 'runs' / 'std-shadow'
        r.fresh(out)
        checked = check(row, project, binding)
        assert_json_error(checked, 2, 'E_MANIFEST_INVALID')
        built = build(row, project, out, binding)
        assert_json_error(built, 2, 'E_MANIFEST_INVALID')
        require(files(out) == [], files(out))

    r.add('std-dependency-shadow-rejected', std_shadow_case)

    def failure_preserves_case(row):
        good = 'package app;\nimport "std/py";\nfunc main() i32 { return 0; }\n'
        project = ordinary_project('preserve-source', good, args.binding)
        new_out = args.out / 'runs' / 'preserve-source-new'
        old_out = args.out / 'runs' / 'preserve-source-old'
        r.fresh(new_out)
        r.fresh(old_out)
        old_out.mkdir(parents=True)
        (old_out / 'program.py').write_bytes(b'old-program')
        (old_out / 'langrt.py').write_bytes(b'old-runtime')
        (old_out / 'unrelated').write_bytes(b'keep')
        before = {name: (old_out / name).read_bytes()
                  for name in ('program.py', 'langrt.py', 'unrelated')}
        write(project / 'src/main.lang',
              'package app;\nimport "std/py";\nfunc main() i32 { return true; }\n')
        failed_new = build(row, project, new_out, args.binding)
        assert_json_error(failed_new, 1, 'E_TYPE_MISMATCH')
        require(files(new_out) == [], files(new_out))
        failed_old = build(row, project, old_out, args.binding)
        assert_json_error(failed_old, 1, 'E_TYPE_MISMATCH')
        after = {name: (old_out / name).read_bytes()
                 for name in ('program.py', 'langrt.py', 'unrelated')}
        require(after == before, 'source failure changed old outputs')

        write(project / 'src/main.lang', good)
        broken = args.out / 'bad-binding.json'
        broken.write_text('{"schema_version": 1, }')
        failed_new = build(row, project, new_out, broken)
        assert_json_error(failed_new, 1, 'E_BINDING_INVALID')
        require(files(new_out) == [], files(new_out))
        failed_old = build(row, project, old_out, broken)
        assert_json_error(failed_old, 1, 'E_BINDING_INVALID')
        after = {name: (old_out / name).read_bytes()
                 for name in ('program.py', 'langrt.py', 'unrelated')}
        require(after == before, 'binding failure changed old outputs')

        missing = args.out / 'missing-binding.json'
        missing.unlink(missing_ok=True)
        failed_missing = build(row, project, new_out, missing)
        assert_json_error(failed_missing, 2, 'E_INPUT_IO')
        require(files(new_out) == [], files(new_out))
        row['preserved_sha256'] = {
            name: hashlib.sha256(value).hexdigest()
            for name, value in before.items()
        }

    r.add('failures-preserve-existing-output', failure_preserves_case)

    def ordinary_project_case(row):
        project = ordinary_project(
            'ordinary-no-binding',
            'package app;\nfunc main() i32 { return 7; }\n')
        checked = check(row, project)
        assert_json_ok(checked)
        out = args.out / 'runs' / 'ordinary-no-binding'
        r.fresh(out)
        built = build(row, project, out)
        assert_json_ok(built)
        ran = r.command(row, [args.python, '-s', out / 'program.py'], out)
        require(ran['exit'] == 7 and ran['stdout'] == '' and ran['stderr'] == '',
                ran)

        bad = ordinary_project(
            'ordinary-negative',
            'package app;\nfunc main() i32 { return true; }\n')
        bad_out = args.out / 'runs' / 'ordinary-negative'
        r.fresh(bad_out)
        checked_bad = check(row, bad)
        assert_json_error(checked_bad, 1, 'E_TYPE_MISMATCH')
        built_bad = build(row, bad, bad_out)
        assert_json_error(built_bad, 1, 'E_TYPE_MISMATCH')
        require(files(bad_out) == [], files(bad_out))
        row['ordinary_return'] = ran['exit']

    r.add('ordinary-project-unchanged', ordinary_project_case)

    def native_case(row):
        (args.out / 'runs').mkdir(parents=True, exist_ok=True)
        out = args.out / 'runs' / 'native-original-04'
        r.fresh(out)
        failures = []
        failed = build(row, original, out, binding, backend='native')
        assert_json_error(failed, 1, 'E_CAPABILITY_UNAVAILABLE')
        require(files(out) == [], files(out))
        failures.append(failed)
        # Without the flag the driver cannot even load std/py; the different
        # code shows that the flag is not silently ignored.
        failed_no_flag = build(row, original, out, None, backend='native')
        assert_json_error(failed_no_flag, 1, 'E_IMPORT_NOT_FOUND')
        require(files(out) == [], files(out))
        failures.append(failed_no_flag)
        if args.native_helper:
            helper_out = args.out / 'runs' / 'native-helper-original-04'
            r.fresh(helper_out)
            direct = r.command(
                row,
                [args.native_helper, '--project', original / 'lang.toml',
                 '--out-dir', helper_out, '--std-binding', binding,
                 '--format', 'json'],
                original)
            assert_json_error(direct, 1, 'E_CAPABILITY_UNAVAILABLE')
            require(files(helper_out) == [], files(helper_out))
            direct_no_flag = r.command(
                row,
                [args.native_helper, '--project', original / 'lang.toml',
                 '--out-dir', helper_out, '--format', 'json'],
                original)
            assert_json_error(direct_no_flag, 1, 'E_IMPORT_NOT_FOUND')
            require(files(helper_out) == [], files(helper_out))
            help_record = r.command(row, [args.native_helper, '--help'], original)
            require(help_record['exit'] == 0 and
                    '--std-binding' in help_record['stdout'], help_record)
            for argv in (
                [args.native_helper, '--project', original / 'lang.toml',
                 '--std-binding'],
                [args.native_helper, '--project', original / 'lang.toml',
                 '--std-binding', ''],
                [args.native_helper, '--project', original / 'lang.toml',
                 '--std-binding', binding, '--std-binding', binding],
            ):
                assert_driver_argument(r.command(row, argv, original))

            # Direct helper parsing: check-only must stay a flag, not become a
            # --std-binding value.  A rejected command must not write a new
            # object or replace an existing one.
            parser_project = ordinary_project(
                'native-helper-argument-parser',
                'package app;\nfunc main() i32 { return 0; }\n')
            control_object = args.out / 'runs' / 'native-helper-check-only.o'
            control_object.unlink(missing_ok=True)
            control = r.command(
                row,
                [args.native_helper, '--project', parser_project / 'lang.toml',
                 '--out-object', control_object, '--check-only'],
                parser_project)
            require(control['exit'] == 0, control)
            require('"phase":"check"' in control['stdout'], control)
            require(not control_object.exists(), control)

            missing_object = args.out / 'runs' / 'native-helper-missing-value.o'
            missing_object.unlink(missing_ok=True)
            missing = r.command(
                row,
                [args.native_helper, '--project', parser_project / 'lang.toml',
                 '--out-object', missing_object,
                 '--std-binding', '--check-only'],
                parser_project)
            assert_driver_argument(missing)
            require(not missing_object.exists(), missing)

            old_object = args.out / 'runs' / 'native-helper-missing-value-old.o'
            old_object.write_bytes(b'old-native-object')
            preserved = r.command(
                row,
                [args.native_helper, '--project', parser_project / 'lang.toml',
                 '--out-object', old_object,
                 '--std-binding', '--check-only'],
                parser_project)
            assert_driver_argument(preserved)
            require(old_object.read_bytes() == b'old-native-object', preserved)

            other_object = args.out / 'runs' / 'native-helper-missing-other.o'
            other_object.unlink(missing_ok=True)
            other = r.command(
                row,
                [args.native_helper, '--project', parser_project / 'lang.toml',
                 '--out-object', other_object, '--std-binding', '--opt', '0'],
                parser_project)
            assert_driver_argument(other)
            require(not other_object.exists(), other)

            relative_project = ordinary_project(
                'native-helper-relative-binding',
                'package app;\nimport "std/py";\nfunc main() {}\n')
            relative_dir = r.fresh(args.out / 'native 绑定 目录')
            relative_dir.mkdir(parents=True)
            relative_binding = relative_dir / '标准 binding.json'
            shutil.copyfile(args.binding, relative_binding)
            relative_value = os.path.relpath(relative_binding, relative_project)
            require(' ' in relative_value and '标准' in relative_value,
                    relative_value)
            relative = r.command(
                row,
                [args.native_helper, '--project', relative_project / 'lang.toml',
                 '--check-only', '--std-binding', relative_value],
                relative_project)
            require(relative['exit'] == 1, relative)
            require('E_CAPABILITY_UNAVAILABLE' in relative['stderr'], relative)
            row['native_helper_argument_checks'] = {
                'control': {'exit': control['exit'],
                            'object_exists': control_object.exists()},
                'missing_before_check_only': {
                    'exit': missing['exit'],
                    'object_exists': missing_object.exists()},
                'missing_preserved_old_object': {
                    'exit': preserved['exit'],
                    'bytes': old_object.read_bytes().decode('ascii')},
                'missing_before_other_option': {
                    'exit': other['exit'],
                    'object_exists': other_object.exists()},
                'relative_spaced_chinese_binding': {
                    'value': relative_value, 'exit': relative['exit'],
                    'code': 'E_CAPABILITY_UNAVAILABLE'},
            }
            row['helper_forwarding_proof'] = {
                'with_flag': 'E_CAPABILITY_UNAVAILABLE',
                'without_flag': 'E_IMPORT_NOT_FOUND',
                'native_helper_sha256': sha(args.native_helper),
            }
        row['native_output'] = failures

    r.add('native-rejects-python-external-source', native_case)

    status = 'passed' if all(row['status'] == 'passed' for row in r.rows) else 'failed'
    summary = {
        'status': status,
        'operation': 'language-t10-cli-standard-binding-20261104',
        'base': '889577800cebcabada07edba2ee6cf3487289d67',
        'cases': r.rows,
        'commands': r.commands,
        'environment': {
            'langc': str(args.langc),
            'langc_sha256': sha(args.langc),
            'python': str(args.python),
            'helper': str(args.helper),
            'helper_sha256': sha(args.helper),
            'native_helper': None if not args.native_helper else str(args.native_helper),
            'binding': str(args.binding),
            'binding_sha256': sha(args.binding),
            'original04_sha256': sha(args.root / 'baseline/samples/04_numpy_torch.lang'),
            'reference_sha256': sha(args.reference),
        },
    }
    (args.out / 'results.json').write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({
        'status': status,
        'cases': {row['name']: row['status'] for row in r.rows},
        'results': str(args.out / 'results.json'),
    }, ensure_ascii=False))
    return 0 if status == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
