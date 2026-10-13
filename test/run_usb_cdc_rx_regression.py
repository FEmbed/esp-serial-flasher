#!/usr/bin/env python3
"""Compile real USB port + SLIP with deterministic SDK doubles. No hardware access."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", type=Path)
parser.add_argument("--compiler", default="clang")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
compiler = shutil.which(args.compiler)
if not compiler:
    raise SystemExit(f"C compiler not found: {args.compiler}")

def run(output):
    output.mkdir(parents=True, exist_ok=True)
    exe = output / ("usb-cdc-rx.exe" if __import__("os").name == "nt" else "usb-cdc-rx")
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DSERIAL_FLASHER_RESET_HOLD_TIME_MS=100",
               "-DSERIAL_FLASHER_BOOT_HOLD_TIME_MS=50", "-I", str(root / "test/usb_port_mocks"),
               "-I", str(root / "port"), "-I", str(root / "include"), "-I", str(root / "private_include"),
               str(root / "test/test_usb_cdc_rx.c"), str(root / "src/slip.c"), "-o", str(exe)]
    build = subprocess.run(command, capture_output=True, text=True)
    receipt = {"command": command, "build_exit": build.returncode, "build_stdout": build.stdout, "build_stderr": build.stderr}
    if build.returncode == 0:
        tested = subprocess.run([str(exe)], capture_output=True, text=True)
        receipt.update(test_exit=tested.returncode, test_stdout=tested.stdout, test_stderr=tested.stderr)
    (output / "result.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    if receipt.get("test_exit") != 0:
        print(json.dumps(receipt, indent=2))
        raise SystemExit(1)
    print(receipt["test_stdout"].strip())

if args.output_dir:
    run(args.output_dir.resolve())
else:
    with tempfile.TemporaryDirectory(prefix="usb-cdc-rx-") as directory:
        run(Path(directory))
