// Copy to secrets.h (gitignored) and fill in.
#pragma once

// The backend's panel API. Keep it on your LAN; never expose it publicly.
#define ARC_URL "http://192.168.1.50:5182"
// Bearer token; must match the backend's panel token.
#define ARC_PANEL_TOKEN ""

// Optional first-boot Wi-Fi. Leave blank to pick a network and type the
// password on the panel itself (stored on the device, not in this file).
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
