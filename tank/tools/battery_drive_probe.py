import urllib.request as u, json, time, sys

base = "http://esp32-tank.local/api/"
SPEED = int(sys.argv[1]) if len(sys.argv) > 1 else 60
DRIVE_S = float(sys.argv[2]) if len(sys.argv) > 2 else 10

def get(p, t=1.5):
    try:
        return True, json.load(u.urlopen(base + p, timeout=t))
    except Exception:
        return False, None

# ensure stable baseline
print("[i] warming up...")
time.sleep(3)
st = get("status")[1]
print("[i] baseline: reboots=%d wifi_drops=%d uptime=%d rssi=%d"
      % (st["reboots"], st["wifi_drops"], st["uptime_s"], st["rssi"]))

b0 = (st["reboots"], st["wifi_drops"], st["uptime_s"])
ok = bad = 0
t0 = time.time()
next_send = t0
drive_end = t0 + DRIVE_S
stopped = False
fails_at = []
while time.time() - drive_end < 4:
    now = time.time()
    # keep motors driven for the window (re-issue so timeout/regen keeps it moving)
    if now < drive_end and now >= next_send:
        get("control?cmd=drive&left=f&right=f&speed=%d" % SPEED, 1.0)
        next_send = now + 0.2
    if now >= drive_end and not stopped:
        get("control?cmd=stop", 1.0)
        stopped = True
    okk, _ = get("status", 1.0)
    if okk:
        ok += 1
    else:
        bad += 1
        fails_at.append(round(now - t0, 1))
time.sleep(1)
d = get("status")[1]
b1 = (d["reboots"], d["wifi_drops"], d["uptime_s"])
print("[i] drive window: status ok=%d bad=%d ; fails at=%s" % (ok, bad, fails_at))
print("[i] reboots %d->%d , wifi_drops %d->%d , uptime %d->%d"
      % (b0[0], b1[0], b0[1], b1[1], b0[2], b1[2]))
print("=== done ===")