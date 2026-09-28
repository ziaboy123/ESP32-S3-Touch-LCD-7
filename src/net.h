// Everything that talks to the outside world — Wi-Fi and Arc's /panel API —
// runs on its own FreeRTOS task on core 0, so a slow request never stalls
// the UI (LVGL stays on core 1 in loop()). The UI only ever sees immutable
// snapshots handed across under a mutex.
#pragma once

#include <Arduino.h>

#include <memory>
#include <vector>

struct PanelSignal {
  String name, value;
  bool ok;
};

struct PanelAlert {
  String id, text;
  bool acked;
};

struct PanelEvent {
  String ago, title, text, source;
  uint32_t color;
};

struct McLogLine {
  String time, kind, player, text;
};

struct NetDevice {
  String name, ssid;
  bool wired;
  int down, up;  // kbps
};

struct PanelAction {
  String id, label, group;
  bool confirm;
};

// One /panel/state response, parsed. Never mutated after it's published.
struct Snapshot {
  String name = "Arc";
  String firmware;  // MD5 of the build Arc holds for this panel
  String time, date, summary;
  bool ready = false, ok = true;
  int unacked = 0, quietSeconds = 0;
  int ownerHome = -1;  // 1 home, 0 away, -1 unknown
  std::vector<PanelAlert> alerts;
  std::vector<PanelSignal> homelab;
  bool mcConfigured = false, mcUp = false, mcTpsOk = true;
  int mcOnline = 0;
  std::vector<String> mcPlayers;
  std::vector<float> tps;
  std::vector<McLogLine> mcLog;  // newest first
  std::vector<String> devices;
  std::vector<PanelEvent> events;
  std::vector<PanelAction> actions;
  bool proxmoxConfigured = false;
  bool pcConfigured = false, pcOnline = false;  // the Wake-on-LAN PC, as the network sees it
  // The office TV (the Hisense itself) and its Fire TV Stick, via the backend.
  bool tvConfigured = false, tvMuted = false, fireConfigured = false;
  String tvState, tvSource, fireState, fireApp;
  String ps5State;  // "on" = awake, "off" = rest mode, "" = not set up
  int tvVolume = -1;  // percent, -1 unknown
  // Live counts from Arc's 30s Proxmox sense, for the home tile.
  int pxHostsUp = 0, pxHosts = 0, pxGuestsRunning = 0, pxGuests = 0, pxProblems = 0;
  // Network (UniFi): WAN status, live rates in kbps, ~30 min of history.
  bool netReady = false, netOnline = false;
  String netIsp, netUptime;
  int netLatency = -1, netDown = 0, netUp = 0, netDrops = 0, netDeviceCount = 0;
  std::vector<int> netHistDown, netHistUp;
  std::vector<NetDevice> netDevices;
};

namespace net {

using SnapshotPtr = std::shared_ptr<const Snapshot>;

enum class WifiState { NoCredentials, Connecting, Connected };

void begin();

// Latest snapshot (nullptr until the first successful fetch) and a counter
// that bumps every time a new one lands, so the UI can skip redraws.
SnapshotPtr latest();
uint32_t generation();

WifiState wifiState();
String wifiSsid();
// A fetch succeeded recently — false means Arc (or the network) is down.
bool arcReachable();

void requestRefresh();
// Downloading a new build from Arc (it restarts itself when done).
bool updatingFirmware();
void runAction(const String &id);
// The result of the last action, once — for the UI's toast.
bool takeResult(String &message, bool &ok);

// One-off GETs for the Minecraft sub-pages (Players, World) — fetched only
// while a page is open, never on the 5s state poll. One in flight at a time;
// a newer request replaces an unstarted one.
void fetchDetail(const String &path);
// The finished fetch, once: its path, HTTP status (<0 = network error) and body.
bool takeDetail(String &path, int &status, String &body);

void startScan();
// True once a scan requested by startScan() has finished.
bool scanDone(std::vector<String> &ssids);
void saveWifi(const String &ssid, const String &password);

}  // namespace net
