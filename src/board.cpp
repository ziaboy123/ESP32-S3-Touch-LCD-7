#include "board.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <lvgl.h>

namespace board {
namespace {

// --- I2C bus shared by the CH422G expander and the GT911 touch controller.
constexpr int kSda = 8;
constexpr int kScl = 9;
constexpr int kTouchInt = 4;

// CH422G: not a register-addressed chip — each 7-bit "address" is a command.
constexpr uint8_t kChMode = 0x24;    // system parameters (bit0 = IO pins are outputs)
constexpr uint8_t kChOutput = 0x38;  // write EXIO0-7 levels
constexpr uint8_t kExTouchReset = 1 << 1;
constexpr uint8_t kExBacklight = 1 << 2;
constexpr uint8_t kExLcdReset = 1 << 3;
// EXIO5 is USB_SEL: high hands GPIO19/20 to the CAN transceiver and the
// native USB port vanishes (found the hard way — Waveshare's reference init
// sets it high). Held low so USB flashing/logging keeps working.

constexpr uint8_t kGt911 = 0x5D;  // the address selected by holding INT low through reset

esp_lcd_panel_handle_t panel = nullptr;
uint8_t expander = 0;

void writeExpander(uint8_t value) {
  Wire.beginTransmission(kChOutput);
  Wire.write(value);
  Wire.endTransmission();
  expander = value;
}

void resetExpanderAndTouch() {
  Wire.beginTransmission(kChMode);
  Wire.write(0x01);
  Wire.endTransmission();

  // Waveshare's reference sequence: touch held in reset with INT low picks
  // GT911 address 0x5D, then released. Backlight stays off until the panel
  // is actually drawing, so power-on doesn't flash garbage.
  pinMode(kTouchInt, OUTPUT);
  digitalWrite(kTouchInt, LOW);
  writeExpander(kExLcdReset);
  delay(100);
  writeExpander(kExLcdReset | kExTouchReset);
  delay(200);
  pinMode(kTouchInt, INPUT);
}

bool gt911Read(uint16_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(kGt911);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(kGt911, (uint8_t)len) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

void gt911Write(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(kGt911);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(value);
  Wire.endTransmission();
}

bool createPanel() {
  esp_lcd_rgb_panel_config_t config = {};
  config.clk_src = LCD_CLK_SRC_DEFAULT;
  // 14MHz (~34fps) rather than Waveshare's 16MHz: less PSRAM bandwidth for
  // the bounce buffers to keep up with, fewer underruns (see createPanel).
  config.timings.pclk_hz = 14 * 1000 * 1000;
  config.timings.h_res = kWidth;
  config.timings.v_res = kHeight;
  config.timings.hsync_pulse_width = 4;
  config.timings.hsync_back_porch = 8;
  config.timings.hsync_front_porch = 8;
  config.timings.vsync_pulse_width = 4;
  config.timings.vsync_back_porch = 8;
  config.timings.vsync_front_porch = 8;
  config.timings.flags.pclk_active_neg = 1;
  config.data_width = 16;
  config.bits_per_pixel = 16;
  config.num_fbs = 1;
  // Streams the PSRAM framebuffer through small SRAM buffers — the known
  // fix for this board's picture drifting while Wi-Fi is busy (PSRAM
  // bandwidth contention starving the LCD DMA).
  config.bounce_buffer_size_px = kWidth * 10;
  config.dma_burst_size = 64;
  config.hsync_gpio_num = 46;
  config.vsync_gpio_num = 3;
  config.de_gpio_num = 5;
  config.pclk_gpio_num = 7;
  config.disp_gpio_num = -1;
  // D0-D15 = B3-B7, G2-G7, R3-R7 (RGB565).
  const int data[16] = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};
  for (int i = 0; i < 16; i++) config.data_gpio_nums[i] = data[i];
  config.flags.fb_in_psram = 1;

  if (esp_lcd_new_rgb_panel(&config, &panel) != ESP_OK) return false;
  if (esp_lcd_panel_reset(panel) != ESP_OK) return false;
  return esp_lcd_panel_init(panel) == ESP_OK;
}

void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
  // Copies into the panel's framebuffer synchronously, so the LVGL buffer
  // is free again as soon as this returns.
  esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
  lv_display_flush_ready(display);
}

void readPointer(lv_indev_t *, lv_indev_data_t *data) {
  // A touch on a dark screen only wakes it — swallowed until the finger
  // lifts, so waking the panel can never also press whatever was under it.
  static bool swallowing = false;
  uint16_t x, y;
  bool touched = readTouch(x, y);
  if (touched && !backlightOn()) {
    setBacklight(true);
    swallowing = true;
  }
  if (swallowing) {
    lv_display_trigger_activity(nullptr);
    if (!touched) swallowing = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (touched) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

}  // namespace

bool readTouch(uint16_t &x, uint16_t &y) {
  // GT911 only flags *new* frames (bit 7 of the status register); between
  // frames keep reporting the last known contact, so a held finger doesn't
  // flicker between pressed and released.
  static bool down = false;
  static uint16_t lastX = 0, lastY = 0;
  uint8_t status;
  if (gt911Read(0x814E, &status, 1) && (status & 0x80)) {
    down = false;
    if ((status & 0x0F) > 0) {
      uint8_t point[8];
      if (gt911Read(0x814F, point, sizeof point)) {
        uint16_t px = point[1] | (point[2] << 8);
        uint16_t py = point[3] | (point[4] << 8);
        if (px < kWidth && py < kHeight) {
          lastX = px;
          lastY = py;
          down = true;
        }
      }
    }
    gt911Write(0x814E, 0);  // hand the buffer back to the controller
  }
  x = lastX;
  y = lastY;
  return down;
}

void setBacklight(bool on) {
  writeExpander(on ? (expander | kExBacklight) : (expander & ~kExBacklight));
}

bool backlightOn() { return expander & kExBacklight; }

bool begin() {
  Wire.begin(kSda, kScl, 400000);
  resetExpanderAndTouch();

  if (!createPanel()) {
    Serial.println("[board] RGB panel init failed");
    return false;
  }

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t *display = lv_display_create(kWidth, kHeight);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  // LVGL renders into *internal* RAM. Rendering into PSRAM (as this did
  // at first) doubled PSRAM traffic on every redraw, starving the LCD's
  // bounce buffers: the panel then re-shows the previous 10-line strip,
  // which looked like broken dashes under text (found live, 2026-09-27).
  // One buffer is enough: flush() copies synchronously anyway.
  size_t bufferBytes = kWidth * 20 * 2;
  void *buffer = heap_caps_malloc(bufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buffer) {
    Serial.println("[board] no internal RAM for the draw buffer, falling back to PSRAM");
    bufferBytes = kWidth * 80 * 2;
    buffer = heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM);
  }
  lv_display_set_buffers(display, buffer, nullptr, bufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, flush);

  lv_indev_t *touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, readPointer);

  uint8_t id[4] = {};
  gt911Read(0x8140, id, 4);
  Serial.printf("[board] panel up, touch id '%c%c%c%c'\n", id[0], id[1], id[2], id[3]);
  return true;
}

}  // namespace board
