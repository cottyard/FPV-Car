import serial, time, sys, urllib.request as u

PORT = "COM4"
BAUD = 115200
SPEED = int(sys.argv[1]) if len(sys.argv) > 1 else 40
DRIVE_SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 6

ser = serial.Serial(PORT, BAUD, timeout=0.2)
ser.dtr = False
ser.rts = False
time.sleep(0.5)
ser.reset_input_buffer()

base = "http://esp32-tank.local/api/"

def hit(path, timeout=1.5):
    ok = False
    t0 = time.time()
    try:
        u.urlopen(base + path, timeout=timeout).read()
        ok = True
    except Exception:
        pass
    return ok, time.time() - t0

# assess baseline reachability before spinning
net_ok = net_bad = 0
for _ in range(10):
    if hit("status")[0]:
        net_ok += 1
    else:
        net_bad += 1
print("[i] baseline status polls: ok=%d bad=%d" % (net_ok, net_bad))
if net_bad > 0:
    print("[!] device not reachable; aborting")
    ser.close()
    sys.exit(1)

start = time.time()
next_send = time.time()
drive_end = start + DRIVE_SECONDS
buffer = ""
boots = 0
poll_bad = 0
poll_ok = 0
stopped = False

print("[i] engaging motors at speed %d for %.1fs..." % (SPEED, DRIVE_SECONDS))
while time.time() - drive_end < 3:
    now = time.time()
    # heartbeat drive while in the drive window
    if now < drive_end and now >= next_send:
        hit("control?cmd=drive&left=f&right=f&speed=%d" % SPEED, 1.0)
        next_send = now + 0.15
    # issue stop once the drive window ends (and only once)
    if now >= drive_end and not stopped:
        hit("control?cmd=stop", 1.0)
        stopped = True
    # frequent status poll to catch WiFi drops while driving
    ok, _ = hit("status", 1.0)
    if ok:
        poll_ok += 1
    else:
        poll_bad += 1
    # read serial
    data = ser.read(4096)
    if data:
        buffer += data.decode("utf-8", errors="replace")
        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            tl = line.rstrip()
            if tl:
                print(tl)
            if "ESP32 Tank - enter" in tl:
                boots += 1
    else:
        time.sleep(0.005)
ser.close()
print("[i] fresh-boot lines during drive+recovery: %d" % boots)
print("[i] status polls during drive window: ok=%d bad=%d" % (poll_ok, poll_bad))
print("=== done ===")