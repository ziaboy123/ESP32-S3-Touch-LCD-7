#include "net.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <WiFi.h>

#include "secrets.h"

namespace net {
namespace {

constexpr uint32_t kPollMs = 5000;
constexpr uint32_t kStaleMs = 20000;
constexpr uint32_t kHttpTimeoutMs = 8000;

SemaphoreHandle_t lock;
QueueHandle_t actionQueue;  // action ids, fixed-size char buffers
struct ActionId { char id[64]; };

SnapshotPtr current;
uint32_t gen = 0;
uint32_t lastSuccess = 0;
volatile bool refreshWanted = true;

String ssid, password;
volatile bool credentialsChanged = false;

bool resultReady = false;
String resultMessage;
bool resultOk = false;

volatile bool updating = false;

String detailWanted;  // path to fetch next, guarded by lock
bool detailReady = false;
String detailPath, detailBody;
int detailStatus = 0;

volatile bool scanWanted = false;
bool scanFinished = false;
std::vector<String> scanSsids;

struct Guard {
  Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(lock); }
};

uint32_t parseColor(const char *hex) {
  if (!hex || hex[0] != '#') return 0x95A5A6;
  return strtoul(hex + 1, nullptr, 16);
}

std::shared_ptr<Snapshot> parse(JsonDocument &doc) {
  auto s = std::make_shared<Snapshot>();
  s->name = doc["name"] | "Arc";
  s->time = doc["time"] | "";
  s->date = doc["date"] | "";
  s->summary = doc["summary"] | "";
  s->ready = doc["ready"] | false;
  s->ok = doc["ok"] | true;
  s->unacked = doc["unacked"] | 0;
  s->quietSeconds = doc["quiet"] | 0;
  s->firmware = doc["firmware"] | "";
  s->ownerHome = doc["owner_home"].isNull() ? -1 : (doc["owner_home"].as<bool>() ? 1 : 0);
  for (JsonObject a : doc["alerts"].as<JsonArray>())
    s->alerts.push_back({a["id"] | "", a["text"] | "", a["acked"] | false});
  for (JsonObject h : doc["homelab"].as<JsonArray>())
    s->homelab.push_back({h["name"] | "", h["value"] | "", h["ok"] | true});
  JsonObject mc = doc["minecraft"];
  s->mcConfigured = mc["configured"] | false;
  s->mcUp = mc["up"] | false;
  s->mcTpsOk = mc["tps_ok"] | true;
  s->mcOnline = mc["online"] | 0;
  for (const char *p : mc["players"].as<JsonArray>()) s->mcPlayers.push_back(p);
  for (float t : mc["tps"].as<JsonArray>()) s->tps.push_back(t);
  for (JsonObject l : mc["log"].as<JsonArray>())
    s->mcLog.push_back({l["time"] | "", l["kind"] | "", l["player"] | "", l["text"] | ""});
  for (const char *d : doc["network"]["devices"].as<JsonArray>()) s->devices.push_back(d);
  for (JsonObject e : doc["events"].as<JsonArray>())
    s->events.push_back({e["ago"] | "", e["title"] | "", e["text"] | "", e["source"] | "", parseColor(e["color"])});
  s->proxmoxConfigured = doc["proxmox"] | false;
  s->pcConfigured = doc["pc"]["configured"] | false;
  s->pcOnline = doc["pc"]["online"] | false;
  JsonObject tv = doc["tv"];
  s->tvConfigured = tv["configured"] | false;
  s->tvState = tv["state"] | "";
  s->tvSource = tv["source"] | "";
  s->tvVolume = tv["volume"] | -1;
  s->tvMuted = tv["muted"] | false;
  s->fireConfigured = tv["fire_configured"] | false;
  s->fireState = tv["fire_state"] | "";
  s->fireApp = tv["fire_app"] | "";
  s->ghConfigured = doc["github"]["configured"] | false;
  s->ghStreak = doc["github"]["streak"] | 0;
  s->ghToday = doc["github"]["today"] | 0;
  JsonObject px = doc["proxmox_summary"];
  s->pxHostsUp = px["hosts_up"] | 0;
  s->pxHosts = px["hosts"] | 0;
  s->pxGuestsRunning = px["guests_running"] | 0;
  s->pxGuests = px["guests"] | 0;
  s->pxProblems = px["problems"] | 0;
  JsonObject n = doc["net"];
  s->netReady = n["ready"] | false;
  s->netOnline = n["online"] | false;
  s->netIsp = n["isp"] | "";
  s->netUptime = n["uptime"] | "";
  s->netLatency = n["latency_ms"].isNull() ? -1 : (int)(n["latency_ms"] | 0);
  s->netDown = n["down_kbps"] | 0;
  s->netUp = n["up_kbps"] | 0;
  s->netDrops = n["drops"] | 0;
  s->netDeviceCount = n["device_count"] | 0;
  for (int v : n["history_down"].as<JsonArray>()) s->netHistDown.push_back(v);
  for (int v : n["history_up"].as<JsonArray>()) s->netHistUp.push_back(v);
  for (JsonObject d : n["devices"].as<JsonArray>())
    s->netDevices.push_back({d["name"] | "?", d["ssid"] | "", d["wired"] | false, d["down"] | 0, d["up"] | 0});
  for (JsonObject a : doc["actions"].as<JsonArray>())
    s->actions.push_back({a["id"] | "", a["label"] | "", a["group"] | "", a["confirm"] | false});
  return s;
}

bool fetchState() {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  if (!http.begin(client, String(ARC_URL) + "/panel/state")) return false;
  http.addHeader("Authorization", String("Bearer ") + ARC_PANEL_TOKEN);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[net] state: HTTP %d\n", code);
    http.end();
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[net] state: bad JSON (%s)\n", err.c_str());
    return false;
  }
  auto snapshot = parse(doc);
  Guard g;
  current = snapshot;
  gen++;
  lastSuccess = millis();
  return true;
}

