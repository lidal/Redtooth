"""Make a copy of the ESP32-C6 BLE controller library that accepts a 5 ms
connection interval, as the Switch 2 uses.

usage: patch_ble_lib.py --ar AR --objdump OBJDUMP SOURCE.a OUTPUT.a

The controller checks a peripheral connection's interval in
r_ble_ll_conn_slave_start (in ble_ll_conn.c.o) with `addi a5,a4,-6`, i.e.
interval - 6 units of 1.25 ms (7.5 ms, the Bluetooth minimum). Changing it to
`addi a5,a4,-4` lowers the minimum to 4 units (5 ms). This is the patch from
ESP32-BLE5-NSController-Emulator (MIT); here it is applied to a copy, and only
if the instruction occurs exactly once and inside that function.

Run by cmake/ble_interval_patch.cmake at configure time.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

OBJECT = "ble_ll_conn.c.o"
FUNCTION = "r_ble_ll_conn_slave_start"
ORIGINAL = bytes.fromhex("9307a7ff")  # addi a5,a4,-6 (little endian)
PATCHED = bytes.fromhex("9307c7ff")   # addi a5,a4,-4


def run(*cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True).stdout


def function_has(objdump, obj, function, needle):
    """Whether `function`'s disassembly contains an instruction matching `needle`."""
    inside = False
    for line in run(objdump, "-d", obj).splitlines():
        if line.endswith(f"<{function}>:"):
            inside = True
        elif inside and re.match(r"^[0-9a-f]+ <", line):
            break
        elif inside and re.search(needle, line):
            return True
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ar", required=True)
    parser.add_argument("--objdump", default="")
    parser.add_argument("source")
    parser.add_argument("output")
    args = parser.parse_args()

    objdump = args.objdump
    if not objdump:  # CMake didn't find it: it sits next to ar
        objdump = re.sub(r"-ar(\.exe)?$", r"-objdump\1", args.ar)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    shutil.copyfile(args.source, args.output)

    with tempfile.TemporaryDirectory() as tmp:
        run(args.ar, "x", os.path.abspath(args.output), OBJECT, cwd=tmp)
        obj = os.path.join(tmp, OBJECT)
        with open(obj, "rb") as f:
            data = f.read()

        found = data.count(ORIGINAL)
        if found != 1:
            already = data.count(PATCHED)
            sys.exit(f"expected exactly one 'addi a5,a4,-6' in {OBJECT}, found {found} "
                     f"({already} 'addi a5,a4,-4'); this library version is not supported")
        if not function_has(objdump, obj, FUNCTION, r"addi\s+a5,a4,-6"):
            sys.exit(f"'addi a5,a4,-6' is not in {FUNCTION}; this library version is not supported")

        with open(obj, "wb") as f:
            f.write(data.replace(ORIGINAL, PATCHED))
        if not function_has(objdump, obj, FUNCTION, r"addi\s+a5,a4,-4"):
            sys.exit("the patched instruction did not end up in " + FUNCTION)

        run(args.ar, "r", os.path.abspath(args.output), obj)

    print(f"BLE controller library patched for 5 ms connection intervals: {args.output}")


if __name__ == "__main__":
    main()
