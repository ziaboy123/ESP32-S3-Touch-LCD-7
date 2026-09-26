// The dashboard: a status bar over four swipeable pages (Overview,
// Homelab, Minecraft, Controls), plus the on-device Wi-Fi setup screen.
// Runs entirely on the LVGL task (loop()); reads net's snapshots, never
// blocks on the network.
#pragma once

namespace ui {

void begin();
// Call every loop — cheap when nothing changed.
void update();

}  // namespace ui
