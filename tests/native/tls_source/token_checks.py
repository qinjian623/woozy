#!/usr/bin/env python3
"""Check token validation, lifetime, concurrent release and public Wz calls."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from focused_cleanup import environment
from run_tls_source import stage_project
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--cxx', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--addon', type=Path, default=HERE / '.build/addon')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []

    def run(argv, timeout):
        result = subprocess.run(list(map(str, argv)), capture_output=True,
                                text=True, timeout=timeout, env=environment())
        rows.append(dict(argv=list(map(str, argv)), exit=result.returncode,
                         stdout=result.stdout, stderr=result.stderr))
        (args.out / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
        if result.returncode:
            raise AssertionError(rows[-1])

    for name, flags in [('O0', ['-O0']), ('O2', ['-O2']),
                        ('ASAN', ['-O1', '-g', '-fsanitize=address,undefined',
                                  '-fno-omit-frame-pointer'])]:
        binary = args.out / name
        run([args.cxx, '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
             *flags, '-I' + str(ROOT / 'runtime/hosted'),
             HERE / 'token_checks.cpp', ROOT / 'runtime/hosted/tls_source.cpp',
             ROOT / 'runtime/hosted/tls.cpp', '-lssl', '-lcrypto',
             '--ld-path=' + str(args.cxx.parent / 'ld.lld'), '-o', binary], 120)
        run([binary], 30)
        assert 'TLS_TOKEN_CHECKS_OK' in rows[-1]['stdout']
        print('PASS token lifetime ' + name, flush=True)

    manifest = stage_project(HERE / 'projects/token_probe/lang.toml',
                             args.out / 'project', args.addon.resolve())
    for opt in (0, 2):
        output = args.out / ('source-O' + str(opt))
        run([args.cli, 'build', '--project',
             manifest, '--backend', 'native',
             '--opt', opt, '--out-dir', output, '--format', 'json'], 120)
        run([output / 'program'], 10)
        print('PASS token public Wz O' + str(opt), flush=True)


if __name__ == '__main__':
    main()
