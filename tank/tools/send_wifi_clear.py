import serial, time

PORT = "COM4"
BAUD = 115200
DURATION = 40  # seconds to capture after send

ser = serial.Serial(PORT, BAUD, timeout=0.3)
# Open without resetting the chip (do not pulse EN).
ser.dtr = False
ser.rts = False
time.sleep(0.5)
ser.reset_input_buffer()

# Send the wifi-clear command over the serial console.
ser.write(b"wifi clear\n")
ser.flush()
print("[i] sent 'wifi clear'")

start = time.time()
buffer = ""
count = 0
while time.time() - start < DURATION:
    data = ser.read(4096)
    if data:
        count += len(data)
        buffer += data.decode("utf-8", errors="replace")
        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            print(line.rstrip())
    else:
        time.sleep(0.05)
ser.close()
print("[i] total bytes read: %d" % count)
print("=== done ===")