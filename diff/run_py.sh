#!/bin/sh
# run_py.sh <script.py> [args...] — jalankan python host tanpa hardcode path
set -e
PYEXE=$(ls -d /c/Users/Administrator/AppData/Local/hermes/tools/python-3.14.9*/python.exe 2>/dev/null | head -1)
if [ -z "$PYEXE" ]; then
  echo "PY_NOT_FOUND" >&2
  exit 127
fi
exec "$PYEXE" "$@"
