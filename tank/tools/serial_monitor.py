import serial, time, sys

PORT = "COM4"
BAUD = 115200
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 6

ser = serial.Serial(PORT, BAUD, timeout=0.2)
# Do NOT touch DTR/RTS: open without resetting the chip.
ser.dtr = False
ser.rts = False

start = time.time()
buffer = ""
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
print("=== monitor done ===")
