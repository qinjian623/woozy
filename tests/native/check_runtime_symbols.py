#!/usr/bin/env python3
"""Reject hosted-library references hidden by C++ host test executables."""
import argparse
import json
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--archive', required=True)
parser.add_argument('--nm', default='nm')
args = parser.parse_args()


def symbols(mode):
    result = subprocess.run([args.nm, mode, '--extern-only', '--format=posix', args.archive],
                            capture_output=True, text=True, check=True)
    return {(parts[0], parts[1]) for line in result.stdout.splitlines()
            if len(parts := line.split()) >= 2 and len(parts[1]) == 1}


defined = {name for name, kind in symbols('--defined-only')}
# ELF x86_64 linkers supply this symbol when resolving position-independent code.
linker_defined = {('_GLOBAL_OFFSET_TABLE_', 'U')}
missing = {(name, kind) for name, kind in symbols('--undefined-only')
           if name not in defined} - {('main', 'U'), ('lang_vec_observe_v1', 'w')} - linker_defined
assert not missing, f'Native runtime depends on external symbols: {sorted(missing)}'
print(json.dumps(dict(status='passed', unresolved=['main', 'lang_vec_observe_v1'])))
