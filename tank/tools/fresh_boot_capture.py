import serial, time, sys

PORT = "COM4"
BAUD = 115200
DURATION = 35  # seconds of capture

ser = serial.Serial(PORT, BAUD, timeout=1)

# 1) Flush any leftover data from a previous session (e.g. user plugged in USB).
ser.reset_input_buffer()
ser.reset_output_buffer()
time.sleep(0.5)
print("[i] buffer flushed, waiting 2s of silence...")
start = time.time()
silent = True
while time.time() - start < 2:
    if ser.read(4096):
        silent = False
print("[i] leftover data present" if not silent else "[i] buffer clean")

# 2) Real reset. Try EN-on-DTR first, then EN-on-RTS.
def attempt_reset():
    # Attempt A: pulse DTR low (some boards map EN to DTR)
    ser.setDTR(True)
    time.sleep(0.1)
    ser.setDTR(False)
    time.sleep(0.2)
    ser.reset_input_buffer()
    got = ser.read(4096)
    if got:
        return b"DTR-reset", got
    # Attempt B: pulse RTS low (EN on RTS)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.2)
    ser.reset_input_buffer()
    got = ser.read(4096)
    if got:
        return b"RTS-reset", got
    return b"no-reset", b""

method, first = attempt_reset()
print(f"[i] reset method: {method.decode()}")
if first:
    sys.stdout.write(first.decode("utf-8", errors="replace"))

# 3) Capture for DURATION seconds.
start = time.time()
buffer = first.decode("utf-8", errors="replace") if first else ""
while time.time() - start < DURATION:
    data = ser.read(4096)
    if data:
        buffer += data.decode("utf-8", errors="replace")
        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            print(line.rstrip())
    else:
        time.sleep(0.05)
ser.close()
print("\n=== capture done ===")
