import urllib.request as u, json, time

base = "http://esp32-tank.local/api/status"
prev = None
t0 = time.time()
while time.time() - t0 < 40:
    try:
        d = json.load(u.urlopen(base, timeout=2))
        cur = (d["uptime_s"], d["reboots"], d["wifi_drops"], d["rssi"])
        if cur != prev:
            print("t=%4.1fs uptime_s=%3d reboots=%d wifi_drops=%d rssi=%d"
                  % (time.time() - t0, cur[0], cur[1], cur[2], cur[3]))
            prev = cur
    except Exception as e:
        print("t=%4.1fs POLL-FAIL %r" % (time.time() - t0, e))
    time.sleep(0.2)
print("capture window ended")