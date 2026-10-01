#!/bin/sh
cd "$(dirname "$0")"
if ! command -v python3 >/dev/null 2>&1; then
  echo "Python 3.10 or later is required. Install Python 3, then reopen this installer."
  read -r -p "Press Return to close..."
  exit 1
fi
python3 flash.py "$@"
status=$?
if [ "$status" -ne 0 ]; then
  read -r -p "Press Return to close..."
fi
exit "$status"
