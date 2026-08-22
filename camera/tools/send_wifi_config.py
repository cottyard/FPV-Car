import sys
import time

import serial

PORT = "COM3"
BAUD = 115200
DEFAULT_SSID = "fhjqr"
DEFAULT_PASSWORD = "12345678"
TIMEOUT_S = 20  # seconds to wait for each prompt


def wait_for(ser, needle, timeout_s=TIMEOUT_S):
    """Read serial until needle appears; return True if found."""
    buf = ""
    start = time.time()
    while time.time() - start < timeout_s:
        data = ser.read(1024)
        if data:
            text = data.decode("utf-8", errors="replace")
            buf += text
            print(text, end="")
            if needle in buf:
                return True
        else:
            time.sleep(0.05)
    print("\n[i] timed out waiting for: %r (got: %r)" % (needle, buf[-200:]))
    return False


def send(ser, line):
    ser.write((line + "\n").encode())
    ser.flush()
    print(">>> %s" % line)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else PORT
    ssid = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_SSID
    password = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_PASSWORD
    ser = serial.Serial(port, BAUD, timeout=0.3)
    # Open without resetting the chip (do not pulse EN).
    ser.dtr = False
    ser.rts = False
    time.sleep(0.5)
    ser.reset_input_buffer()
    print("==> adding WiFi %s on %s" % (ssid, port))

    send(ser, "wifi add")
    if not wait_for(ser, "Enter SSID:"):
        ser.close()
        return 1
    send(ser, ssid)
    if not wait_for(ser, "Enter password"):
        ser.close()
        return 1
    send(ser, password)
    # Device saves the network and restarts.
    wait_for(ser, "Restarting", 10)
    ser.close()
    print("==> done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
