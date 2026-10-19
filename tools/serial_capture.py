# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Junesgone

"""Reset the board into run mode and capture its UART output.

Usage: py -3.14 serial_capture.py [port] [seconds] [baud]
"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

s = serial.Serial(port, baud, timeout=0.1)
# ESP32 auto-reset: DTR -> IO0, RTS -> EN.  IO0 high + EN pulse = normal boot.
s.setDTR(False)
s.setRTS(True)
time.sleep(0.2)
s.reset_input_buffer()
s.setRTS(False)
end = time.time() + secs
while time.time() < end:
    chunk = s.read(4096)
    if chunk:
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
s.close()
