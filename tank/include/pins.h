#pragma once

// Default motor wiring. Must avoid UART0 TX/RX (GPIO 1/3, which the ROM
// bootloader drives in download mode). The pins below are the four motor-driver
// inputs; the web page (/api/pins) can remap them without reflashing.
// left fwd/rev -> GPIO 14/15; right fwd/rev -> GPIO 12/13.
constexpr uint8_t LEFT_FORWARD_PIN = 14;
constexpr uint8_t LEFT_REVERSE_PIN = 15;
constexpr uint8_t RIGHT_FORWARD_PIN = 12;
constexpr uint8_t RIGHT_REVERSE_PIN = 13;

// AP fallback portal, started when the stored Wi-Fi network is unreachable.
constexpr char AP_SSID[] = "Tank-Control";
constexpr char AP_PASSWORD[] = "tankdrive";
