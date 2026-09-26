// Waveshare ESP32-S3-Touch-LCD-7 hardware: the 800x480 RGB panel, GT911
// touch, and the CH422G I/O expander that owns the backlight and reset
// lines. Everything here is board-specific; nothing above it knows pins.
#pragma once

#include <stdint.h>

namespace board {

constexpr int kWidth = 800;
constexpr int kHeight = 480;

// Brings up I2C, the expander, the RGB panel and touch, then LVGL on top.
// Returns false (and logs why) if the panel couldn't be created.
bool begin();

// Backlight is on/off only on this board — the expander pin isn't PWM.
void setBacklight(bool on);
bool backlightOn();

// Latest touch, polled by LVGL's input driver. Exposed for diagnostics.
bool readTouch(uint16_t &x, uint16_t &y);

}  // namespace board
