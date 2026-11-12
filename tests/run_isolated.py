#!/usr/bin/env python3
"""Give a CTest driver a fresh --out directory and retain its report."""
from pathlib import Path
import subprocess
import sys
import tempfile


base = Path(sys.argv[1])
base.mkdir(parents=True, exist_ok=True)
out = Path(tempfile.mkdtemp(prefix='run-', dir=base)) / 'results'
print('Test results:', out, flush=True)
raise SystemExit(subprocess.call([sys.executable, *sys.argv[2:], '--out', str(out)]))
