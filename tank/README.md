# ESP32 L9110 Tank Firmware

Independent firmware for the ESP32-D0WD-V3 on `COM4`. The original ESP32-S3 camera project is unchanged; the tank firmware lives in `tank-esp32`.

## Wiring

| L9110 input | ESP32 GPIO |
| --- | --- |
| Left track forward (A1) | GPIO 16 |
| Left track reverse (A2) | GPIO 17 |
| Right track forward (B1) | GPIO 15 |
| Right track reverse (B2) | GPIO 14 |
| GND | GND |

Pins are the compile-time defaults in `include/pins.h`, but can be changed at runtime (stored in NVS) via the web page or `/api/pins`. Share GND between ESP32 and driver, and connect the L9110 motor supply to its own power source.

GPIO 1/3 are UART0 TX/RX; they were used for motors before and got driven by the ROM bootloader in download mode, spinning the motors despite the firmware not running. Motor pins are now 16/17/15/14 to avoid this.

## Use

1. On boot the tank joins the Wi-Fi network stored in NVS (first boot default: `Redmi_0DAC` / `16716811`) as a STA on the same network as the PC.
2. On the PC open `tank\tank.html`, enter `http://esp32-tank.local` and press CONNECT.
3. Drive with the pad or keyboard: `↑`/`↓`/`←`/`→`, `Space` to stop, drag the speed slider. Independently `Q`/`A` = left track fwd/rev, `W`/`S` = right track fwd/rev.
4. Motors stop on key/touch release, browser focus loss, or a 350 ms command watchdog timeout.

The firmware serves only a light control API; the UI lives on the PC.

### Changing Wi-Fi without reflashing (stored in NVS, namespace `tank-wifi`)

- **Web:** enter SSID + password in the Wi-Fi settings card of `tank\tank.html`, click 保存并重启.
- **Serial:** `wifi config` (SSID then password), `wifi status`, `wifi clear`.
- **AP fallback:** if the stored network is unreachable within 20 s, the tank starts AP `Tank-Control` (password `tankdrive`) at `http://192.168.4.1/`.

### Web API (CORS enabled)

| Endpoint | Description |
| --- | --- |
| `GET /api/control?cmd=drive&left=f\|r\|0&right=f\|r\|0&speed=N` | Drive each track |
| `GET /api/control?cmd=stop` | Stop |
| `GET /api/status` | Speed, motion, wifi_mode, ssid, ip, rssi |
| `GET /api/wifi?ssid=&password=` | Save Wi-Fi and reboot |
| `GET /api/pins` | Read current motor GPIO mapping |
| `GET /api/pins?lf=&lr=&rf=&rr=` | Set motor GPIO mapping (4 distinct usable GPIOs) and reboot |

Motor and Wi-Fi settings are both stored in NVS and changed without reflashing.

## Build and upload

```
py -m platformio run -t upload
```

Global PlatformIO has Arduino framework support; `toolchain-xtensa-esp32` needed a `.piopm` file with `owner: espressif` in its package dir to avoid a stalled download. Use the local proxy for downloads if reachability is an issue (`HTTP_PROXY`/`HTTPS_PROXY = http://127.0.0.1:10808`).

## Remaining

With motors connected, use `tank\tank.html` (or the tank page at `http://esp32-tank.local/`) and its four 200 ms test buttons to confirm the L9110 input order, save the mapping, and check direction at the lowest speed before driving.