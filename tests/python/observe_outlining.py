"""Reuse the owner observer and count real Vec iteration begin/end calls."""
import contextlib
import io
import json
from pathlib import Path
import runpy
import sys

sys.dont_write_bytecode = True
directory = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(directory))
import langrt as rt

counts = dict(begin=0, end=0)
original_begin, original_end = rt.Vec.begin, rt.Vec.end


def begin(value):
    counts['begin'] += 1
    return original_begin(value)


def end(value):
    counts['end'] += 1
    return original_end(value)


rt.Vec.begin, rt.Vec.end = begin, end
output = io.StringIO()
with contextlib.redirect_stdout(output):
    runpy.run_path(str(Path(__file__).with_name('observe_ownership.py')), run_name='__main__')
row = json.loads(output.getvalue())
row['iterations'] = counts
print(json.dumps(row))
