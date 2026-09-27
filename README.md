# ESP32-S3-Touch-LCD-7 — Zia's Panel

Firmware that turns a **Waveshare ESP32-S3-Touch-LCD-7** (7" 800×480
capacitive touch screen with an ESP32-S3 on the back) into a wall-mounted
dashboard for a homelab. It runs directly on the chip: no OS, no browser,
just C++ on Arduino / ESP-IDF 5 with **LVGL 9** for the UI.

The panel is a thin client. It polls a small LAN-only HTTP API on my
homelab service (Arc), draws what it's told, and sends button presses back
as fixed action IDs. It never holds infrastructure credentials and can
never send an arbitrary command.

## What's on it

A home screen of six tiles, each with a live one-line status, each opening
a full-screen section with a Back button:

- **Overview** — one big status card (green / red / amber once
  acknowledged, tap to acknowledge) plus a live activity feed.
- **Homelab** — every health check: services, disk, certificates, and
  outside-in checks of the public sites.
- **Proxmox** — a card per host (CPU / RAM / disk bars, uptime) and every
  container and VM with its status and usage.
- **Network** — internet status, latency, live download/upload with a
  30-minute graph, and the busiest devices right now.
- **Minecraft** — server status, TPS, a live server log that scrolls
  upward like in-game chat, and quick actions. Sub-pages for **Players**
  (stats, leaderboard, full **inventories with real item icons**, armour,
  offhand, ender chest, custom names and enchantments) and **World**
  (in-game time, weather, game-rule switches, disk usage).
- **Controls** — four tiles of its own: **PC** (on/off state, Wake-on-LAN,
  and a confirm-first shut down that the backend performs), **Arc** (check
  now, acknowledge, quiet mode, briefing), **Restarts** (confirm-first
  service restarts) and **Panel** (Wi-Fi setup, screen off, firmware and
  address).

It drifts back to the home screen after a few minutes untouched, jumps to
Overview when a new alert arrives, and stays lit (it's on USB power) unless
turned off from Controls — then a touch only wakes it, never also pressing
whatever was under your finger.

## Layout

```
src/board.*        panel, touch (GT911), IO expander (CH422G), LVGL port
src/net.*          Wi-Fi + the backend API, on its own FreeRTOS task (core 0)
src/ui.*           every screen, on the LVGL task (core 1)
src/lv_mem_psram.c LVGL allocator that prefers PSRAM
include/lv_conf.h  LVGL config
```

## Building

PlatformIO, using the [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform (arduino-esp32 3.x / ESP-IDF 5, for the RGB panel driver with
bounce buffers).

1. `cp include/secrets.example.h include/secrets.h` and set the backend URL
   and token.
2. First flash over the **native USB** port: `pio run -e panel -t upload`.
3. Wi-Fi is set up on the panel itself: it scans, you pick a network and
   type the password on the on-screen keyboard. It's stored on the device.

## Updates

After the first flash, updates are **pulled**, not pushed:
`deploy-firmware.sh` builds and copies the image to the backend (configure
the destination in `deploy.env`, from `deploy.env.example`). The backend
advertises the build's MD5 in its state response; the panel downloads it
when it differs from what it's running, installs it and restarts. The MD5
is remembered on the device, so a mismatch can never cause an update loop.

Pushing with ArduinoOTA/espota was the first attempt. It needs the panel to
connect back to the computer doing the upload, which a desktop firewall
rightly blocks. It also matters that the update restarts the chip in
software, for the GPIO0 reason below.

## Backend API

All endpoints take `Authorization: Bearer <token>`.

| Endpoint | |
|---|---|
| `GET /panel/state` | everything the screens draw, polled every 5 s (ASCII-only, pre-formatted times) |
| `POST /panel/action/{id}` | press one button from the backend's fixed list |
| `GET /panel/firmware` | the current firmware image (self-update) |
| `GET /panel/proxmox` | hosts and guests, fetched only while that section is open |
| `GET /panel/minecraft/{players,world,inventory/{uuid}}` | Minecraft sub-pages; inventories include each item icon as a base64 PNG |

## Hardware gotchas (all found the hard way)

- **CH422G EXIO5 is USB_SEL.** Driving it high hands GPIO19/20 to the CAN
  transceiver and the native USB port vanishes. Waveshare's reference init
  drives it high; this firmware holds it low.
- **GPIO0 is both the boot strap pin and the LCD's G3 data line.** A reset
  while the panel is lit (the RESET button, or esptool's USB hard reset)
  samples it low and lands in download mode. Only a power cycle, or a
  software restart, boots cleanly — hence pulled updates that restart in
  software.
- **macOS resets the chip when you open its serial port** with default
  line settings, straight into download mode. Open it with DTR on and RTS
  off to read the log without touching the board.
- **Occasional "broken dashes" under text** were bounce-buffer underruns:
  when PSRAM is too busy to refill the LCD's bounce buffer in time, the
  panel re-shows the previous 10-line strip. Fixed by rendering into an
  internal-RAM draw buffer, a 14 MHz pixel clock, and only touching labels
  and styles when they actually change.
- **Internal RAM runs out quietly.** arduino-esp32 keeps every allocation
  under 4 KB in internal RAM, and an LVGL UI is thousands of small
  allocations. A custom LVGL allocator that prefers PSRAM took free
  internal RAM from ~30 KB to ~168 KB.
- **LVGL's bundled lodepng is patched:** `lodepng_decode32` hands back an
  `lv_draw_buf_t*`, not a raw pixel array. Reading it as raw pixels draws
  perfectly transparent icons.
- **`LV_LABEL_LONG_DOT` needs a real width.** With only a max-width, the
  label can collapse to nothing but "…".
