"""Regenerate independent expected values using the test-only APFloat oracle."""
import argparse
import json
from pathlib import Path
import random
import subprocess


def inputs():
    rng = random.Random(20261104)
    for width, fraction, bias in ((32, 23, 127), (64, 52, 1023)):
        sign = 1 << (width - 1)
        one = bias << fraction
        normal = 1 << fraction
        infinity = ((1 << (width - fraction - 1)) - 1) << fraction
        edges = [0, sign, 1, 2, 3, normal - 1, normal, normal + 1,
                 one - 1, one, one + 1, one + 2, one + (1 << fraction),
                 infinity - 1, infinity, infinity | sign,
                 infinity + (1 << (fraction - 1)), 1 | sign, one | sign]
        fmt = lambda x: f'{x:0{width // 4}x}'
        # All special pairs, adjacent normal/subnormal, overflow and rounding
        # thresholds, with both signs and each arithmetic operation.
        pairs = [(a, b) for a in (0, sign, infinity, infinity | sign,
                                  infinity + (1 << (fraction - 1))) for b in edges]
        pairs += [(a, b) for a in edges for b in (one - 1, one + 1)]
        pairs += [(1, one + (1 << fraction)), (3, one + (1 << fraction)),
                  (normal * 2 - 1, one + (1 << fraction)),
                  (infinity - 1, one - 1), (infinity - 1, one + 1)]
        pairs += [(rng.getrandbits(width), rng.getrandbits(width)) for _ in range(128)]
        # Non-neat dynamic quotients with normal, subnormal and extreme scales.
        pairs += [(rng.randrange(1, normal * 2), rng.randrange(one, one + normal))
                  for _ in range(64)]
        for a, b in pairs:
            for op in ('add', 'sub', 'mul', 'div'):
                yield width, op, fmt(a), fmt(b)
        for a in edges:
            yield width, 'neg', fmt(a), fmt(0)
            for b in (0, one, infinity + (1 << (fraction - 1))):
                for op in ('eq', 'ne', 'lt', 'le', 'gt', 'ge'):
                    yield width, op, fmt(a), fmt(b)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--oracle', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--receipt', type=Path, required=True)
    args = p.parse_args()
    version = subprocess.run([str(args.oracle), '--version'], capture_output=True, text=True)
    assert version.returncode == 0 and version.stdout.strip() == '22.1.8', version
    rows = list(inputs())
    data = ''.join(' '.join(map(str, row)) + '\n' for row in rows)
    ran = subprocess.run([str(args.oracle)], input=data, capture_output=True, text=True)
    args.receipt.write_text(json.dumps(dict(argv=[str(args.oracle)], returncode=ran.returncode,
        stderr=ran.stderr, version=version.stdout.strip(), inputs=len(rows)), indent=2) + '\n')
    assert ran.returncode == 0 and not ran.stderr, ran
    expected = ran.stdout.splitlines()
    assert len(expected) == len(rows)
    args.out.write_text('# LLVM 22.1.8 APFloat rmNearestTiesToEven; seed=20261104\n'
        '# width op left right class bits-or-bool sign\n' + ''.join(
            '\t'.join(map(str, row)) + '\t' + value + '\n'
            for row, value in zip(rows, expected)))


if __name__ == '__main__':
    main()
