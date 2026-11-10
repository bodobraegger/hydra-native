#!/usr/bin/env python3
"""Checks the order of the portal calls of build/hydra-native-capture (src/capture_linux.c) with its dry run (no
dialog opens)."""
import pathlib
import subprocess
import sys

EXPECTED_CALLS = ["CreateSession", "SelectSources", "Start", "OpenPipeWireRemote"]

capture = pathlib.Path(__file__).resolve().parent.parent / "build" / "hydra-native-capture"
output = subprocess.run([str(capture), "picker", "640", "360", "--dry-run"], capture_output=True, text=True, check=True).stdout
calls = [line.split()[1] for line in output.splitlines() if line.startswith("call ")]
command = [line for line in output.splitlines() if line.startswith("run ")]
errors = []
if calls != EXPECTED_CALLS:
    errors.append(f"calls {calls}, expected {EXPECTED_CALLS}")
if "'types': <uint32 3>" not in output:
    errors.append("SelectSources does not offer monitors and windows")
if not command or "pipewiresrc fd=" not in command[0] or "path=42" not in command[0]:
    errors.append(f"pipeline {command}")
if errors:
    sys.exit("portal_test: " + "; ".join(errors) + "\n" + output)
print("portal_test: ok")
