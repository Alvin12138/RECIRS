#!/usr/bin/env python3
"""
OpenPLC CFI Operator CLI
Runtime Stage Layer 3: Operator Command Interface

Usage:
    ./cfi_operator.py status              # Get current CFI status
    ./cfi_operator.py check               # Manual integrity check
    ./cfi_operator.py events              # Print event log
    ./cfi_operator.py normal              # Set mode to NORMAL (needs integrity check)
    ./cfi_operator.py suspect             # Set mode to SUSPECT
    ./cfi_operator.py degraded            # Set mode to DEGRADED
    ./cfi_operator.py survival            # Set mode to SURVIVAL
    ./cfi_operator.py period N            # Set integrity period to N cycles
    ./cfi_operator.py stop                # Request safety stop
    ./cfi_operator.py clear               # Clear degrade state
"""

import socket
import struct
import sys

CFI_SOCK = "/tmp/openplc_cfi.sock"

CMD_MAP = {
    "status":   0,
    "events":   1,
    "check":    2,
    "normal":   3,
    "suspect":  4,
    "degraded": 5,
    "survival": 6,
    "period":   7,
    "stop":     8,
    "clear":    9,
}

def send_cmd(cmd_id, arg=0):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(CFI_SOCK)
    except (socket.error, FileNotFoundError) as e:
        print(f"[ERROR] Cannot connect to OpenPLC CFI server at {CFI_SOCK}: {e}")
        print("        Make sure OpenPLC is running with CFI_PROTECT enabled.")
        sys.exit(1)

    req = struct.pack("II", cmd_id, arg)
    sock.sendall(req)

    resp_data = sock.recv(260)
    sock.close()

    if len(resp_data) < 4:
        print("[ERROR] Invalid response from server")
        sys.exit(1)

    status = struct.unpack("i", resp_data[:4])[0]
    msg = resp_data[4:].decode('utf-8', errors='replace').rstrip('\x00').strip()
    return status, msg

def fmt_status(msg):
    """Pretty-print status response"""
    parts = msg.split()
    print("+================== CFI Runtime Status ==================+")
    for part in parts:
        if '=' in part:
            key, val = part.split('=', 1)
            print(f"  {key:20s}: {val}")
    print("+========================================================+")

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(0)

    cmd_name = sys.argv[1]
    if cmd_name not in CMD_MAP:
        print(f"[ERROR] Unknown command: {cmd_name}")
        print("Run without arguments for usage.")
        sys.exit(1)

    cmd_id = CMD_MAP[cmd_name]
    arg = 0

    if cmd_name == "period":
        if len(sys.argv) < 3:
            print("[ERROR] 'period' requires an argument: cfi_operator.py period N")
            sys.exit(1)
        try:
            arg = int(sys.argv[2])
        except ValueError:
            print("[ERROR] Period must be an integer")
            sys.exit(1)

    status, msg = send_cmd(cmd_id, arg)

    if cmd_name == "status":
        fmt_status(msg)
    elif status == 0:
        print(f"[OK] {msg}")
    elif status == 2:
        print(f"[BASELINE] {msg}")
    else:
        print(f"[FAIL] {msg}")

    sys.exit(0 if status >= 0 else 1)

if __name__ == "__main__":
    main()
