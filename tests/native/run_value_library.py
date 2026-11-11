#!/usr/bin/env python3
"""Execute the same text/Vec program on both backends and count native frees."""
import argparse
import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from string_cases import command, succeeded


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'root', 'clang', 'lld', 'runtime', 'python', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(status='failed', executions=[], lifetimes=[], panics=[])
    try:
        probe = args.out / 'lifetime.o'
        report['probe_compile'] = row = command([
            args.clang, '-std=c++20', '-O2', '-fno-exceptions', '-fno-rtti',
            '-fstack-protector-strong', '-c',
            args.root/'tests/native/value_library/lifetime_probe.cpp', '-o', probe])
        succeeded(row)
        manifest = args.root/'tests/native/value_library/lang.toml'
        for backend, opt in (('native', 0), ('native', 2), ('python', None)):
            out = args.out / f'{backend}-{opt}'
            argv = [args.cli, 'build', '--project', manifest, '--backend', backend,
                    '--out-dir', out, '--format', 'json']
            if opt is not None:
                argv += ['--opt', opt]
            row = dict(backend=backend, opt=opt, build=command(argv))
            report['executions'].append(row)
            succeeded(row['build'])
            row['execute'] = run = command([out/'program'] if backend == 'native'
                                          else [args.python, '-B', '-s', out/'program.py'])
            succeeded(run)
            if backend == 'native':
                binary = args.out / f'lifetime-{opt}'
                row = dict(opt=opt, link=command([
                    args.clang, '-nostdlib', '-static', f'--ld-path={args.lld}',
                    '-Wl,--wrap=main,--wrap=lang_alloc_v1,--wrap=lang_free_v1',
                    out/'program.o', probe, args.runtime, '-o', binary]))
                report['lifetimes'].append(row)
                succeeded(row['link'])
                row['execute'] = run = command([binary])
                counts = re.fullmatch(r'(\d+):(\d+)\n', run['stderr'])
                assert run['returncode'] == 0 and counts, run
                row['allocations'], row['frees'] = map(int, counts.groups())
                assert row['allocations'] > 0 and row['allocations'] == row['frees'], row
        cases = {
            'negative-index': ('let s="中"+"😀";let r=s[-1];', 'vec-index'),
            'index-end': ('let s="中"+"😀";let r=s[2];', 'vec-index'),
            'reverse-slice': ('let s="中"+"😀";let r=s[2:1];', 'vec-slice'),
            'push': ('let v=vec.make[i32](1,1);for x in v {vec.push[i32](v,2);break;}', 'active-iteration'),
            'reserve': ('let v=vec.make[i32](1,1);for x in v {vec.reserve[i32](v,8);break;}', 'active-iteration'),
            'pop': ('let v=vec.make[i32](1,1);for x in v {let r=vec.pop[i32](v);break;}', 'active-iteration'),
        }
        for name, (body, cause) in cases.items():
            project = args.out / 'panics' / name
            (project/'src').mkdir(parents=True, exist_ok=True)
            (project/'lang.toml').write_text(
                'schema_version=1\n[package]\nname="panic"\nsource_root="src"\n'
                'entry="app"\n[build]\nbackend="native"\nword_bits=64\n')
            (project/'src/main.wz').write_text(
                'package app;import "std/vec" as vec;func main() i32 {' + body + 'return 0;}')
            for backend in ('native', 'python'):
                out = project / backend
                row = dict(case=name, backend=backend, build=command([
                    args.cli, 'build', '--project', project/'lang.toml', '--backend', backend,
                    '--out-dir', out, '--format', 'json']))
                report['panics'].append(row)
                succeeded(row['build'])
                row['execute'] = run = command([out/'program'] if backend == 'native'
                                              else [args.python, '-B', '-s', out/'program.py'])
                marker = f'LANG_NATIVE_PANIC:v1:{cause}:' if backend == 'native' else 'LANG_PANIC:'
                assert run['returncode'] == 101 and marker in run['stderr'], run
        report['status'] = 'passed'
    finally:
        path = args.out/'value-library-results.json'
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], report=str(path))))


if __name__ == '__main__':
    main()