void postAction(const char *id) {
  String message;
  bool ok = false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(30000);  // synchronous restarts can take a few seconds
  if (http.begin(client, String(ARC_URL) + "/panel/action/" + id)) {
    http.addHeader("Authorization", String("Bearer ") + ARC_PANEL_TOKEN);
    int code = http.POST("");
    if (code == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getStream())) {
        ok = doc["ok"] | false;
        message = doc["message"] | "";
      }
    } else {
      message = code > 0 ? "Arc said HTTP " + String(code) : "Couldn't reach Arc";
    }
    http.end();
  }
  Serial.printf("[net] action %s -> %s %s\n", id, ok ? "ok" : "failed", message.c_str());
  Guard g;
  resultReady = true;
  resultOk = ok;
  resultMessage = message;
}

// Self-update, pulled from Arc (see README, "Updates"). Arc advertises the MD5
// of the build it holds; when that differs from what's running, download
// it and restart. The restart is a software one, which matters on this
// board: a hardware reset while the panel is lit samples GPIO0 (shared
// with the LCD) low and lands in download mode.
//
// The advertised MD5 is remembered in NVS before restarting, so even if
// the running image's MD5 never quite matches the file's (padding), it's
// installed once and never again — no update loop.
void maybeUpdate(const String &advertised) {
  if (advertised.isEmpty() || advertised == ESP.getSketchMD5()) return;
  Preferences prefs;
  prefs.begin("firmware", true);
  String installed = prefs.getString("installed", "");
  prefs.end();
  static String attempted;  // a failed download isn't retried until reboot
  if (advertised == installed || advertised == attempted) return;
  attempted = advertised;

  Serial.printf("[net] firmware %s advertised, running %s — updating\n", advertised.c_str(),
                ESP.getSketchMD5().c_str());
  updating = true;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, String(ARC_URL) + "/panel/firmware");
  http.addHeader("Authorization", String("Bearer ") + ARC_PANEL_TOKEN);
  httpUpdate.rebootOnUpdate(false);
  t_httpUpdate_return result = httpUpdate.update(http);
  if (result == HTTP_UPDATE_OK) {
    prefs.begin("firmware", false);
    prefs.putString("installed", advertised);
    prefs.end();
    Serial.println("[net] firmware installed, restarting");
    delay(300);
    ESP.restart();
  }
  Serial.printf("[net] firmware update failed: %s\n", httpUpdate.getLastErrorString().c_str());
  updating = false;
}

