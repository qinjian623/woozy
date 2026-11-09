"""Count actual owning borrow starts and matching ends, then reuse ARC checks."""

import contextlib
import io
import json
from pathlib import Path
import runpy
import sys

sys.dont_write_bytecode = True
out = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(out))
import langrt as rt

original_begin, original_end = rt.Frame.borrow, rt.Frame.end_borrow
active, calls = set(), []


def begin(frame, key):
    original_begin(frame, key)
    active.add((id(frame), key))
    calls.append(["begin", list(key)])


def end(frame, key):
    identity = (id(frame), key)
    if identity in active:
        active.remove(identity)
        calls.append(["end", list(key)])
    original_end(frame, key)


rt.Frame.borrow, rt.Frame.end_borrow = begin, end
output = io.StringIO()
with contextlib.redirect_stdout(output):
    runpy.run_path(
        str(Path(__file__).parent.parent / "python/observe_ownership.py"), run_name="__main__"
    )
result = json.loads(output.getvalue())
result["borrow_calls"] = calls
result["active_borrows"] = len(active)
print(json.dumps(result))
