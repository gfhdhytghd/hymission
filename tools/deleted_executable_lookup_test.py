#!/usr/bin/env python3
"""Exercise native-style lookup after unlinking a disposable executable copy."""
import pathlib
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="hymission-lookup-") as directory:
    executable = pathlib.Path(directory) / "lookup-test"
    shutil.copy2(sys.argv[1], executable)
    subprocess.run([str(executable), "--deleted-executable"], check=True)
