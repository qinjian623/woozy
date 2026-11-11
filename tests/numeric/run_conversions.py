#!/usr/bin/env python3
"""Explicit numeric conversions: real build/run and independent expectations.

Covers, for the same sources and both backends:
  * the executable value tables in `tests/numeric/values`,
    `tests/numeric/float-conversions` and `tests/numeric/float-narrowing`
    (integer truncation, integer/float conversion, f64->f32
    normal/subnormal/overflow/signed-zero/NaN classes, constant and dynamic
    paths, variable/read/parameter/return/indirect calls, evaluation order,
    short circuit and typed throws);
  * the native probe sweep, split into the frozen legacy fixture (367
    requests) and the independent narrowing fixture (271 requests): bit
    patterns are handed to the generated functions through their real LLVM
    signatures and compared with an exact `Fraction` oracle
    (`tests/native/float_expected.py`), never with the implementation;
  * the frozen inputs that used to be rejected only for f64->f32 narrowing,
    now checked, built and run in both backends;
  * static failures (source errors and unsupported bool/non-numeric pairs)
    in both backends, including fresh "no output" and stale "unchanged bytes"
    checks;
  * dynamic failures: check accepts, real execution Panics with exit 101 and
    the backend marker;
  * static ELF properties: no PT_INTERP, no DT_NEEDED and no libm/trunc symbol.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'native'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from float_expected import decoded, operation, rounded  # noqa: E402
import cases  # noqa: E402
from fractions import Fraction  # noqa: E402


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def command(argv, cwd=None, timeout=180, text=True):
    argv = [str(a) for a in argv]
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    try:
        p = subprocess.run(argv, cwd=cwd, capture_output=True, text=text,
                           timeout=timeout, env=env)
        return dict(argv=argv, cwd=None if cwd is None else str(cwd),
                    exit=p.returncode, stdout=p.stdout, stderr=p.stderr)
    except subprocess.TimeoutExpired as e:
        return dict(argv=argv, cwd=None if cwd is None else str(cwd),
                    exit='timeout', stdout=e.stdout or '', stderr=e.stderr or '')


def ok(row, what):
    assert row['exit'] == 0, f'{what}: {row}'


def manifest(root, backend='native', word_bits=64):
    (root / 'src').mkdir(parents=True, exist_ok=True)
    (root / 'lang.toml').write_text(
        'schema_version = 1\n[package]\nname = "app"\nsource_root = "src"\n'
        'entry = "app"\n[build]\nbackend = "' + backend + '"\n'
        'word_bits = ' + str(word_bits) + '\n')


def project(root, source, backend='native', word_bits=64):
    manifest(root, backend, word_bits)
    (root / 'src' / 'main.wz').write_text(source)


def diagnostics(build):
    try:
        return json.loads(build['stdout'])
    except ValueError:
        return {}


def elf_checks(program):
    headers = command(['readelf', '--wide', '-l', program])
    dynamic = command(['readelf', '--wide', '-d', program])
    symbols = command(['nm', '--undefined-only', program])
    undefined_rows = [line.split() for line in symbols['stdout'].splitlines()
                      if line.strip()]
    undefined = [fields[-1] for fields in undefined_rows]
    optional_hooks = {'lang_vec_observe_v1', 'lang_process_init_v1'}
    unexpected = [fields for fields in undefined_rows
                  if fields[0] != 'w' or fields[-1] not in optional_hooks]
    check = dict(command=[headers, dynamic, symbols], undefined=undefined,
                 unexpected=unexpected)
    assert 'INTERP' not in headers['stdout'], 'PT_INTERP present'
    assert 'NEEDED' not in dynamic['stdout'], 'DT_NEEDED present'
    assert 'There is no dynamic section' in dynamic['stdout'] or \
        dynamic['stdout'].strip() == '', 'unexpected dynamic section'
    assert not unexpected, f'unexpected undefined symbols: {unexpected}'
    assert not any(name.startswith(('trunc', 'round', 'rint', 'nearbyint',
                                    'floor', 'ceil', 'fmod', '__trunc'))
                   for name in undefined), undefined
    return check


def values_matrix(args, report, tmp):
    rows = []
    report['values'] = rows

    def run_project(label, root, frontend32):
        for backend in ('native', 'python'):
            row = dict(project=label, backend=backend, word_bits=64)
            row['check'] = r = command([args.cli, 'check', '--project',
                                        root / 'lang.toml', '--backend', backend,
                                        '--format', 'json'])
            ok(r, f'{label} check {backend}')
            assert diagnostics(r)['status'] == 'ok', r
            rows.append(row)
            for opt in (0, 2) if backend == 'native' else (0,):
                out = tmp / label / backend / f'O{opt}'
                build = [args.cli, 'build', '--project', root / 'lang.toml',
                         '--backend', backend, '--out-dir', out, '--format',
                         'json']
                if backend == 'native':
                    build += ['--opt', str(opt)]
                row[f'build{opt if backend == "native" else ""}'] = r = command(build)
                ok(r, f'{label} build')
                if backend == 'native':
                    program = out / 'program'
                    row[f'execute{opt}'] = r = command([program])
                else:
                    program = out / 'program.py'
                    row['execute'] = r = command([args.python, program], cwd=out)
                ok(r, f'{label} run')
                row[f'artifacts{opt if backend == "native" else ""}'] = {
                    str(p.relative_to(out)): sha(p) for p in sorted(out.rglob('*'))
                    if p.is_file()}
                if backend == 'native':
                    row[f'elf{opt}'] = elf_checks(program)
        if frontend32:
            # The frontend must accept isize/usize conversions for a 32-bit
            # target too; neither backend lowers python/32 or native/32, so
            # this is check only.
            row = dict(project=label, backend='native', word_bits=32,
                       frontend_only=True)
            narrow = tmp / f'{label}32'
            project(narrow, (root / 'src' / 'main.wz').read_text(),
                    word_bits=32)
            row['check'] = r = command([args.cli, 'check', '--project',
                                        narrow / 'lang.toml', '--backend',
                                        'native', '--format', 'json'])
            ok(r, f'{label} check word_bits=32')
            assert diagnostics(r)['status'] == 'ok', r
            rows.append(row)

    run_project('values', args.root / 'tests/numeric/values', True)
    run_project('float-conversions',
                args.root / 'tests/numeric/float-conversions', False)
    run_project('float-narrowing',
                args.root / 'tests/numeric/float-narrowing', False)
    return rows

# Fixed probe values, hand chosen around RNE midpoints and integer bounds.
FIXED_I64 = [0, 1, -1, 16777215, 16777216, 16777217, 16777218, 16777219,
             16777220, -16777217, -16777219, 2147483647, -2147483648,
             9007199254740991, 9007199254740992, 9007199254740993,
             9007199254740995, (1 << 62) + (1 << 38) + 1, (1 << 63) - 1,
             -(1 << 63)]
FIXED_U64 = [0, 1, 16777215, 16777216, 16777217, 16777218, 16777219,
             16777220, 4294967295, 9007199254740993,
             (1 << 63) + (1 << 39) + 1, (1 << 63) + (1 << 39),
             (1 << 64) - (1 << 40) - 1, (1 << 64) - 1]


def ieee_bits(value, width):
    """Exact IEEE bit pattern of a Python float (an exact power-of-two neighbour)."""
    import math
    import struct
    try:
        packed = struct.pack('>f' if width == 32 else '>d', value)
    except OverflowError:
        packed = struct.pack('>f' if width == 32 else '>d',
                             math.copysign(math.inf, value))
    return int.from_bytes(packed, 'big')


def float_patterns(width):
    frac, exp = (23, 8) if width == 32 else (52, 11)
    sign = 1 << (width - 1)
    one = 0x3f800000 if width == 32 else 0x3ff0000000000000
    half = 0x3f000000 if width == 32 else 0x3fe0000000000000
    patterns = [0, sign, 1, sign | 1, 2, half, one, sign | one,
                one + (1 << (frac - 1)), sign | (one + (1 << (frac - 1))),
                ((1 << exp) - 2) << frac, sign | (((1 << exp) - 2) << frac)]
    values = [2147483520, 2147483648, 4294967040, 4294967296,
              9223370937343148032, 9223372036854775808,
              18446742974197923840, 18446744073709551616,
              -2147483648, -4294967296, -9223372036854775808,
              -18446744073709551616]
    patterns.extend(ieee_bits(float(v), width) for v in values)
    return patterns


def float_edge_patterns(width):
    """Exact-region identity/widening patterns, including all NaN categories."""
    frac, exp = (23, 8) if width == 32 else (52, 11)
    sign = 1 << (width - 1)
    one = 0x3f800000 if width == 32 else 0x3ff0000000000000
    half = 0x3f000000 if width == 32 else 0x3fe0000000000000
    infinity = ((1 << exp) - 1) << frac
    quiet = infinity | (1 << (frac - 1))
    signaling = infinity | 1
    values = [
        0, sign, 1, sign | 1,
        (1 << frac) - 1, sign | ((1 << frac) - 1),
        1 << frac, sign | (1 << frac),
        half, sign | half,
        one - 1, one, one + 1,
        sign | (one - 1), sign | one, sign | (one + 1),
        infinity - 1, sign | (infinity - 1), infinity, sign | infinity,
        quiet, sign | quiet, quiet | 0x123, sign | (quiet | 0x123),
        signaling, sign | signaling, signaling | 0x123,
    ]
    return list(dict.fromkeys(values))


def widen32(pattern):
    """Independent binary32 -> binary64 expectation from exact decoded values."""
    category, sign, value = decoded(pattern, 32)
    if category == 'nan':
        return 'nan'
    if category == 'infinity':
        return (sign << 63) | 0x7ff0000000000000
    if category == 'zero':
        return sign << 63
    return rounded(value, 64, zero_sign=sign)


def narrow64(pattern):
    """Exact f64 -> f32 expectation from the decoded source value.

    Binary32 rounding is done by the test-only integer RNE kernel; finite
    overflow becomes signed infinity, signed zero keeps its sign, and NaN is
    compared only by class. The critical constants below were also checked by
    the LLVM 22.1.8 APFloat oracle used by tests/floating.
    """
    category, sign, value = decoded(pattern, 64)
    if category == 'nan':
        return 'nan'
    if category == 'infinity':
        return sign << 31 | 0x7f800000
    if category == 'zero':
        return sign << 31
    return rounded(value, 32, zero_sign=sign)


def f64_exact(value):
    return rounded(Fraction(value), 64, zero_sign=int(value < 0))


def narrow_patterns():
    """Edge patterns plus fixed random binary64 inputs for the dynamic path."""
    import random
    sign = 1 << 63
    infinity = 0x7ff0000000000000
    quiet = infinity | (1 << 51)
    signaling = infinity | 1
    patterns = [0, sign, 1, sign | 1, 2, sign | 2,
                infinity, sign | infinity,
                quiet, sign | quiet, quiet | 0x123, sign | (quiet | 0x123),
                signaling, sign | signaling, signaling | 0x123,
                sign | (signaling | 0x123)]
    min_normal = Fraction(1, 2 ** 126)
    min_subnormal = Fraction(1, 2 ** 149)
    half_min_subnormal = Fraction(1, 2 ** 150)
    tiny = Fraction(1, 2 ** 200)
    max_f32 = Fraction(2 ** 24 - 1) * Fraction(2) ** 104
    overflow_tie = max_f32 + Fraction(2) ** 103
    exact = [
        min_subnormal, -min_subnormal,
        half_min_subnormal, -half_min_subnormal,
        half_min_subnormal - tiny, half_min_subnormal + tiny,
        3 * half_min_subnormal,
        3 * min_subnormal,
        Fraction(2 ** 23 - 1) * min_subnormal,
        min_normal - half_min_subnormal,
        min_normal, min_normal + half_min_subnormal,
        max_f32, -max_f32,
        overflow_tie, -overflow_tie,
        overflow_tie - Fraction(2) ** 75,
        overflow_tie + Fraction(2) ** 75,
        Fraction(1), -Fraction(1),
        Fraction(1) + Fraction(1, 2 ** 24),
        Fraction(1) - Fraction(1, 2 ** 24),
        Fraction(1) + Fraction(3, 2 ** 24),
        Fraction(1) + Fraction(1, 2 ** 24) + Fraction(1, 2 ** 50),
        Fraction(1) + Fraction(1, 2 ** 24) - Fraction(1, 2 ** 50),
        Fraction(1) + Fraction(1, 2 ** 23),
        Fraction(1) + Fraction(1, 2 ** 22),
        Fraction(2) ** -149, Fraction(2) ** -126,
        Fraction(2) ** 127, Fraction(2) ** 128,
        (Fraction(2 ** 53 - 1) * Fraction(2) ** 971),
    ]
    for value in exact:
        patterns.append(f64_exact(value))
        patterns.append(f64_exact(-value))
    rng = random.Random(20261109)
    patterns.extend(rng.getrandbits(64) for _ in range(64))
    return list(dict.fromkeys(patterns))


def narrow_request(name, pattern, expected, source):
    return dict(name=name, result='f32', arg_widths=[64],
                arguments=[pattern], expected=expected, source=source)


def probe_requests():
    requests = []
    for name, result, width, values in (
            ('i2f32', 'f32', 64, FIXED_I64),
            ('i2f64', 'f64', 64, FIXED_I64),
            ('u2f32', 'f32', 64, FIXED_U64),
            ('u2f64', 'f64', 64, FIXED_U64),
            ('b2f32', 'f32', 8, [-128, -1, 0, 127]),
            ('c2f32', 'f32', 8, [0, 1, 255]),
            ('b2f64', 'f64', 16, [-32768, -1, 0, 32767]),
            ('c2f64', 'f64', 16, [0, 1, 65535]),
            ('d2f32', 'f32', 32, [-2147483648, -1, 0, 2147483647]),
            ('e2f64', 'f64', 32, [0, 1, 4294967295])):
        for value in values:
            requests.append(dict(
                name=name, result=result, arg_widths=[width],
                arguments=[value & ((1 << width) - 1)],
                expected=rounded(Fraction(value), int(result[1:])),
                source=f'Fraction({value}) rounded once to {result}, RNE'))
    for width in (32, 64):
        for target in ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64'):
            name = ('f2' if width == 64 else 's2') + target
            bits = int(target[1:])
            low = -(1 << (bits - 1)) if target[0] == 'i' else 0
            high = ((1 << (bits - 1)) - 1) if target[0] == 'i' else                 (1 << bits) - 1
            for pattern in float_patterns(width):
                category, _, value = decoded(pattern, width)
                if category in ('nan', 'infinity'):
                    continue  # dynamic Panic material, checked by execution
                truncated = int(value)  # Fraction -> exact truncation
                if not low <= truncated <= high:
                    continue
                requests.append(dict(
                    name=name, result=target, arg_widths=[width],
                    arguments=[pattern], expected=truncated,
                    source='exact IEEE decode, truncate towards zero, range-check'))
    for width in (32, 64):
        for pattern in float_edge_patterns(width):
            category, _, _ = decoded(pattern, width)
            requests.append(dict(
                name=f'idf{width}', result=f'f{width}',
                arg_widths=[width], arguments=[pattern],
                expected='nan' if category == 'nan' else pattern,
                source='identity must preserve every non-NaN bit; NaN is compared by class'))
    for pattern in float_edge_patterns(32):
        requests.append(dict(
            name='widen', result='f64', arg_widths=[32], arguments=[pattern],
            expected=widen32(pattern),
            source='exact binary32 value widened to binary64; NaN by class'))
    for value in (-2147483648, -16777219, -16777217, -1, 0, 1, 16777215,
                  16777216, 16777217, 16777218, 16777219, 16777220,
                  2147483647):
        inner = rounded(Fraction(value), 32)
        category, sign, actual = decoded(inner, 32)
        requests.append(dict(
            name='widen_after_i32', result='f64', arg_widths=[32],
            arguments=[value & 0xffffffff],
            expected=rounded(actual, 64, zero_sign=sign),
            source='int->f32 RNE first, then exact widening to f64'))

    return requests


def narrowing_probe_requests():
    requests = []
    patterns = narrow_patterns()
    narrow_source = ('exact f64 decode, then one RNE rounding to binary32; '
                     'overflow to signed infinity, NaN by class')
    for pattern in patterns:
        requests.append(narrow_request('narrow', pattern,
                                       narrow64(pattern), narrow_source))
    # The variable-read and indirect-call routes must produce the same bits
    # for every edge value, not only for a convenient finite subset.
    for name in ('narrow_read', 'narrow_indirect'):
        for pattern in patterns[:48]:
            requests.append(narrow_request(name, pattern, narrow64(pattern),
                                           'same narrowing through ' + name))
    add_pairs = [
        (f64_exact(Fraction(1)), f64_exact(Fraction(1, 2 ** 24))),
        (f64_exact(Fraction(1)), f64_exact(Fraction(3, 2 ** 24))),
        (f64_exact(Fraction(1)), f64_exact(Fraction(-1, 2 ** 24))),
        (f64_exact(Fraction(2 ** 24 - 1) * Fraction(2) ** 104),
         f64_exact(Fraction(2) ** 103)),
        (f64_exact(Fraction(2 ** 24 - 1) * Fraction(2) ** 104),
         f64_exact(-Fraction(2) ** 75)),
        (f64_exact(Fraction(2) ** -150), f64_exact(Fraction(2) ** -151)),
        (f64_exact(Fraction(2) ** -150), f64_exact(-Fraction(2) ** -151)),
        (0x7ff0000000000000, 0xfff0000000000000),
        (0, 1 << 63), (1 << 63, 1 << 63),
        (0x7fefffffffffffff, 0x7fefffffffffffff),
        (0xffefffffffffffff, 0xffefffffffffffff),
    ]
    add_pairs += [(patterns[i], patterns[j])
                  for i, j in ((0, 0), (2, 2), (4, 5), (30, 31), (40, 41))]
    for x, y in add_pairs:
        raw = operation('add', [x, y], 64)
        requests.append(dict(
            name='narrow_add', result='f32', arg_widths=[64, 64],
            arguments=[x, y], expected=narrow64(raw),
            source='f64 add with one RNE step, then f64->f32 RNE'))
    expr_pairs = [
        (f64_exact(Fraction(1)), f64_exact(Fraction(1, 2 ** 24))),
        (f64_exact(Fraction(2 ** 24 - 1) * Fraction(2) ** 104),
         f64_exact(Fraction(2))),
        (f64_exact(Fraction(2) ** -150), f64_exact(Fraction(2))),
        (f64_exact(Fraction(1) + Fraction(1, 2 ** 24)),
         f64_exact(Fraction(1))),
        (0x7ff0000000000000, 0x3ff0000000000000),
        (0, 1 << 63),
    ]
    for x, y in expr_pairs:
        raw = operation('nofma', [x, y, x], 64)
        requests.append(dict(
            name='narrow_expr', result='f32', arg_widths=[64, 64],
            arguments=[x, y], expected=narrow64(raw),
            source='f64 x*y then +x, each RNE, then f64->f32 RNE'))
    constants = [
        ('narrow_tie_down', 0x3f800000),
        ('narrow_tie_up', 0x3f800002),
        ('narrow_above_tie', 0x3f800001),
        ('narrow_max_finite', 0x7f7fffff),
        ('narrow_below_overflow', 0x7f7fffff),
        ('narrow_overflow_tie', 0x7f800000),
        ('narrow_negative_overflow', 0xff800000),
        ('narrow_positive_infinity', 0x7f800000),
        ('narrow_negative_infinity', 0xff800000),
        ('narrow_half_min_subnormal', 0x00000000),
        ('narrow_negative_half_min_subnormal', 0x80000000),
        ('narrow_min_subnormal_tie_up', 0x00000002),
        ('narrow_min_subnormal_above_half', 0x00000001),
        ('narrow_min_subnormal_below_half', 0x00000000),
        ('narrow_min_normal_tie', 0x00800000),
        ('narrow_nan', 'nan'),
        ('narrow_positive_zero', 0x00000000),
        ('narrow_negative_zero', 0x80000000),
        ('narrow_const_read', 0x3f800000),
        ('narrow_from_const', 0x3f800000),
        ('narrow_from_const_infinity', 0x7f800000),
        ('narrow_from_const_overflow', 0x7f800000),
    ]
    for name, expected in constants:
        requests.append(dict(name=name, result='f32', arg_widths=[],
                             arguments=[], expected=expected,
                             source='constant folding of the same IEEE RNE '
                                    'rule; expected bits checked by APFloat'))
    return requests


def probe_matrix_for(args, report, tmp, key, manifest, requests,
                     min_controls=5):
    rows = dict(status='failed', runs=[], totals=dict(passed=0, failed=0))
    report[key] = rows
    for opt in (0, 2):
        out = tmp / 'probes' / key / f'O{opt}'
        out.mkdir(parents=True, exist_ok=True)
        request_file = out / 'requests.tsv'
        request_file.write_text(''.join(
            request['name'] + '\t' + request['result'] + ''.join(
                f'\t{argument:0{width // 4}x}'
                for argument, width in zip(request['arguments'],
                                           request['arg_widths'])) + '\n'
            for request in requests))
        row = dict(opt=opt, values=[])
        rows['runs'].append(row)
        row['build'] = r = command([
            args.probe, args.root / manifest, out / 'probe', opt, request_file])
        ok(r, 'probe emit')
        row['metadata'] = json.loads((out / 'probe/metadata.json').read_text())
        assert row['metadata']['probes'] == len(requests), row['metadata']
        assert row['metadata']['bad_ir_rejected'] >= \
            row['metadata']['convert_instructions'], row['metadata']
        assert row['metadata']['convert_instructions'] >= 20, row['metadata']
        assert row['metadata']['legal_retarget_controls'] >= min_controls, \
            row['metadata']
        for mutation_key in ('bool_mutations', 'float_width_mutations',
                             'origin_mutations', 'operand_mutations',
                             'address_mutations', 'compound_operand_mutations',
                             'unary_operand_mutations',
                             'external_operand_mutations'):
            assert row['metadata'][mutation_key] >= 1, \
                (mutation_key, row['metadata'])
        # The bool-source mutation inserts into the same instruction vector, so
        # the relocation path must really run (no stale reference is used).
        assert row['metadata']['insert_mutations'] >= 1, row['metadata']
        row['observe_build'] = r = command([
            args.clang, f'-O{opt}', '-ffp-contract=off',
            out / 'probe/observe.ll', args.runtime, '-o', out / 'observe'])
        ok(r, 'probe observe build')
        raw_run = command([out / 'observe'], text=False)
        assert raw_run['exit'] == 0, raw_run['exit']
        data = raw_run['stdout']
        row['execute'] = dict(raw_run, stdout_hex=data.hex(),
                              stderr_hex=raw_run['stderr'].hex())
        del row['execute']['stdout'], row['execute']['stderr']
        offset = 0
        for request in requests:
            result = request['result']
            width = int(result[1:])
            if result in ('f32', 'f64'):
                size = width // 8
            else:
                size = width // 8
            assert offset + size <= len(data), 'missing probe observation'
            raw = int.from_bytes(data[offset:offset + size], 'little')
            offset += size
            if result in ('f32', 'f64'):
                if request['expected'] == 'nan':
                    category, _, _ = decoded(raw, width)
                    matched = category == 'nan'
                    observed, expected = category, 'nan'
                else:
                    matched = raw == request['expected']
                    observed = f'{raw:0{width // 4}x}'
                    expected = f"{request['expected']:0{width // 4}x}"
            else:
                if result[0] == 'u':
                    observed_value = raw
                else:
                    observed_value = raw - (1 << width) \
                        if raw >> (width - 1) else raw
                matched = observed_value == request['expected']
                observed = str(observed_value)
                expected = str(request['expected'])
            observation = dict(
                function=request['name'], result=result,
                arguments=[f'{a:0{w // 4}x}' for a, w in
                           zip(request['arguments'], request['arg_widths'])],
                width=width, expected=expected, actual=observed,
                expected_source=request['source'],
                status='passed' if matched else 'failed')
            row['values'].append(observation)
            rows['totals']['passed' if matched else 'failed'] += 1
            assert matched, observation
        assert offset == len(data), 'extra probe observation bytes'
    python_probe(args, report, tmp, key, manifest, requests)
    rows['status'] = 'passed'


def python_probe(args, report, tmp, key, manifest, requests):
    rows = dict(status='not_run', reason='--helper not given', values=[],
                totals=dict(passed=0, failed=0))
    report[key]['python'] = rows
    if not args.helper:
        return
    generated = tmp / 'probes' / 'python' / key
    result = command([args.helper, '--project',
                      args.root / manifest, '--out-dir', generated])
    ok(result, 'python probe emit')
    spec = generated / 'requests.json'
    spec.write_text(json.dumps(requests) + '\n')
    observed = command([args.python,
                        args.root / 'tests/numeric/observe_conversions.py',
                        generated, spec])
    ok(observed, 'python probe observe')
    rows['execute'] = observed
    for request, row in zip(requests, json.loads(observed['stdout'])):
        assert row['outcome'] == 'return', row
        actual = row['result']
        expected = request['expected']
        result_type = request['result']
        width = int(result_type[1:])
        if result_type in ('f32', 'f64'):
            if expected == 'nan':
                matched = actual['class'] == 'nan'
            else:
                matched = actual['bits'] == f'{expected:0{width // 4}x}'
        else:
            matched = actual['value'] == expected
        observation = dict(function=request['name'], result=result_type,
                           arguments=[f'{a:0{w // 4}x}' for a, w in
                                      zip(request['arguments'],
                                          request['arg_widths'])],
                           expected='nan' if expected == 'nan' else
                           f'{expected:0{width // 4}x}'
                           if result_type in ('f32', 'f64') else str(expected),
                           actual=actual, expected_source=request['source'],
                           status='passed' if matched else 'failed')
        rows['values'].append(observation)
        rows['totals']['passed' if matched else 'failed'] += 1
        assert matched, observation
    rows['status'] = 'passed'


def probe_matrix(args, report, tmp):
    """Run the frozen legacy project and the independent narrowing project.

    The original float-conversions source keeps its exact bytes and its 367
    requests; the new narrowing declarations live in a separate project and
    contribute the remaining 271 requests.
    """
    if not args.probe:
        report['probes'] = dict(status='not_run', reason='--probe not given')
        report['probes_narrowing'] = dict(status='not_run',
                                          reason='--probe not given')
        return
    legacy = probe_requests()
    narrowing = narrowing_probe_requests()
    assert len(legacy) == 367, ('legacy probe count', len(legacy))
    assert len(narrowing) == 271, ('narrowing probe count', len(narrowing))
    probe_matrix_for(args, report, tmp, 'probes',
                     'tests/numeric/float-conversions/lang.toml', legacy)
    probe_matrix_for(args, report, tmp, 'probes_narrowing',
                     'tests/numeric/float-narrowing/lang.toml', narrowing,
                     min_controls=1)
    report['probes']['narrowing_fixture'] = 'probes_narrowing'


def accepted_matrix(args, report, tmp):
    rows = []
    report['accepted'] = rows
    for case in cases.accepted_cases():
        root = tmp / 'accepted' / case['id'] / 'project'
        project(root, 'package app;\n' + case['source'] + '\n')
        row = dict(id=case['id'], reason=case['reason'], checks=[], runs=[])
        rows.append(row)
        for backend in ('native', 'python'):
            r = command([args.cli, 'check', '--project', root / 'lang.toml',
                         '--backend', backend, '--format', 'json'])
            ok(r, f'accepted case {case["id"]} check {backend}')
            assert diagnostics(r)['status'] == 'ok', r
            row['checks'].append(dict(backend=backend, command=r))
        for opt in (0, 2):
            out = tmp / 'accepted' / case['id'] / 'native' / f'O{opt}'
            build = command([args.cli, 'build', '--project',
                             root / 'lang.toml', '--backend', 'native',
                             '--out-dir', out, '--opt', str(opt),
                             '--format', 'json'])
            ok(build, f'accepted case {case["id"]} native build O{opt}')
            ran = command([out / 'program'])
            ok(ran, f'accepted case {case["id"]} native run O{opt}')
            row['runs'].append(dict(backend='native', opt=opt, build=build,
                                    execute=ran))
        out = tmp / 'accepted' / case['id'] / 'python' / 'O0'
        build = command([args.cli, 'build', '--project', root / 'lang.toml',
                         '--backend', 'python', '--out-dir', out,
                         '--format', 'json'])
        ok(build, f'accepted case {case["id"]} python build')
        ran = command([args.python, out / 'program.py'], cwd=out)
        ok(ran, f'accepted case {case["id"]} python run')
        row['runs'].append(dict(backend='python', opt=0, build=build,
                                execute=ran))
    return rows


def static_matrix(args, report, tmp):
    rows = []
    report['static'] = rows
    for case in cases.static_cases():
        root = tmp / 'static' / case['id'] / 'project'
        project(root, 'package app;\n' + case['source'] + '\n')
        row = dict(id=case['id'], code=case['code'], reason=case['reason'],
                   checks=[], build=[])
        rows.append(row)
        for backend in ('native', 'python'):
            r = command([args.cli, 'check', '--project', root / 'lang.toml',
                         '--backend', backend, '--format', 'json'])
            codes = {d['code'] for d in diagnostics(r).get('diagnostics', [])}
            row['checks'].append(dict(backend=backend, command=r, codes=sorted(codes)))
            assert r['exit'] == 1 and codes == {case['code']}, (case['id'], backend, r)
        for backend in ('native', 'python'):
            fresh = tmp / 'static' / case['id'] / backend / 'fresh'
            stale = tmp / 'static' / case['id'] / backend / 'stale'
            stale.mkdir(parents=True, exist_ok=True)
            for name in ('program', 'program.o', 'unrelated'):
                (stale / name).write_bytes(('old-' + name).encode())
            before = {p.name: sha(p) for p in stale.iterdir()}
            for label, out in (('fresh', fresh), ('stale', stale)):
                build = [args.cli, 'build', '--project', root / 'lang.toml',
                         '--backend', backend, '--out-dir', out,
                         '--format', 'json']
                if backend == 'native':
                    build += ['--opt', '0']
                r = command(build)
                entry = dict(backend=backend, label=label, command=r)
                row['build'].append(entry)
                assert r['exit'] == 1, (case['id'], backend, label, r)
                if label == 'fresh':
                    assert not out.exists(), f'{case["id"]} created output'
                else:
                    assert before == {p.name: sha(p) for p in stale.iterdir()}, \
                        f'{case["id"]} changed stale output'
            assert not list(stale.parent.glob('.woozyc-*')), 'left assembly temp'
    return rows


def panic_matrix(args, report, tmp):
    rows = []
    report['panic'] = rows
    for case in cases.panic_cases():
        root = tmp / 'panic' / case['id'] / 'project'
        project(root, 'package app;\n' + case['source'] + '\n')
        row = dict(id=case['id'], reason=case['reason'], checks=[], runs=[])
        rows.append(row)
        for backend in ('native', 'python'):
            r = command([args.cli, 'check', '--project', root / 'lang.toml',
                         '--backend', backend, '--format', 'json'])
            row['checks'].append(dict(backend=backend, command=r))
            ok(r, f'panic case {case["id"]} check {backend}')
        for opt in (0, 2):
            out = tmp / 'panic' / case['id'] / 'native' / f'O{opt}'
            r = command([args.cli, 'build', '--project', root / 'lang.toml',
                         '--backend', 'native', '--out-dir', out, '--opt',
                         str(opt), '--format', 'json'])
            ok(r, 'panic build')
            ran = command([out / 'program'])
            row['runs'].append(dict(backend='native', opt=opt, build=r,
                                    execute=ran))
            assert ran['exit'] == 101 and ran['stdout'] == '', ran
            assert ran['stderr'].startswith(
                'LANG_NATIVE_PANIC:v1:' + case['native'] + ':'), ran
        out = tmp / 'panic' / case['id'] / 'python' / 'O0'
        r = command([args.cli, 'build', '--project', root / 'lang.toml',
                     '--backend', 'python', '--out-dir', out, '--format',
                     'json'])
        ok(r, 'panic build python')
        ran = command([args.python, out / 'program.py'], cwd=out)
        row['runs'].append(dict(backend='python', opt=0, build=r, execute=ran))
        assert ran['exit'] == 101 and ran['stdout'] == '', ran
        assert ran['stderr'].startswith('LANG_PANIC: '), ran
        assert case['python'] in ran['stderr'], ran
    return rows


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'root', 'python', 'out', 'probe', 'clang', 'runtime',
                 'helper'):
        parser.add_argument('--' + name, type=Path,
                            required=name not in ('probe', 'clang', 'runtime',
                                                  'helper'))
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix='work-', dir=args.out))
    report = dict(operation_id='language-explicit-numeric-conversions-20261106',
                  contract='compiler-dev/1', status='failed', tempdir=str(tmp),
                  implementation_sha256={f: sha(args.root / f) for f in (
                      'src/sema/check.cpp', 'src/hir/hir.cpp',
                      'src/ir/verify.cpp', 'src/backend/native/emit.cpp',
                      'src/backend/python/emitter.cpp',
                      'runtime/python/langrt.py', 'include/lang/hir/hir.hpp',
                      'include/lang/backend/python.hpp')},
                  input_sha256={str(p.relative_to(args.root)): sha(p) for p in
                                sorted((args.root / 'tests/numeric').rglob('*'))
                                if p.is_file()},
                  failures=[])
    try:
        values_matrix(args, report, tmp)
        accepted_matrix(args, report, tmp)
        probe_matrix(args, report, tmp)
        static_matrix(args, report, tmp)
        panic_matrix(args, report, tmp)
        legacy = report.get('probes', {}).get('totals', {})
        narrowing = report.get('probes_narrowing', {}).get('totals', {})
        report['counts'] = dict(
            values_runs=len(report['values']),
            accepted_cases=len(report['accepted']),
            probes=dict(passed=legacy.get('passed', 0) +
                                narrowing.get('passed', 0),
                        failed=legacy.get('failed', 0) +
                               narrowing.get('failed', 0)),
            probes_legacy=legacy,
            probes_narrowing=narrowing,
            static_cases=len(report['static']),
            panic_cases=len(report['panic']))
        report['status'] = 'passed'
    except AssertionError as error:
        report['failures'].append(str(error))
        raise
    finally:
        (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['counts']))


main()