void fetchWantedDetail() {
  String path;
  {
    Guard g;
    path = detailWanted;
    detailWanted = "";
  }
  if (path.isEmpty()) return;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(25000);  // Players reads five stats files through Beacon
  int status = -1;
  String body;
  if (http.begin(client, String(ARC_URL) + path)) {
    http.addHeader("Authorization", String("Bearer ") + ARC_PANEL_TOKEN);
    status = http.GET();
    if (status > 0) body = http.getString();
    http.end();
  }
  Serial.printf("[net] detail %s -> %d (%u bytes)\n", path.c_str(), status, body.length());
  Guard g;
  detailReady = true;
  detailPath = path;
  detailStatus = status;
  detailBody = body;
}

void loadCredentials() {
  Preferences prefs;
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", WIFI_SSID);
  password = prefs.getString("pass", WIFI_PASSWORD);
  prefs.end();
}

void connect() {
  if (ssid.isEmpty()) return;
  Serial.printf("[net] connecting to %s\n", ssid.c_str());
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), password.c_str());
}

void task(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("arc-panel");
  // Modem sleep adds latency to every poll and is known to worsen this
  // board's display drift; the panel is mains-powered anyway.
  WiFi.setSleep(false);
  loadCredentials();
  connect();
  uint32_t lastPoll = 0, lastReconnect = millis();
  for (;;) {
    if (credentialsChanged) {
      credentialsChanged = false;
      connect();
      lastReconnect = millis();
    }
    if (scanWanted) {
      scanWanted = false;
      int n = WiFi.scanNetworks();
      std::vector<String> found;
      for (int i = 0; i < n; i++) {
        String name = WiFi.SSID(i);
        if (name.length() && std::find(found.begin(), found.end(), name) == found.end()) found.push_back(name);
      }
      WiFi.scanDelete();
      Guard g;
      scanSsids = found;
      scanFinished = true;
    }
    if (WiFi.status() == WL_CONNECTED) {
      ActionId action;
      fetchWantedDetail();
      if (xQueueReceive(actionQueue, &action, 0) == pdTRUE) {
        postAction(action.id);
        refreshWanted = true;
      }
      if (refreshWanted || millis() - lastPoll >= kPollMs) {
        refreshWanted = false;
        lastPoll = millis();
        if (fetchState()) {
          SnapshotPtr snapshot = latest();
          if (snapshot) maybeUpdate(snapshot->firmware);
        }
      }
    } else if (!ssid.isEmpty() && millis() - lastReconnect > 15000) {
      lastReconnect = millis();
      connect();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

}  // namespace

void begin() {
  lock = xSemaphoreCreateMutex();
  actionQueue = xQueueCreate(4, sizeof(ActionId));
  xTaskCreatePinnedToCore(task, "net", 12288, nullptr, 1, nullptr, 0);
}

SnapshotPtr latest() {
  Guard g;
  return current;
}

uint32_t generation() {
  Guard g;
  return gen;
}

WifiState wifiState() {
  if (ssid.isEmpty()) return WifiState::NoCredentials;
  return WiFi.status() == WL_CONNECTED ? WifiState::Connected : WifiState::Connecting;
}

String wifiSsid() { return ssid; }

bool arcReachable() {
  Guard g;
  return lastSuccess && millis() - lastSuccess < kStaleMs;
}

void requestRefresh() { refreshWanted = true; }

bool updatingFirmware() { return updating; }

void runAction(const String &id) {
  ActionId action = {};
  strlcpy(action.id, id.c_str(), sizeof action.id);
  xQueueSend(actionQueue, &action, 0);
}

bool takeResult(String &message, bool &ok) {
  Guard g;
  if (!resultReady) return false;
  resultReady = false;
  message = resultMessage;
  ok = resultOk;
  return true;
}

void fetchDetail(const String &path) {
  Guard g;
  detailWanted = path;
}

bool takeDetail(String &path, int &status, String &body) {
  Guard g;
  if (!detailReady) return false;
  detailReady = false;
  path = detailPath;
  status = detailStatus;
  body = detailBody;
  detailBody = "";
  return true;
}

void startScan() {
  Guard g;
  scanFinished = false;
  scanWanted = true;
}

bool scanDone(std::vector<String> &ssids) {
  Guard g;
  if (!scanFinished) return false;
  ssids = scanSsids;
  return true;
}

void saveWifi(const String &newSsid, const String &newPassword) {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putString("ssid", newSsid);
  prefs.putString("pass", newPassword);
  prefs.end();
  ssid = newSsid;
  password = newPassword;
  credentialsChanged = true;
}

}  // namespace net
