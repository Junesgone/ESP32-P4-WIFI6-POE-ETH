"""Reset the board, wait until the voice logger is listening, send one console
line and print everything that comes back - including the DeepSeek reply.

Usage: python serial_ask.py "your question" [port] [reply_wait_s]
"""
import sys, time, serial

text = sys.argv[1] if len(sys.argv) > 1 else "hello"
port = sys.argv[2] if len(sys.argv) > 2 else "COM5"
reply_wait = float(sys.argv[3]) if len(sys.argv) > 3 else 45.0

s = serial.Serial(port, 115200, timeout=0.2)
s.setDTR(False)
s.setRTS(True)
time.sleep(0.2)
s.reset_input_buffer()
s.setRTS(False)

seen = b""

def drain(seconds, stop_marker=None):
    global seen
    end = time.time() + seconds
    while time.time() < end:
        chunk = s.read(4096)
        if not chunk:
            continue
        seen += chunk
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
        if stop_marker and stop_marker in seen:
            return True
    return False

print("--- waiting for the board to be ready ---", flush=True)
ready = False
deadline = time.time() + 45
while time.time() < deadline and not ready:
    drain(1.0)
    if b"listening..." in seen or b"type a line + Enter" in seen:
        ready = True

if not ready:
    print("", flush=True)
    print("!! board did not report ready in time", flush=True)
    s.close()
    sys.exit(1)

drain(3.0)          # let boot noise settle
seen = b""

print("", flush=True)
print("--- sending: " + text + " ---", flush=True)
s.write((text + chr(13) + chr(10)).encode("utf-8"))
s.flush()
drain(reply_wait, stop_marker=b"<<")
drain(2.0)
s.close()