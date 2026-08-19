#pragma once

// Initial L9110 wiring. The web page can remap these GPIOs without reflashing.
// GPIO 1/3 are UART0 TX/RX and get driven by the ROM bootloader in download mode,
// so motor inputs were moved off them. A1/A2 (left track) -> GPIO 16/17,
// B1/B2 (right track) -> GPIO 15/14.
constexpr uint8_t LEFT_FORWARD_PIN = 16;
constexpr uint8_t LEFT_REVERSE_PIN = 17;
constexpr uint8_t RIGHT_FORWARD_PIN = 15;
constexpr uint8_t RIGHT_REVERSE_PIN = 14;

// AP fallback portal, started when the stored Wi-Fi network is unreachable.
constexpr char AP_SSID[] = "Tank-Control";
constexpr char AP_PASSWORD[] = "tankdrive";
