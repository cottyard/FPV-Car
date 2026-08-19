#pragma once

// Default L9110 wiring. Must avoid UART0 TX/RX (GPIO 1/3, which the ROM
// bootloader drives in download mode) and strapping pins (0/2/4/5/12/15,
// which can mis-link at boot). These four are safe output GPIOs on classic
// ESP32. The web page (/api/pins) can remap them without reflashing.
// left fwd/rev -> GPIO 16/17; right fwd/rev -> GPIO 25/26.
constexpr uint8_t LEFT_FORWARD_PIN = 16;
constexpr uint8_t LEFT_REVERSE_PIN = 17;
constexpr uint8_t RIGHT_FORWARD_PIN = 25;
constexpr uint8_t RIGHT_REVERSE_PIN = 26;

// AP fallback portal, started when the stored Wi-Fi network is unreachable.
constexpr char AP_SSID[] = "Tank-Control";
constexpr char AP_PASSWORD[] = "tankdrive";
