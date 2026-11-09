#!/usr/bin/env python3
"""Check list inference and execute the same program on both backends."""
import argparse
import json
from pathlib import Path

import run_unbound_methods as common


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'root', 'python', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--library', type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = args.root / 'tests/generics/list-inference/lang.toml'
    before = common.source_hashes(manifest.parent)
    report = dict(task='list inference', status='failed', runs=[],
                  source_sha256=before)
    try:
        common.simple_case(args, args.library, 'positive', manifest, before, report)
        assert common.source_hashes(manifest.parent) == before
        report['status'] = 'passed'
    finally:
        (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], records=len(report['runs']))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
