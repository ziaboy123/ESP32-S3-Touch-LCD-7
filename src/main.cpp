// Zia's Panel — a wall-mounted homelab dashboard for the Waveshare
// ESP32-S3-Touch-LCD-7: status, homelab, Proxmox, network, Minecraft and a
// fixed set of buttons, over a small LAN-only HTTP API. See README.md.
#include <Arduino.h>
#include <lvgl.h>

#include "board.h"
#include "net.h"
#include "ui.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("[arc-panel] booting");
  if (!board::begin()) {
    for (;;) delay(1000);
  }
  Serial.println("[arc-panel] board up");
  net::begin();
  Serial.println("[arc-panel] net started");
  ui::begin();
  Serial.println("[arc-panel] ui built");
  lv_timer_handler();  // draw once before the backlight comes up
  board::setBacklight(true);
  Serial.println("[arc-panel] ready");
}

void loop() {
  // Heartbeat on the USB log — the native USB port stays readable without
  // resetting the board as long as the monitor opens it with DTR on and RTS
  // off (macOS's default open toggles both and drops it into download mode).
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 5000) {
    lastBeat = millis();
    Serial.printf("[arc-panel] up %lus, wifi %d, arc %s, heap %u, psram %u\n", millis() / 1000,
                  (int)net::wifiState(), net::arcReachable() ? "ok" : "unreachable",
                  ESP.getFreeHeap(), ESP.getFreePsram());
  }
  ui::update();
  lv_timer_handler();
  delay(5);
}
