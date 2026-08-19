import serial, time

PORT = "COM4"
BAUD = 115200
DURATION = 20

def read_port(ser):
    try:
        data = ser.read(4096)
        return data
    except Exception as e:
        return b"<ERR: %r>" % e

ser = serial.Serial(PORT, BAUD, timeout=1)
ser.reset_input_buffer()
print("[i] reading without reset for %d s..." % DURATION)
start = time.time()
buffer = ""
count = 0
while time.time() - start < DURATION:
    data = read_port(ser)
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
print("=== capture done ===")
