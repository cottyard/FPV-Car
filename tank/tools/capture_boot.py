import serial, time

PORT = "COM4"
BAUD = 115200
DURATION = 25  # seconds of capture

ser = serial.Serial(PORT, BAUD, timeout=1)
# Normal-boot reset: pulse EN (RTS) low briefly, keep GPIO0 (DTR) high.
ser.setDTR(False)
ser.setRTS(True)   # EN low
time.sleep(0.12)
ser.setRTS(False)  # EN high -> normal boot
time.sleep(0.2)

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
print("\n=== capture done ===")
