# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Junesgone

#!/usr/bin/env python3
"""Reset the board, wait until it is ready, send one console line, keep listening.

    python tools/serial_send.py "enroll 主人" COM5 110

Commands like "enroll <name>" only work once the firmware is past its boot
sequence, and the boot log is long enough to swallow anything sent too early -
so this resets the board first and waits for the ready marker.
"""
import sys
import time

import serial

text = sys.argv[1] if len(sys.argv) > 1 else ""
port = sys.argv[2] if len(sys.argv) > 2 else "COM5"
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
marker = sys.argv[4] if len(sys.argv) > 4 else "listening..."

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()

# Pulse EN to get a deterministic boot, then wait for the ready marker.
ser.setRTS(True)
time.sleep(0.15)
ser.setRTS(False)
ser.reset_input_buffer()

deadline = time.time() + 40.0
ready = False
end = time.time() + seconds + 40.0
while time.time() < deadline and not ready:
    line = ser.readline()
    if not line:
        continue
    s = line.decode("utf-8", "replace").rstrip("\r\n")
    if s:
        print(s, flush=True)
    if marker in s:
        ready = True

if not ready:
    print("[warn] never saw %r; sending anyway" % marker, flush=True)
elif text:
    ser.write((text + "\n").encode("utf-8"))
    ser.flush()
    print("[sent] " + text, flush=True)
    end = time.time() + seconds

while time.time() < end:
    line = ser.readline()
    if not line:
        continue
    s = line.decode("utf-8", "replace").rstrip("\r\n")
    if s:
        print(s, flush=True)
ser.close()
