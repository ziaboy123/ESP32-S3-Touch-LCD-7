#include "ui.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <WiFi.h>
#include <lvgl.h>

#include "board.h"
#include "net.h"

#include <map>

// LVGL's bundled lodepng (LV_USE_LODEPNG). Declared directly: its header
// exposes C++ overloads when included from C++ that clash with the C build.
// NB LVGL patches it: `*out` receives an lv_draw_buf_t* (pixels in its
// ->data, R,G,B,A byte order, row stride in its header), not a raw pixel
// array — reading it as raw pixels draws transparent garbage (found live).
extern "C" unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h, const unsigned char *in,
                                     size_t insize);

namespace ui {
namespace {

// --- Palette: near-black with a deep red (#b91c1c) as the accent.
// Status colours are deliberately brighter than the accent, so "alert"
// never reads as "just the brand colour".
constexpr uint32_t kBg = 0x0A0A0B;
constexpr uint32_t kCard = 0x161619;
constexpr uint32_t kBorder = 0x27272A;
constexpr uint32_t kButton = 0x232327;
constexpr uint32_t kText = 0xE7E7EA;
constexpr uint32_t kMuted = 0x8B8B94;
constexpr uint32_t kAccent = 0xB91C1C;
constexpr uint32_t kOk = 0x22C55E;
constexpr uint32_t kWarn = 0xF59E0B;
constexpr uint32_t kAlert = 0xEF4444;

constexpr int kTopBar = 52;
constexpr uint32_t kToastMs = 3500;
constexpr uint32_t kHomeAfterMs = 3 * 60 * 1000;  // untouched this long: drift back to the home tiles
constexpr uint32_t kProxmoxRefreshMs = 15000;

// The home screen's tiles, each opening one full-screen section.
enum Section { kOverview, kHomelab, kProxmox, kNetwork, kMinecraft, kControls, kSectionCount };
constexpr int kHome = -1;
int currentSection = kHome;
bool screenOffByHand = false;  // Screen off button pressed; see applyBacklight()

lv_obj_t *mainScreen, *wifiScreen;

// Status bar
lv_obj_t *topBar, *pill, *clockLabel, *wifiIcon;

// Overview
lv_obj_t *statusCard, *statusTitle, *statusSummary, *statusDetail;
lv_obj_t *statHomelab, *statMinecraft, *statDevices;
lv_obj_t *feedList;

// Homelab / Minecraft / Controls
lv_obj_t *homelabList;
lv_obj_t *mcState, *mcPlayers, *mcTps, *mcButtons, *mcLogList;
lv_obj_t *controlsPage;
lv_obj_t *quietLabel;

lv_obj_t *toast;
lv_timer_t *toastTimer;

// Wi-Fi setup
lv_obj_t *wifiList, *wifiChosen, *wifiPassword, *wifiCancel;
String chosenSsid;
bool scanning = false;

uint32_t seenGeneration = 0;
net::SnapshotPtr snap;
String homelabSig, feedSig, actionsSig, mcLogSig;

// --- Change-only setters -----------------------------------------------------
// Setting a label or style to the value it already has still makes LVGL
// redraw it; the top bar and tiles are refreshed constantly, so redrawing
// only on real changes keeps PSRAM traffic (and display glitches) down.

void setText(lv_obj_t *label, const char *value) {
  if (strcmp(lv_label_get_text(label), value) != 0) lv_label_set_text(label, value);
}

void setTextColor(lv_obj_t *obj, uint32_t color) {
  if (lv_color_to_u32(lv_obj_get_style_text_color(obj, LV_PART_MAIN)) != (lv_color_to_u32(lv_color_hex(color))))
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

void setBgColor(lv_obj_t *obj, uint32_t color) {
  if (lv_color_to_u32(lv_obj_get_style_bg_color(obj, LV_PART_MAIN)) != lv_color_to_u32(lv_color_hex(color)))
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
}

void setBorder(lv_obj_t *obj, uint32_t color, int width) {
  if (lv_color_to_u32(lv_obj_get_style_border_color(obj, LV_PART_MAIN)) != lv_color_to_u32(lv_color_hex(color)))
    lv_obj_set_style_border_color(obj, lv_color_hex(color), 0);
  if (lv_obj_get_style_border_width(obj, LV_PART_MAIN) != width) lv_obj_set_style_border_width(obj, width, 0);
}

// --- Small builders --------------------------------------------------------

lv_obj_t *box(lv_obj_t *parent, int w, int h, uint32_t bg = kCard) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(kBorder), 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_radius(o, 14, 0);
  lv_obj_set_style_pad_all(o, 14, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t *bare(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *value = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, value);
  return l;
}

lv_obj_t *dot(lv_obj_t *parent, uint32_t color, int size = 12) {
  lv_obj_t *d = bare(parent);
  lv_obj_set_size(d, size, size);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
  return d;
}

void column(lv_obj_t *o, int gap) {
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(o, gap, 0);
}

void row(lv_obj_t *o, int gap) {
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(o, gap, 0);
}

// Button callbacks carry the action id as a heap copy, freed with the button.
char *attachId(lv_obj_t *obj, const String &id) {
  char *copy = strdup(id.c_str());
  lv_obj_add_event_cb(obj, [](lv_event_t *e) { free(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, copy);
  return copy;
}

void showToast(const String &message, uint32_t color) {
  lv_label_set_text(toast, message.c_str());
  lv_obj_set_style_border_color(toast, lv_color_hex(color), 0);
  lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
  lv_timer_reset(toastTimer);
  lv_timer_resume(toastTimer);
}

// --- Actions -----------------------------------------------------------------

void run(const String &id, const String &label) {
  net::runAction(id);
  showToast(label + "...", kMuted);
}

void confirmThen(const String &id, const String &label) {
  lv_obj_t *m = lv_msgbox_create(nullptr);
  lv_obj_set_width(m, 460);
  lv_msgbox_add_title(m, (label + "?").c_str());
  lv_msgbox_add_text(m, "This interrupts whatever it's doing for a moment.");
  lv_obj_t *yes = lv_msgbox_add_footer_button(m, "Do it");
  lv_obj_t *no = lv_msgbox_add_footer_button(m, "Cancel");
  lv_obj_set_style_bg_color(no, lv_color_hex(kButton), 0);
  // "id\nlabel" in one heap string, freed with the button.
  char *payload = attachId(yes, id + "\n" + label);
  lv_obj_add_event_cb(yes, [](lv_event_t *e) {
    String payload = (const char *)lv_event_get_user_data(e);
    int split = payload.indexOf('\n');
    lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target_obj(e)));
    run(payload.substring(0, split), payload.substring(split + 1));
    lv_msgbox_close_async(mbox);
  }, LV_EVENT_CLICKED, payload);
  lv_obj_add_event_cb(no, [](lv_event_t *e) {
    lv_msgbox_close_async(lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target_obj(e))));
  }, LV_EVENT_CLICKED, nullptr);
}

lv_obj_t *actionButton(lv_obj_t *parent, const PanelAction &a, int w, int h) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(kButton), 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(a.confirm ? 0x3F1D1D : kBorder), 0);
  lv_obj_t *l = text(b, &lv_font_montserrat_20, kText, a.label.c_str());
  lv_obj_center(l);
  char *payload = attachId(b, a.id + "\n" + a.label + "\n" + (a.confirm ? "1" : "0"));
  lv_obj_add_event_cb(b, [](lv_event_t *e) {
    String payload = (const char *)lv_event_get_user_data(e);
    int first = payload.indexOf('\n'), second = payload.lastIndexOf('\n');
    String id = payload.substring(0, first), label = payload.substring(first + 1, second);
    if (payload.endsWith("1")) confirmThen(id, label);
    else run(id, label);
  }, LV_EVENT_CLICKED, payload);
  if (a.id == "arc:quiet") quietLabel = l;
  return b;
}

lv_obj_t *localButton(lv_obj_t *parent, const char *label, lv_event_cb_t cb) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, 180, 64);
  lv_obj_set_style_bg_color(b, lv_color_hex(kButton), 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_center(text(b, &lv_font_montserrat_20, kText, label));
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

// --- Wi-Fi setup screen ------------------------------------------------------

void showWifiSetup();

void pickNetwork(lv_event_t *e) {
  chosenSsid = (const char *)lv_event_get_user_data(e);
  lv_label_set_text_fmt(wifiChosen, "Network: %s", chosenSsid.c_str());
  lv_textarea_set_text(wifiPassword, "");
}

void populateWifiList(const std::vector<String> &ssids) {
  lv_obj_clean(wifiList);
  if (ssids.empty()) lv_list_add_text(wifiList, "No networks found");
  for (const String &s : ssids) {
    lv_obj_t *b = lv_list_add_button(wifiList, LV_SYMBOL_WIFI, s.c_str());
    lv_obj_add_event_cb(b, pickNetwork, LV_EVENT_CLICKED, attachId(b, s));
  }
}

void connectChosen(lv_event_t *) {
  if (chosenSsid.isEmpty()) {
    lv_label_set_text(wifiChosen, "Pick a network first");
    return;
  }
  net::saveWifi(chosenSsid, lv_textarea_get_text(wifiPassword));
  lv_screen_load(mainScreen);
  showToast("Connecting to " + chosenSsid, kMuted);
}

void buildWifiScreen() {
  wifiScreen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(wifiScreen, lv_color_hex(kBg), 0);
  lv_obj_remove_flag(wifiScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = text(wifiScreen, &lv_font_montserrat_24, kText, "Wi-Fi setup");
  lv_obj_set_pos(title, 16, 12);

  wifiList = lv_list_create(wifiScreen);
  lv_obj_set_size(wifiList, 370, 200);
  lv_obj_set_pos(wifiList, 12, 48);

  wifiChosen = text(wifiScreen, &lv_font_montserrat_20, kText, "Pick a network");
  lv_obj_set_pos(wifiChosen, 400, 52);
  lv_label_set_long_mode(wifiChosen, LV_LABEL_LONG_DOT);
  lv_obj_set_width(wifiChosen, 385);

  wifiPassword = lv_textarea_create(wifiScreen);
  lv_textarea_set_one_line(wifiPassword, true);
  lv_textarea_set_password_mode(wifiPassword, true);
  lv_textarea_set_placeholder_text(wifiPassword, "Password");
  lv_obj_set_size(wifiPassword, 385, 50);
  lv_obj_set_pos(wifiPassword, 400, 88);

  lv_obj_t *connect = localButton(wifiScreen, "Connect", connectChosen);
  lv_obj_set_pos(connect, 400, 150);
  lv_obj_set_style_bg_color(connect, lv_color_hex(kAccent), 0);
  wifiCancel = localButton(wifiScreen, "Cancel", [](lv_event_t *) { lv_screen_load(mainScreen); });
  lv_obj_set_pos(wifiCancel, 600, 150);
  lv_obj_t *rescan = localButton(wifiScreen, LV_SYMBOL_REFRESH, [](lv_event_t *) { showWifiSetup(); });
  lv_obj_set_size(rescan, 60, 40);
  lv_obj_set_pos(rescan, 322, 6);

  lv_obj_t *keyboard = lv_keyboard_create(wifiScreen);
  lv_obj_set_size(keyboard, 800, 220);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(keyboard, wifiPassword);
  lv_obj_add_event_cb(keyboard, connectChosen, LV_EVENT_READY, nullptr);
}

void showWifiSetup() {
  lv_obj_clean(wifiList);
  lv_list_add_text(wifiList, "Scanning...");
  if (net::wifiState() == net::WifiState::NoCredentials) lv_obj_add_flag(wifiCancel, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(wifiCancel, LV_OBJ_FLAG_HIDDEN);
  net::startScan();
  scanning = true;
  lv_screen_load(wifiScreen);
}

// --- Minecraft sub-pages (Players, World) -----------------------------------
// Full-screen layers over the tabs, opened from the Minecraft page, closed
// with Back. Data comes from Arc (which reads it through Beacon), fetched
// only while a page is open.

enum class Page { None, Players, World, Inventory };
Page openPage = Page::None;
lv_obj_t *pageLayer, *pageTitle, *pageBody;
uint32_t pageFetchedAt = 0;
constexpr uint32_t kWorldRefreshMs = 10000;

JsonDocument playersDoc;  // kept, so switching between players needs no refetch
int selectedPlayer = -1;  // -1 = leaderboard

// World page widgets, built once then updated in place so a refresh never
// deletes a switch mid-touch.
lv_obj_t *worldClock = nullptr, *worldPhase, *worldFacts, *worldDisks;
lv_obj_t *ruleSwitches[4];
String ruleIds[4];

void closePage() {
  openPage = Page::None;
  lv_obj_add_flag(pageLayer, LV_OBJ_FLAG_HIDDEN);
}

void pageMessage(const char *message) {
  lv_obj_clean(pageBody);
  worldClock = nullptr;
  lv_obj_t *l = text(pageBody, &lv_font_montserrat_20, kMuted, message);
  lv_obj_center(l);
}

void showPage(Page page, const char *title) {
  openPage = page;
  lv_label_set_text(pageTitle, title);
  lv_obj_remove_flag(pageLayer, LV_OBJ_FLAG_HIDDEN);
  pageMessage("Loading...");
  pageFetchedAt = millis();
}

void openPlayers() {
  selectedPlayer = -1;
  showPage(Page::Players, "Players");
  net::fetchDetail("/panel/minecraft/players");
}

void openWorld() {
  showPage(Page::World, "World");
  net::fetchDetail("/panel/minecraft/world?fresh=1");  // one world save, so weather is current
}

void backToPlayers();

void buildPageLayer() {
  pageLayer = bare(mainScreen);
  lv_obj_set_size(pageLayer, board::kWidth, board::kHeight - kTopBar);
  lv_obj_set_pos(pageLayer, 0, kTopBar);
  lv_obj_set_style_bg_opa(pageLayer, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(pageLayer, lv_color_hex(kBg), 0);
  lv_obj_add_flag(pageLayer, LV_OBJ_FLAG_CLICKABLE);  // swallow touches meant for the tabs underneath

  lv_obj_t *back = localButton(pageLayer, LV_SYMBOL_LEFT "  Back", [](lv_event_t *) {
    if (openPage == Page::Inventory) backToPlayers();
    else closePage();
  });
  lv_obj_set_size(back, 130, 44);
  lv_obj_set_pos(back, 12, 8);
  pageTitle = text(pageLayer, &lv_font_montserrat_24, kText, "");
  lv_obj_set_pos(pageTitle, 160, 17);

  pageBody = bare(pageLayer);
  lv_obj_set_size(pageBody, 776, 352);
  lv_obj_set_pos(pageBody, 12, 64);
  lv_obj_add_flag(pageLayer, LV_OBJ_FLAG_HIDDEN);
}

// Players ----------------------------------------------------------------

void renderPlayers();
void openInventory(int player);

lv_obj_t *statTile(lv_obj_t *parent, const String &value, const char *caption) {
  lv_obj_t *tile = box(parent, 117, 66, 0x0E0E10);
  lv_obj_set_style_pad_all(tile, 8, 0);
  text(tile, &lv_font_montserrat_20, kText, value.c_str());
  lv_obj_t *cap = text(tile, &lv_font_montserrat_14, kMuted, caption);
  lv_obj_align(cap, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  return tile;
}

String topList(JsonArrayConst items) {
  String out;
  for (JsonObjectConst e : items) {
    if (out.length()) out += ", ";
    out += String((const char *)(e["name"] | "?")) + " " + String((long)(e["count"] | 0));
  }
  return out.length() ? out : String("-");
}

void leaderboardColumn(lv_obj_t *parent, const char *title, const char *key, const char *suffix) {
  lv_obj_t *col = bare(parent);
  lv_obj_set_size(col, 158, LV_SIZE_CONTENT);
  column(col, 8);
  text(col, &lv_font_montserrat_16, kMuted, title);
  struct Entry { String name; float value; };
  std::vector<Entry> entries;
  for (JsonObjectConst p : playersDoc["players"].as<JsonArrayConst>()) {
    if (p["stats"].isNull()) continue;
    entries.push_back({p["name"] | "?", p["stats"][key] | 0.0f});
  }
  std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.value > b.value; });
  for (size_t i = 0; i < entries.size(); i++) {
    String value = (entries[i].value == (long)entries[i].value) ? String((long)entries[i].value) : String(entries[i].value, 1);
    String line = String(i + 1) + ". " + entries[i].name + "  " + value + suffix;
    lv_obj_t *l = text(col, &lv_font_montserrat_16, i == 0 ? kWarn : kText, line.c_str());
    lv_obj_set_width(l, 158);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  }
}

void renderPlayers() {
  lv_obj_clean(pageBody);
  worldClock = nullptr;
  JsonArrayConst players = playersDoc["players"].as<JsonArrayConst>();

  lv_obj_t *list = bare(pageBody);
  lv_obj_set_size(list, 230, 352);
  column(list, 6);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  auto entry = [&](int index, const String &label, uint32_t dotColor, bool op) {
    lv_obj_t *b = lv_button_create(list);
    lv_obj_set_size(b, 226, 46);
    lv_obj_set_style_bg_color(b, lv_color_hex(kButton), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_border_width(b, index == selectedPlayer ? 2 : 0, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(kAccent), 0);
    row(b, 10);
    if (dotColor) dot(b, dotColor, 10);
    lv_obj_t *name = text(b, &lv_font_montserrat_16, kText, label.c_str());
    lv_obj_set_flex_grow(name, 1);
    if (op) text(b, &lv_font_montserrat_14, kWarn, "OP");
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
      selectedPlayer = (int)(intptr_t)lv_event_get_user_data(e);
      renderPlayers();
    }, LV_EVENT_CLICKED, (void *)(intptr_t)index);
  };
  entry(-1, LV_SYMBOL_LIST "  Leaderboard", 0, false);
  int i = 0;
  for (JsonObjectConst p : players) {
    entry(i++, p["name"] | "?", (p["online"] | false) ? kOk : 0x3F3F46, p["op"] | false);
  }

  lv_obj_t *detail = box(pageBody, 534, 352);
  lv_obj_set_pos(detail, 242, 0);
  lv_obj_set_style_pad_all(detail, 16, 0);

  if (selectedPlayer < 0) {
    row(detail, 14);
    lv_obj_set_flex_align(detail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    leaderboardColumn(detail, "Playtime", "playtime_h", "h");
    leaderboardColumn(detail, "Mob kills", "mob_kills", "");
    leaderboardColumn(detail, "Deaths", "deaths", "");
    return;
  }

  JsonObjectConst p = players[selectedPlayer];
  column(detail, 8);
  lv_obj_t *head = bare(detail);
  lv_obj_set_size(head, 500, LV_SIZE_CONTENT);
  row(head, 12);
  text(head, &lv_font_montserrat_28, kText, p["name"] | "?");
  bool online = p["online"] | false;
  text(head, &lv_font_montserrat_16, online ? kOk : kMuted, online ? "online" : "offline");
  if (p["op"] | false) text(head, &lv_font_montserrat_16, kWarn, "OP");

  JsonObjectConst st = p["stats"];
  if (st.isNull()) {
    text(detail, &lv_font_montserrat_16, kMuted, "No stats yet - they haven't played on this world.");
    return;
  }
  lv_obj_t *grid = bare(detail);
  lv_obj_set_size(grid, 502, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(grid, 8, 0);
  lv_obj_set_style_pad_column(grid, 8, 0);
  statTile(grid, String((float)(st["playtime_h"] | 0.0f), 1) + "h", "playtime");
  statTile(grid, String((long)(st["deaths"] | 0)), "deaths");
  statTile(grid, String((long)(st["mob_kills"] | 0)), "mob kills");
  statTile(grid, String((long)(st["player_kills"] | 0)), "player kills");
  statTile(grid, String((float)(st["distance_km"] | 0.0f), 1) + "km", "walked");
  statTile(grid, String((long)(st["jumps"] | 0)), "jumps");
  statTile(grid, String((long)(st["damage_dealt_hearts"] | 0)), "hearts dealt");
  statTile(grid, String((long)(st["damage_taken_hearts"] | 0)), "hearts taken");
  lv_obj_t *mined = text(detail, &lv_font_montserrat_16, kText, ("Top mined: " + topList(st["top_mined"])).c_str());
  lv_obj_set_width(mined, 500);
  lv_label_set_long_mode(mined, LV_LABEL_LONG_DOT);
  lv_obj_t *killed = text(detail, &lv_font_montserrat_16, kText, ("Top kills: " + topList(st["top_killed"])).c_str());
  lv_obj_set_width(killed, 500);
  lv_label_set_long_mode(killed, LV_LABEL_LONG_DOT);
  lv_obj_t *inv = localButton(detail, "Inventory  " LV_SYMBOL_RIGHT, [](lv_event_t *) { openInventory(selectedPlayer); });
  lv_obj_set_size(inv, 200, 44);
  lv_obj_set_style_border_width(inv, 1, 0);
  lv_obj_set_style_border_color(inv, lv_color_hex(kAccent), 0);
}


// Inventory ------------------------------------------------------------------
// Minecraft-style: armour and offhand down the left, main inventory over the
// hotbar, ender chest on a toggle, and a detail card for the tapped item
// (custom name, enchantments, trim, potion). Icons arrive inline from Arc as
// 16x16 PNGs; each is decoded once, upscaled 2x with nearest-neighbour (crisp
// pixels, like the game) and cached for the life of the panel.

JsonDocument invDoc;
String invUuid, invName;
bool showingEnder = false;
int selectedSlot = -1;  // group * 100 + index
lv_obj_t *invDetail;
std::map<String, lv_image_dsc_t *> iconCache;

constexpr int kSlot = 46, kStep = 50, kIconPx = 32;
constexpr int kGridX = 62, kGridY = 30;
enum SlotGroup { kHotbar = 0, kMain = 1, kArmor = 2, kOffhand = 3, kEnder = 4 };

lv_image_dsc_t *decodeIcon(const char *b64) {
  size_t b64len = strlen(b64), pngLen = 0;
  std::vector<unsigned char> png(b64len * 3 / 4 + 4);
  int b64err = mbedtls_base64_decode(png.data(), png.size(), &pngLen, (const unsigned char *)b64, b64len);
  if (b64err != 0) {
    Serial.printf("[ui] icon: base64 decode failed (%d, %u chars)\n", b64err, (unsigned)b64len);
    return nullptr;
  }
  lv_draw_buf_t *decoded = nullptr;
  unsigned w = 0, h = 0;
  unsigned pngErr = lodepng_decode32((unsigned char **)&decoded, &w, &h, png.data(), pngLen);
  if (pngErr != 0 || !decoded || !w || !h) {
    Serial.printf("[ui] icon: PNG decode failed (lodepng %u, %u bytes)\n", pngErr, (unsigned)pngLen);
    if (decoded) lv_draw_buf_destroy(decoded);
    return nullptr;
  }
  const uint8_t *rgba = decoded->data;
  uint32_t stride = decoded->header.stride;
  // RGBA -> LVGL ARGB8888 (little-endian B,G,R,A), nearest-neighbour to 32x32.
  size_t bytes = kIconPx * kIconPx * 4;
  uint8_t *pixels = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  auto *dsc = (lv_image_dsc_t *)heap_caps_calloc(1, sizeof(lv_image_dsc_t), MALLOC_CAP_SPIRAM);
  if (!pixels || !dsc) {
    lv_draw_buf_destroy(decoded);
    return nullptr;
  }
  for (int y = 0; y < kIconPx; y++) {
    for (int x = 0; x < kIconPx; x++) {
      const uint8_t *src = rgba + (y * h / kIconPx) * stride + (x * w / kIconPx) * 4;
      uint8_t *dst = pixels + (y * kIconPx + x) * 4;
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = src[3];
    }
  }
  lv_draw_buf_destroy(decoded);
  dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
  dsc->header.w = kIconPx;
  dsc->header.h = kIconPx;
  dsc->header.stride = kIconPx * 4;
  dsc->data_size = bytes;
  dsc->data = pixels;
  return dsc;
}

void cacheIcons() {
  int offered = 0, decoded = 0;
  for (JsonPairConst kv : invDoc["icons"].as<JsonObjectConst>()) {
    offered++;
    String key = kv.key().c_str();
    if (iconCache.count(key)) continue;
    lv_image_dsc_t *dsc = decodeIcon(kv.value().as<const char *>());
    if (dsc) {
      iconCache[key] = dsc;
      decoded++;
    }
  }
  Serial.printf("[ui] icons: %d offered, %d newly decoded, %u cached\n", offered, decoded, (unsigned)iconCache.size());
}

JsonObjectConst slotItem(int code) {
  JsonObjectConst slots = invDoc["slots"];
  int group = code / 100, index = code % 100;
  switch (group) {
    case kHotbar: return slots["hotbar"][index];
    case kMain: return slots["main"][index];
    case kArmor: return slots["armor"][index];
    case kOffhand: return slots["offhand"];
    default: return slots["ender"][index];
  }
}

lv_obj_t *itemIcon(lv_obj_t *parent, JsonObjectConst item) {
  auto it = iconCache.find(String((const char *)(item["icon"] | "")));
  if (it != iconCache.end()) {
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, it->second);
    return img;
  }
  // No icon (unusual item Beacon couldn't find art for): its initial instead.
  String name = item["base"] | "?";
  return text(parent, &lv_font_montserrat_20, kText, name.substring(0, 1).c_str());
}

void renderInventory();

void showItemDetail() {
  lv_obj_clean(invDetail);
  column(invDetail, 6);
  JsonObjectConst item = selectedSlot >= 0 ? slotItem(selectedSlot) : JsonObjectConst();
  if (item.isNull()) {
    text(invDetail, &lv_font_montserrat_16, kMuted, "Tap an item to see\nits name and enchantments.");
    return;
  }
  lv_obj_t *icon = itemIcon(invDetail, item);
  if (lv_obj_check_type(icon, &lv_image_class)) {
    lv_image_set_scale(icon, 512);  // 64px
    lv_image_set_antialias(icon, false);
    lv_obj_set_size(icon, 64, 64);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
  }
  bool enchanted = item["ench"].size() > 0;
  // Minecraft colours: enchanted names aqua, curses red, enchantment lines grey.
  lv_obj_t *name = text(invDetail, &lv_font_montserrat_20, enchanted ? 0x55FFFF : kText, item["name"] | "?");
  lv_obj_set_width(name, 222);
  lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
  if (item["custom"] | false) text(invDetail, &lv_font_montserrat_14, kMuted, item["base"] | "");
  int count = item["count"] | 1;
  if (count > 1) text(invDetail, &lv_font_montserrat_16, kText, ("x" + String(count)).c_str());
  for (JsonObjectConst e : item["ench"].as<JsonArrayConst>())
    text(invDetail, &lv_font_montserrat_16, (e["curse"] | false) ? 0xFF5555 : 0xAAAAAA, e["text"] | "");
  if (item["trim"].is<const char *>()) text(invDetail, &lv_font_montserrat_14, 0xAAAAAA, item["trim"] | "");
  if (item["potion"].is<const char *>()) text(invDetail, &lv_font_montserrat_14, 0x5555FF, item["potion"] | "");
}

lv_obj_t *slotBox(int code, int x, int y) {
  lv_obj_t *slot = bare(pageBody);
  lv_obj_set_size(slot, kSlot, kSlot);
  lv_obj_set_pos(slot, x, y);
  lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(slot, lv_color_hex(0x26262A), 0);
  lv_obj_set_style_radius(slot, 4, 0);
  JsonObjectConst item = slotItem(code);
  bool enchanted = !item.isNull() && item["ench"].size() > 0;
  bool selected = code == selectedSlot;
  lv_obj_set_style_border_width(slot, selected || enchanted ? 2 : 1, 0);
  lv_obj_set_style_border_color(slot, lv_color_hex(selected ? kAccent : enchanted ? 0xA855F7 : 0x3A3A40), 0);
  if (!item.isNull()) {
    lv_obj_center(itemIcon(slot, item));
    int count = item["count"] | 1;
    if (count > 1) {
      lv_obj_t *c = text(slot, &lv_font_montserrat_14, 0xFFFFFF, String(count).c_str());
      lv_obj_align(c, LV_ALIGN_BOTTOM_RIGHT, -2, 0);
    }
    lv_obj_add_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slot, [](lv_event_t *e) {
      selectedSlot = (int)(intptr_t)lv_event_get_user_data(e);
      renderInventory();
    }, LV_EVENT_CLICKED, (void *)(intptr_t)code);
  }
  return slot;
}

void renderInventory() {
  lv_obj_clean(pageBody);
  worldClock = nullptr;
  String stats = "Health " + String((int)(invDoc["health"] | 0.0f)) + "/20    Food " + String((int)(invDoc["food"] | 0)) +
                 "/20    Level " + String((int)(invDoc["xp_level"] | 0)) + "    " + (const char *)(invDoc["gamemode"] | "") +
                 "    " + (const char *)(invDoc["dimension"] | "");
  text(pageBody, &lv_font_montserrat_16, kMuted, stats.c_str());

  for (int i = 0; i < 4; i++) slotBox(kArmor * 100 + i, 0, kGridY + i * kStep);
  slotBox(kOffhand * 100, 0, kGridY + 4 * kStep + 12);

  if (showingEnder) {
    for (int i = 0; i < 27; i++) slotBox(kEnder * 100 + i, kGridX + (i % 9) * kStep, kGridY + (i / 9) * kStep);
  } else {
    for (int i = 0; i < 27; i++) slotBox(kMain * 100 + i, kGridX + (i % 9) * kStep, kGridY + (i / 9) * kStep);
    for (int i = 0; i < 9; i++) slotBox(kHotbar * 100 + i, kGridX + i * kStep, kGridY + 3 * kStep + 10);
  }

  lv_obj_t *toggle = localButton(pageBody, showingEnder ? "Inventory" : "Ender chest", [](lv_event_t *) {
    showingEnder = !showingEnder;
    selectedSlot = -1;
    renderInventory();
  });
  lv_obj_set_size(toggle, 180, 44);
  lv_obj_set_pos(toggle, kGridX, kGridY + 4 * kStep + 20);

  invDetail = box(pageBody, 254, 322);
  lv_obj_set_pos(invDetail, kGridX + 8 * kStep + kSlot + 14, kGridY);
  lv_obj_set_style_pad_all(invDetail, 14, 0);
  lv_obj_add_flag(invDetail, LV_OBJ_FLAG_SCROLLABLE);
  showItemDetail();
}

void openInventory(int player) {
  JsonObjectConst p = playersDoc["players"][player];
  invUuid = p["uuid"] | "";
  invName = p["name"] | "?";
  showingEnder = false;
  selectedSlot = -1;
  showPage(Page::Inventory, (invName + "'s inventory").c_str());
  net::fetchDetail("/panel/minecraft/inventory/" + invUuid);
}

void backToPlayers() {
  showPage(Page::Players, "Players");
  renderPlayers();  // from the cached roster, no refetch
}

// World --------------------------------------------------------------------

void buildWorld() {
  lv_obj_clean(pageBody);
  lv_obj_t *left = box(pageBody, 380, 352);
  lv_obj_set_style_pad_all(left, 18, 0);
  text(left, &lv_font_montserrat_14, kMuted, "In-game time");
  worldClock = text(left, &lv_font_montserrat_48, kText, "--:--");
  lv_obj_align(worldClock, LV_ALIGN_TOP_LEFT, 0, 20);
  worldPhase = text(left, &lv_font_montserrat_20, kWarn, "");
  lv_obj_align(worldPhase, LV_ALIGN_TOP_LEFT, 170, 44);
  worldFacts = text(left, &lv_font_montserrat_16, kText, "");
  lv_obj_set_style_text_line_space(worldFacts, 8, 0);
  lv_obj_align(worldFacts, LV_ALIGN_TOP_LEFT, 0, 96);

  lv_obj_t *rules = box(pageBody, 384, 196);
  lv_obj_set_pos(rules, 392, 0);
  column(rules, 6);
  text(rules, &lv_font_montserrat_14, kMuted, "Game rules");
  for (int i = 0; i < 4; i++) {
    lv_obj_t *r = bare(rules);
    lv_obj_set_size(r, 352, 32);
    lv_obj_t *label = text(r, &lv_font_montserrat_16, kText, "");
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 60, 30);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sw, [](lv_event_t *e) {
      int index = (int)(intptr_t)lv_event_get_user_data(e);
      lv_obj_t *label = lv_obj_get_child(lv_obj_get_parent(lv_event_get_target_obj(e)), 0);
      run(ruleIds[index], lv_label_get_text(label));
    }, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    ruleSwitches[i] = sw;
  }

  lv_obj_t *disks = box(pageBody, 384, 146);
  lv_obj_set_pos(disks, 392, 206);
  text(disks, &lv_font_montserrat_14, kMuted, "Disk");
  worldDisks = text(disks, &lv_font_montserrat_16, kText, "");
  lv_obj_set_style_text_line_space(worldDisks, 6, 0);
  lv_obj_align(worldDisks, LV_ALIGN_TOP_LEFT, 0, 22);
}

void renderWorld(JsonDocument &doc) {
  if (!worldClock) buildWorld();
  lv_label_set_text(worldClock, doc["clock"] | "--:--");
  String phase = doc["phase"] | "";
  lv_label_set_text(worldPhase, phase.c_str());
  uint32_t phaseColor = phase == "Day" ? 0xFACC15 : phase == "Night" ? 0x818CF8 : 0xFB923C;
  lv_obj_set_style_text_color(worldPhase, lv_color_hex(phaseColor), 0);
  String facts = String("Weather     ") + (const char *)(doc["weather"] | "?") +
                 "\nDifficulty  " + (const char *)(doc["difficulty"] | "?") +
                 "\nUptime      " + (const char *)(doc["uptime"] | "?") +
                 "\nSpawn        " + (const char *)(doc["spawn"] | "?") +
                 "\nSeed          " + (const char *)(doc["seed"] | "?");
  lv_label_set_text(worldFacts, facts.c_str());
  String disks;
  for (JsonObjectConst d : doc["disks"].as<JsonArrayConst>())
    disks += String(disks.length() ? "\n" : "") + (const char *)(d["name"] | "?") + "   " + (const char *)(d["size"] | "?");
  lv_label_set_text(worldDisks, disks.c_str());
  int i = 0;
  for (JsonObjectConst r : doc["rules"].as<JsonArrayConst>()) {
    if (i >= 4) break;
    ruleIds[i] = r["id"] | "";
    lv_label_set_text(lv_obj_get_child(lv_obj_get_parent(ruleSwitches[i]), 0), r["label"] | "");
    if (r["on"] | false) lv_obj_add_state(ruleSwitches[i], LV_STATE_CHECKED);
    else lv_obj_remove_state(ruleSwitches[i], LV_STATE_CHECKED);
    if (r["known"] | false) lv_obj_remove_state(ruleSwitches[i], LV_STATE_DISABLED);
    else lv_obj_add_state(ruleSwitches[i], LV_STATE_DISABLED);
    i++;
  }
}

// Network ------------------------------------------------------------------
// Internet status, live throughput with a ~30 minute graph, and the busiest
// devices right now — all from Arc's network sense (UniFi), in the 5s state.

lv_obj_t *netStatus, *netFacts, *netDownLabel, *netUpLabel, *netChart, *netDeviceList;
lv_chart_series_t *netDownSeries, *netUpSeries;
constexpr uint32_t kDownColor = 0x38BDF8, kUpColor = 0xA78BFA;

String rate(int kbps) {
  if (kbps < 1000) return String(kbps) + " kbps";
  return String(kbps / 1000.0f, kbps < 10000 ? 1 : 0) + " Mbps";
}

void buildNetwork(lv_obj_t *body) {
  lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *left = box(body, 420, 352);
  lv_obj_set_style_pad_all(left, 16, 0);
  text(left, &lv_font_montserrat_14, kMuted, "Internet");
  netStatus = text(left, &lv_font_montserrat_36, kText, "-");
  lv_obj_align(netStatus, LV_ALIGN_TOP_LEFT, 0, 18);
  netFacts = text(left, &lv_font_montserrat_14, kMuted, "");
  lv_obj_align(netFacts, LV_ALIGN_TOP_LEFT, 0, 64);
  netDownLabel = text(left, &lv_font_montserrat_20, kDownColor, "");
  lv_obj_align(netDownLabel, LV_ALIGN_TOP_RIGHT, 0, 16);
  netUpLabel = text(left, &lv_font_montserrat_20, kUpColor, "");
  lv_obj_align(netUpLabel, LV_ALIGN_TOP_RIGHT, 0, 44);

  netChart = lv_chart_create(left);
  lv_obj_set_size(netChart, 388, 190);
  lv_obj_align(netChart, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_chart_set_type(netChart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(netChart, 3, 0);
  lv_obj_set_style_bg_opa(netChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(netChart, 0, 0);
  lv_obj_set_style_line_color(netChart, lv_color_hex(kBorder), LV_PART_MAIN);
  lv_obj_set_style_size(netChart, 0, 0, LV_PART_INDICATOR);  // lines only, no point dots
  lv_obj_set_style_line_width(netChart, 2, LV_PART_ITEMS);
  netDownSeries = lv_chart_add_series(netChart, lv_color_hex(kDownColor), LV_CHART_AXIS_PRIMARY_Y);
  netUpSeries = lv_chart_add_series(netChart, lv_color_hex(kUpColor), LV_CHART_AXIS_PRIMARY_Y);

  lv_obj_t *right = box(body, 346, 352);
  lv_obj_set_pos(right, 430, 0);
  text(right, &lv_font_montserrat_14, kMuted, "Busiest devices right now");
  netDeviceList = bare(right);
  lv_obj_set_size(netDeviceList, 316, 300);
  lv_obj_align(netDeviceList, LV_ALIGN_TOP_LEFT, 0, 24);
  column(netDeviceList, 6);
}

void applyNetwork() {
  if (!snap->netReady) {
    lv_label_set_text(netStatus, "Waiting");
    return;
  }
  lv_label_set_text(netStatus, snap->netOnline ? "Online" : "Down");
  lv_obj_set_style_text_color(netStatus, lv_color_hex(snap->netOnline ? kOk : kAlert), 0);
  String facts = snap->netIsp + "   " + (snap->netLatency >= 0 ? String(snap->netLatency) + " ms" : String("-")) +
                 "\nUp " + snap->netUptime + "   " + String(snap->netDrops) + " drops";
  lv_label_set_text(netFacts, facts.c_str());
  lv_label_set_text(netDownLabel, (LV_SYMBOL_DOWN " " + rate(snap->netDown)).c_str());
  lv_label_set_text(netUpLabel, (LV_SYMBOL_UP " " + rate(snap->netUp)).c_str());

  size_t points = snap->netHistDown.size();
  int peak = 100;
  for (size_t i = 0; i < points; i++) peak = max(peak, max(snap->netHistDown[i], snap->netHistUp[i]));
  lv_chart_set_point_count(netChart, points ? points : 1);
  lv_chart_set_range(netChart, LV_CHART_AXIS_PRIMARY_Y, 0, peak + peak / 5);
  for (size_t i = 0; i < points; i++) {
    lv_chart_set_value_by_id(netChart, netDownSeries, i, snap->netHistDown[i]);
    lv_chart_set_value_by_id(netChart, netUpSeries, i, snap->netHistUp[i]);
  }
  lv_chart_refresh(netChart);

  lv_obj_clean(netDeviceList);
  for (const NetDevice &d : snap->netDevices) {
    lv_obj_t *r = bare(netDeviceList);
    lv_obj_set_size(r, 316, 30);
    lv_obj_t *name = text(r, &lv_font_montserrat_16, kText, d.name.c_str());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 150);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
    String rates = String(LV_SYMBOL_DOWN) + rate(d.down) + "  " + LV_SYMBOL_UP + rate(d.up);
    lv_obj_t *r2 = text(r, &lv_font_montserrat_14, (d.down + d.up) > 0 ? kText : kMuted, rates.c_str());
    lv_obj_align(r2, LV_ALIGN_RIGHT_MID, 0, 0);
  }
}

// Proxmox ------------------------------------------------------------------
// Hosts across the top (CPU/RAM/disk bars), every container and VM below.
// Fetched from Arc only while the section is open (read-only API token).

lv_obj_t *proxmoxBody;
JsonDocument proxmoxDoc;
bool proxmoxHave = false;
uint32_t proxmoxFetchedAt = 0;

void proxmoxMessage(const char *message) {
  lv_obj_clean(proxmoxBody);
  lv_obj_center(text(proxmoxBody, &lv_font_montserrat_20, kMuted, message));
}

void meter(lv_obj_t *parent, const String &label, float fraction) {
  lv_obj_t *l = text(parent, &lv_font_montserrat_14, kMuted, label.c_str());
  (void)l;
  lv_obj_t *bar = lv_bar_create(parent);
  lv_obj_set_size(bar, 222, 8);
  lv_bar_set_range(bar, 0, 1000);
  lv_bar_set_value(bar, (int)(constrain(fraction, 0.0f, 1.0f) * 1000), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A2A2E), LV_PART_MAIN);
  uint32_t color = fraction > 0.9f ? kAlert : fraction > 0.75f ? kWarn : kOk;
  lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
}

void renderProxmox() {
  lv_coord_t scroll = 0;
  lv_obj_t *oldList = lv_obj_get_child_count(proxmoxBody) > 1 ? lv_obj_get_child(proxmoxBody, -1) : nullptr;
  if (oldList) scroll = lv_obj_get_scroll_y(oldList);
  lv_obj_clean(proxmoxBody);

  lv_obj_t *hosts = bare(proxmoxBody);
  lv_obj_set_size(hosts, 776, 150);
  lv_obj_set_flex_flow(hosts, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(hosts, 10, 0);
  lv_obj_add_flag(hosts, LV_OBJ_FLAG_SCROLLABLE);
  for (JsonObjectConst n : proxmoxDoc["nodes"].as<JsonArrayConst>()) {
    lv_obj_t *card = box(hosts, 252, 150);
    lv_obj_set_style_pad_all(card, 12, 0);
    column(card, 3);
    bool online = n["online"] | false;
    lv_obj_t *head = bare(card);
    lv_obj_set_size(head, 226, LV_SIZE_CONTENT);
    row(head, 8);
    dot(head, online ? kOk : (n["expected_off"] | false) ? 0x3F3F46 : kAlert, 10);
    text(head, &lv_font_montserrat_20, online ? kText : kMuted, n["name"] | "?");
    bool expectedOff = n["expected_off"] | false;
    text(head, &lv_font_montserrat_14, kMuted,
         online ? (const char *)(n["uptime"] | "") : expectedOff ? "off, as expected" : "offline");
    if (!online) {
      if (!expectedOff) lv_obj_set_style_border_color(card, lv_color_hex(kAlert), 0);
      lv_obj_set_style_bg_color(card, lv_color_hex(0x111113), 0);
      continue;
    }
    float memTotal = n["mem_total_gb"] | 1.0f, diskTotal = n["disk_total_gb"] | 1.0f;
    meter(card, "CPU " + String((int)(n["cpu"] | 0)) + "% of " + String((int)(n["cores"] | 0)) + " cores",
          (n["cpu"] | 0) / 100.0f);
    meter(card, "RAM " + String((float)(n["mem_gb"] | 0.0f), 1) + " / " + String(memTotal, 0) + " GB",
          (n["mem_gb"] | 0.0f) / memTotal);
    meter(card, "Disk " + String((float)(n["disk_gb"] | 0.0f), 0) + " / " + String(diskTotal, 0) + " GB",
          (n["disk_gb"] | 0.0f) / diskTotal);
  }

  lv_obj_t *list = box(proxmoxBody, 776, 192);
  lv_obj_set_pos(list, 0, 160);
  lv_obj_set_style_pad_all(list, 10, 0);
  column(list, 4);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  for (JsonObjectConst g : proxmoxDoc["guests"].as<JsonArrayConst>()) {
    bool running = g["running"] | false;
    lv_obj_t *r = bare(list);
    lv_obj_set_size(r, 750, 28);
    lv_obj_t *d = dot(r, running ? kOk : 0x3F3F46, 10);
    lv_obj_align(d, LV_ALIGN_LEFT_MID, 0, 0);
    String name = String((int)(g["id"] | 0)) + "  " + (const char *)(g["name"] | "?");
    lv_obj_t *n = text(r, &lv_font_montserrat_16, running ? kText : kMuted, name.c_str());
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_width(n, 250);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 20, 0);
    String where = String((const char *)(g["kind"] | "")) + " on " + (const char *)(g["node"] | "?");
    lv_obj_align(text(r, &lv_font_montserrat_14, kMuted, where.c_str()), LV_ALIGN_LEFT_MID, 280, 0);
    String usage = running ? "CPU " + String((float)(g["cpu"] | 0.0f), 1) + "%   RAM " + String((float)(g["mem_gb"] | 0.0f), 1) +
                                 "/" + String((float)(g["mem_total_gb"] | 0.0f), 0) + " GB   " + (const char *)(g["uptime"] | "")
                           : String((g["expected_off"] | false) ? "off, as expected" : "stopped");
    lv_obj_align(text(r, &lv_font_montserrat_14, running ? kText : kMuted, usage.c_str()), LV_ALIGN_RIGHT_MID, 0, 0);
  }
  lv_obj_update_layout(list);
  lv_obj_scroll_to_y(list, scroll, LV_ANIM_OFF);  // a refresh shouldn't lose your place
}

void fetchProxmox() {
  proxmoxFetchedAt = millis();
  net::fetchDetail("/panel/proxmox");
}

void handleDetail() {
  String path, body;
  int status;
  if (!net::takeDetail(path, status, body)) return;
  bool players = path.startsWith("/panel/minecraft/players");
  bool world = path.startsWith("/panel/minecraft/world");
  bool inventory = path.startsWith("/panel/minecraft/inventory/");
  if (path == "/panel/proxmox") {
    if (currentSection != kProxmox) return;
    if (status == 200 && !deserializeJson(proxmoxDoc, body)) {
      proxmoxHave = true;
      renderProxmox();
    } else if (!proxmoxHave) {
      proxmoxMessage(status == 404 ? "Proxmox isn't set up yet.\nRun deploy/set-proxmox-token.sh on the Mac."
                                   : "Couldn't reach Proxmox.");
    }
    return;
  }
  if ((players && openPage != Page::Players) || (world && openPage != Page::World) ||
      (inventory && (openPage != Page::Inventory || !path.endsWith(invUuid))))
    return;  // closed (or moved on) meanwhile
  if (status != 200) {
    if (world && worldClock) return;  // keep showing the last good data; the next refresh may work
    if (inventory && status == 404)
      pageMessage((invName + " hasn't joined this server yet,\nso there's no inventory to show.").c_str());
    else
      pageMessage(status == 502 ? "Beacon didn't answer - is it running?" : "Couldn't reach Arc.");
    return;
  }
  if (players) {
    playersDoc.clear();
    if (deserializeJson(playersDoc, body)) return pageMessage("Got an unreadable reply from Arc.");
    renderPlayers();
  } else if (inventory) {
    invDoc.clear();
    if (deserializeJson(invDoc, body)) return pageMessage("Got an unreadable reply from Arc.");
    cacheIcons();
    renderInventory();
  } else if (world) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) return pageMessage("Got an unreadable reply from Arc.");
    renderWorld(doc);
  }
}

// --- Main screen -----------------------------------------------------------

void buildTopBar(lv_obj_t *parent) {
  topBar = bare(parent);
  lv_obj_set_size(topBar, board::kWidth, kTopBar);
  lv_obj_set_style_bg_opa(topBar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(topBar, lv_color_hex(kBg), 0);
  lv_obj_set_style_pad_hor(topBar, 16, 0);
  lv_obj_set_style_border_side(topBar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(topBar, 1, 0);
  lv_obj_set_style_border_color(topBar, lv_color_hex(kBorder), 0);

  lv_obj_t *brand = bare(topBar);
  lv_obj_set_size(brand, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  row(brand, 10);
  lv_obj_align(brand, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *mark = bare(brand);
  lv_obj_set_size(mark, 5, 28);
  lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(mark, lv_color_hex(kAccent), 0);
  lv_obj_set_style_radius(mark, 2, 0);
  // The panel's own name — Arc is the service behind it, this is the screen.
  lv_obj_t *name = text(brand, &lv_font_montserrat_24, kText, "Zia's Panel");
  lv_obj_set_style_text_letter_space(name, 1, 0);

  pill = text(topBar, &lv_font_montserrat_16, kText, "Starting...");
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pill, 14, 0);
  lv_obj_set_style_pad_hor(pill, 14, 0);
  lv_obj_set_style_pad_ver(pill, 5, 0);
  // A fixed width: LONG_DOT with only a max-width let the label collapse to
  // nothing and show just "..." (found live).
  lv_label_set_long_mode(pill, LV_LABEL_LONG_DOT);
  lv_obj_set_width(pill, 360);
  lv_obj_set_style_text_align(pill, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(pill, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *right = bare(topBar);
  lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  row(right, 14);
  lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
  wifiIcon = text(right, &lv_font_montserrat_20, kMuted, LV_SYMBOL_WIFI);
  clockLabel = text(right, &lv_font_montserrat_24, kText, "--:--");
}

void buildOverview(lv_obj_t *tab) {
  lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

  statusCard = box(tab, 444, 344);
  lv_obj_set_pos(statusCard, 0, 0);
  lv_obj_set_style_pad_all(statusCard, 20, 0);
  lv_obj_add_flag(statusCard, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(statusCard, [](lv_event_t *) {
    if (snap && snap->unacked > 0) run("arc:ack", "Acknowledge");
  }, LV_EVENT_CLICKED, nullptr);

  statusTitle = text(statusCard, &lv_font_montserrat_36, kText, "Waiting for Arc");
  statusSummary = text(statusCard, &lv_font_montserrat_20, kText, "");
  lv_obj_set_width(statusSummary, 400);
  lv_label_set_long_mode(statusSummary, LV_LABEL_LONG_DOT);
  lv_obj_set_height(statusSummary, 52);
  lv_obj_align(statusSummary, LV_ALIGN_TOP_LEFT, 0, 52);
  statusDetail = text(statusCard, &lv_font_montserrat_16, kMuted, "");
  lv_obj_set_width(statusDetail, 400);
  lv_obj_set_height(statusDetail, 90);
  lv_label_set_long_mode(statusDetail, LV_LABEL_LONG_DOT);
  lv_obj_align(statusDetail, LV_ALIGN_TOP_LEFT, 0, 112);

  lv_obj_t *stats = bare(statusCard);
  lv_obj_set_size(stats, 404, 86);
  lv_obj_align(stats, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  row(stats, 10);
  lv_obj_t **targets[] = {&statHomelab, &statMinecraft, &statDevices};
  const char *captions[] = {"homelab ok", "in Minecraft", "on Wi-Fi"};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *tile = box(stats, 128, 86, 0x0E0E10);
    lv_obj_set_style_pad_all(tile, 10, 0);
    *targets[i] = text(tile, &lv_font_montserrat_28, kText, "-");
    lv_obj_t *cap = text(tile, &lv_font_montserrat_14, kMuted, captions[i]);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }

  lv_obj_t *feed = box(tab, 320, 344);
  lv_obj_set_pos(feed, 456, 0);
  text(feed, &lv_font_montserrat_20, kText, "Activity");
  feedList = bare(feed);
  lv_obj_set_size(feedList, 290, 280);
  lv_obj_align(feedList, LV_ALIGN_TOP_LEFT, 0, 34);
  column(feedList, 10);
  lv_obj_add_flag(feedList, LV_OBJ_FLAG_SCROLLABLE);
}

void buildHomelab(lv_obj_t *tab) {
  homelabList = bare(tab);
  lv_obj_set_size(homelabList, 776, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(homelabList, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(homelabList, 8, 0);
  lv_obj_set_style_pad_column(homelabList, 8, 0);
}

void buildMinecraft(lv_obj_t *tab) {
  lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

  // Top half: server status, compact.
  lv_obj_t *card = box(tab, 370, 167);
  lv_obj_set_style_pad_all(card, 16, 0);
  text(card, &lv_font_montserrat_14, kMuted, "Minecraft server");
  mcState = text(card, &lv_font_montserrat_36, kText, "-");
  lv_obj_align(mcState, LV_ALIGN_TOP_LEFT, 0, 22);
  mcTps = text(card, &lv_font_montserrat_14, kMuted, "");
  lv_obj_align(mcTps, LV_ALIGN_TOP_RIGHT, 0, 0);
  mcPlayers = text(card, &lv_font_montserrat_16, kText, "");
  lv_obj_set_width(mcPlayers, 336);
  lv_label_set_long_mode(mcPlayers, LV_LABEL_LONG_DOT);
  lv_obj_align(mcPlayers, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  // Bottom half: chat, commands, joins and leaves, newest first.
  lv_obj_t *log = box(tab, 370, 167);
  lv_obj_set_pos(log, 0, 177);
  lv_obj_set_style_pad_all(log, 12, 0);
  text(log, &lv_font_montserrat_14, kMuted, "Server log");
  mcLogList = bare(log);
  lv_obj_set_size(mcLogList, 346, 122);
  lv_obj_align(mcLogList, LV_ALIGN_TOP_LEFT, 0, 20);
  column(mcLogList, 3);
  lv_obj_add_flag(mcLogList, LV_OBJ_FLAG_SCROLLABLE);

  mcButtons = bare(tab);
  lv_obj_set_size(mcButtons, 396, 344);
  lv_obj_set_pos(mcButtons, 382, 0);
  lv_obj_set_flex_flow(mcButtons, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(mcButtons, 10, 0);
  lv_obj_set_style_pad_column(mcButtons, 10, 0);
}

void buildControls(lv_obj_t *tab) {
  controlsPage = bare(tab);
  lv_obj_set_size(controlsPage, 776, LV_SIZE_CONTENT);
  column(controlsPage, 8);
}

lv_obj_t *homeLayer;
lv_obj_t *sectionLayers[kSectionCount];
lv_obj_t *tileStatus[kSectionCount], *tiles[kSectionCount];

void closePage();

void showHome() {
  closePage();
  for (lv_obj_t *layer : sectionLayers) lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(homeLayer, LV_OBJ_FLAG_HIDDEN);
  currentSection = kHome;
}

void showSection(int section) {
  closePage();
  lv_obj_add_flag(homeLayer, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < kSectionCount; i++) {
    if (i == section) lv_obj_remove_flag(sectionLayers[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(sectionLayers[i], LV_OBJ_FLAG_HIDDEN);
  }
  currentSection = section;
  if (section == kProxmox) {
    if (!proxmoxHave) proxmoxMessage("Loading...");
    fetchProxmox();
  }
}

// A full-screen section: Back to the home tiles, a title, and a body inset
// to the same 776x352 the section builders lay themselves out in.
lv_obj_t *buildSection(int section, const char *title) {
  lv_obj_t *layer = bare(mainScreen);
  lv_obj_set_size(layer, board::kWidth, board::kHeight - kTopBar);
  lv_obj_set_pos(layer, 0, kTopBar);
  lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(layer, lv_color_hex(kBg), 0);
  lv_obj_t *back = localButton(layer, LV_SYMBOL_LEFT "  Back", [](lv_event_t *) { showHome(); });
  lv_obj_set_size(back, 130, 44);
  lv_obj_set_pos(back, 12, 8);
  lv_obj_set_pos(text(layer, &lv_font_montserrat_24, kText, title), 160, 17);
  lv_obj_t *body = bare(layer);
  lv_obj_set_size(body, 776, 352);
  lv_obj_set_pos(body, 12, 64);
  lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
  sectionLayers[section] = layer;
  return body;
}

void buildHome() {
  homeLayer = bare(mainScreen);
  lv_obj_set_size(homeLayer, board::kWidth, board::kHeight - kTopBar);
  lv_obj_set_pos(homeLayer, 0, kTopBar);
  const char *titles[kSectionCount] = {"Overview", "Homelab", "Proxmox", "Network", "Minecraft", "Controls"};
  const char *icons[kSectionCount] = {LV_SYMBOL_EYE_OPEN, LV_SYMBOL_LIST, LV_SYMBOL_DRIVE,
                                      LV_SYMBOL_WIFI, LV_SYMBOL_IMAGE, LV_SYMBOL_SETTINGS};
  for (int i = 0; i < kSectionCount; i++) {
    lv_obj_t *tile = box(homeLayer, 248, 196);
    lv_obj_set_pos(tile, 16 + (i % 3) * 260, 12 + (i / 3) * 208);
    lv_obj_set_style_pad_all(tile, 18, 0);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1E1E22), LV_STATE_PRESSED);
    lv_obj_add_event_cb(tile, [](lv_event_t *e) { showSection((int)(intptr_t)lv_event_get_user_data(e)); },
                        LV_EVENT_CLICKED, (void *)(intptr_t)i);
    text(tile, &lv_font_montserrat_28, kAccent, icons[i]);
    lv_obj_t *t = text(tile, &lv_font_montserrat_28, kText, titles[i]);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 54);
    tileStatus[i] = text(tile, &lv_font_montserrat_16, kMuted, "");
    lv_obj_set_width(tileStatus[i], 210);
    lv_label_set_long_mode(tileStatus[i], LV_LABEL_LONG_DOT);
    lv_obj_align(tileStatus[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    tiles[i] = tile;
  }
}

void buildMain() {
  mainScreen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(mainScreen, lv_color_hex(kBg), 0);
  lv_obj_remove_flag(mainScreen, LV_OBJ_FLAG_SCROLLABLE);
  buildTopBar(mainScreen);
  buildHome();

  buildOverview(buildSection(kOverview, "Overview"));
  lv_obj_t *homelabBody = buildSection(kHomelab, "Homelab");
  lv_obj_add_flag(homelabBody, LV_OBJ_FLAG_SCROLLABLE);
  buildHomelab(homelabBody);
  proxmoxBody = buildSection(kProxmox, "Proxmox");
  buildNetwork(buildSection(kNetwork, "Network"));
  buildMinecraft(buildSection(kMinecraft, "Minecraft"));
  lv_obj_t *controlsBody = buildSection(kControls, "Controls");
  lv_obj_add_flag(controlsBody, LV_OBJ_FLAG_SCROLLABLE);
  buildControls(controlsBody);

  buildPageLayer();

  toast = text(lv_layer_top(), &lv_font_montserrat_20, kText, "");
  lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(toast, lv_color_hex(kCard), 0);
  lv_obj_set_style_border_width(toast, 2, 0);
  lv_obj_set_style_radius(toast, 12, 0);
  lv_obj_set_style_pad_hor(toast, 18, 0);
  lv_obj_set_style_pad_ver(toast, 10, 0);
  lv_obj_set_style_max_width(toast, 700, 0);
  lv_label_set_long_mode(toast, LV_LABEL_LONG_DOT);
  lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
  toastTimer = lv_timer_create([](lv_timer_t *t) {
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(t);
  }, kToastMs, nullptr);
  lv_timer_pause(toastTimer);
}

// --- Applying a snapshot ---------------------------------------------------

void applyStatus() {
  uint32_t border = kBorder, bg = kCard, titleColor = kText;
  String title, summary = snap->summary, detail;
  if (!snap->ready) {
    title = "Warming up";
    summary = "Arc hasn't finished its first checks yet.";
  } else if (snap->alerts.empty()) {
    title = "All clear";
    border = 0x14532D, bg = 0x0B1A10, titleColor = kOk;
    summary = "Everything's green. Nothing needs you.";
  } else {
    bool anyNew = snap->unacked > 0;
    title = String(snap->alerts.size()) + (snap->alerts.size() == 1 ? " alert" : " alerts");
    border = anyNew ? kAlert : kWarn;
    bg = anyNew ? 0x2A0B0B : 0x261A06;
    titleColor = anyNew ? kAlert : kWarn;
    summary = anyNew ? "Tap to acknowledge" : "Acknowledged, still watching";
    for (size_t i = 0; i < snap->alerts.size() && i < 4; i++) detail += "- " + snap->alerts[i].text + "\n";
  }
  lv_label_set_text(statusTitle, title.c_str());
  lv_obj_set_style_text_color(statusTitle, lv_color_hex(titleColor), 0);
  lv_label_set_text(statusSummary, summary.c_str());
  lv_label_set_text(statusDetail, detail.c_str());
  lv_obj_set_style_border_color(statusCard, lv_color_hex(border), 0);
  lv_obj_set_style_border_width(statusCard, snap->alerts.empty() ? 1 : 2, 0);
  lv_obj_set_style_bg_color(statusCard, lv_color_hex(bg), 0);

  int okCount = 0;
  for (const auto &s : snap->homelab) okCount += s.ok;
  lv_label_set_text_fmt(statHomelab, "%d/%d", okCount, (int)snap->homelab.size());
  lv_label_set_text(statMinecraft, snap->mcConfigured ? String(snap->mcOnline).c_str() : "-");
  lv_label_set_text(statDevices, String(snap->devices.size()).c_str());
}

void applyFeed() {
  String sig;
  for (const auto &e : snap->events) sig += e.ago + e.title + e.text;
  if (sig == feedSig) return;
  feedSig = sig;
  lv_obj_clean(feedList);
  if (snap->events.empty()) text(feedList, &lv_font_montserrat_16, kMuted, "Quiet so far.");
  for (const auto &e : snap->events) {
    lv_obj_t *item = bare(feedList);
    lv_obj_set_size(item, 280, LV_SIZE_CONTENT);
    column(item, 2);
    lv_obj_t *head = bare(item);
    lv_obj_set_size(head, 280, LV_SIZE_CONTENT);
    row(head, 8);
    dot(head, e.color, 10);
    lv_obj_t *t = text(head, &lv_font_montserrat_16, kText, e.title.c_str());
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(t, 1);
    text(head, &lv_font_montserrat_14, kMuted, e.ago.c_str());
    if (e.text.length()) {
      lv_obj_t *body = text(item, &lv_font_montserrat_14, kMuted, e.text.c_str());
      lv_obj_set_width(body, 262);
      lv_obj_set_style_pad_left(body, 18, 0);
      lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
      lv_obj_set_height(body, 18);
    }
  }
}

void applyHomelab() {
  String sig;
  for (const auto &s : snap->homelab) sig += s.name + s.value + (s.ok ? "1" : "0");
  if (sig == homelabSig) return;
  homelabSig = sig;
  lv_obj_clean(homelabList);
  if (snap->homelab.empty()) text(homelabList, &lv_font_montserrat_20, kMuted, "No homelab data yet.");
  for (const auto &s : snap->homelab) {
    lv_obj_t *item = box(homelabList, 384, 48);
    lv_obj_set_style_pad_ver(item, 0, 0);
    if (!s.ok) lv_obj_set_style_border_color(item, lv_color_hex(kAlert), 0);
    row(item, 10);
    dot(item, s.ok ? kOk : kAlert);
    lv_obj_t *name = text(item, &lv_font_montserrat_16, kText, s.name.c_str());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 170);
    lv_obj_t *value = text(item, &lv_font_montserrat_16, s.ok ? kMuted : kAlert, s.value.c_str());
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
  }
}

void applyMcLog() {
  String sig;
  for (const auto &l : snap->mcLog) sig += l.time + l.kind + l.player + l.text;
  if (sig == mcLogSig) return;
  mcLogSig = sig;
  lv_obj_clean(mcLogList);
  // Like Minecraft chat: newest line at the bottom, older ones pushed up.
  // The spacer soaks up spare height so a short log sits at the bottom
  // too; once the log overflows it collapses and the list scrolls instead.
  lv_obj_t *spacer = bare(mcLogList);
  lv_obj_set_width(spacer, 1);
  lv_obj_set_flex_grow(spacer, 1);
  if (snap->mcLog.empty()) {
    text(mcLogList, &lv_font_montserrat_14, kMuted, "Nothing logged yet.");
    return;
  }
  for (auto it = snap->mcLog.rbegin(); it != snap->mcLog.rend(); ++it) {
    const McLogLine &l = *it;
    uint32_t color = kText;
    String line = l.time + "  ";
    if (l.kind == "arc") color = 0x22D3EE, line += "[" + l.player + "] " + l.text;  // aqua, like the in-game [Arc] tag
    else if (l.kind == "chat") line += "<" + l.player + "> " + l.text;
    else if (l.kind == "command") color = 0xC084FC, line += l.player + " " + l.text;
    else if (l.kind == "join") color = kOk, line += l.player + " joined";
    else if (l.kind == "leave") color = kWarn, line += l.player + " left";
    else line += l.player + " " + l.text;
    lv_obj_t *label = text(mcLogList, &lv_font_montserrat_14, color, line.c_str());
    lv_obj_set_width(label, 340);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  }
  lv_obj_update_layout(mcLogList);
  lv_obj_scroll_to_view(lv_obj_get_child(mcLogList, -1), LV_ANIM_OFF);  // pin to the newest line
}

void applyMinecraft() {
  if (!snap->mcConfigured) {
    lv_label_set_text(mcState, "Not set up");
    lv_obj_set_style_text_color(mcState, lv_color_hex(kMuted), 0);
    lv_label_set_text(mcPlayers, "");
    lv_label_set_text(mcTps, "");
    return;
  }
  lv_label_set_text(mcState, snap->mcUp ? "Online" : "Offline");
  lv_obj_set_style_text_color(mcState, lv_color_hex(snap->mcUp ? kOk : kAlert), 0);
  String players = snap->mcOnline == 0 ? String("Nobody on") : String(snap->mcOnline) + " on: ";
  for (size_t i = 0; i < snap->mcPlayers.size(); i++) players += (i ? ", " : "") + snap->mcPlayers[i];
  lv_label_set_text(mcPlayers, players.c_str());
  String tps = "TPS";
  for (float t : snap->tps) tps += "  " + String(t, 1);
  lv_label_set_text(mcTps, snap->tps.empty() ? "" : tps.c_str());
  lv_obj_set_style_text_color(mcTps, lv_color_hex(snap->mcTpsOk ? kMuted : kWarn), 0);
  applyMcLog();
}

void section(const char *title) {
  lv_obj_t *l = text(controlsPage, &lv_font_montserrat_16, kMuted, title);
  lv_obj_set_style_pad_top(l, 6, 0);
}

lv_obj_t *buttonRow() {
  lv_obj_t *r = bare(controlsPage);
  lv_obj_set_size(r, 776, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(r, 10, 0);
  lv_obj_set_style_pad_column(r, 10, 0);
  return r;
}

void applyActions() {
  String sig;
  for (const auto &a : snap->actions) sig += a.id + ",";
  if (sig == actionsSig) return;
  actionsSig = sig;
  quietLabel = nullptr;

  lv_obj_clean(mcButtons);
  for (const auto &a : snap->actions)
    if (a.group == "Minecraft") actionButton(mcButtons, a, 190, 72);
  lv_obj_t *players = localButton(mcButtons, "Players  " LV_SYMBOL_RIGHT, [](lv_event_t *) { openPlayers(); });
  lv_obj_t *world = localButton(mcButtons, "World  " LV_SYMBOL_RIGHT, [](lv_event_t *) { openWorld(); });
  for (lv_obj_t *b : {players, world}) {
    lv_obj_set_size(b, 190, 72);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(kAccent), 0);
  }

  lv_obj_clean(controlsPage);
  const char *groups[] = {"Arc", "Restart", "Office"};
  for (const char *group : groups) {
    lv_obj_t *r = nullptr;
    for (const auto &a : snap->actions) {
      if (a.group != group) continue;
      if (!r) {
        section(group);
        r = buttonRow();
      }
      actionButton(r, a, 182, 64);
    }
  }
  section("Panel");
  lv_obj_t *r = buttonRow();
  localButton(r, LV_SYMBOL_WIFI " Wi-Fi", [](lv_event_t *) { showWifiSetup(); });
  localButton(r, LV_SYMBOL_EYE_CLOSE " Screen off", [](lv_event_t *) {
    screenOffByHand = true;
    board::setBacklight(false);
  });
  String build = "Firmware " + ESP.getSketchMD5().substring(0, 7) + "  |  " + WiFi.localIP().toString();
  text(controlsPage, &lv_font_montserrat_14, kMuted, build.c_str());
}

void applyQuiet() {
  if (!quietLabel) return;
  if (snap->quietSeconds > 0) lv_label_set_text_fmt(quietLabel, "Quiet: %dm left", (snap->quietSeconds + 59) / 60);
  else lv_label_set_text(quietLabel, "Quiet 1h");
}

// --- Status bar + backlight ---------------------------------------------------

void setTile(int section, const String &status, uint32_t color, uint32_t border = kBorder) {
  setText(tileStatus[section], status.c_str());
  setTextColor(tileStatus[section], color);
  setBorder(tiles[section], border, border == kBorder ? 1 : 2);
}

// Each home tile's one-line status, so the home screen is glanceable too.
void applyTiles() {
  if (!snap->ready) setTile(kOverview, "Warming up", kMuted);
  else if (snap->alerts.empty()) setTile(kOverview, "All clear", kOk);
  else {
    bool fresh = snap->unacked > 0;
    String n = String(snap->alerts.size()) + (snap->alerts.size() == 1 ? " alert" : " alerts");
    setTile(kOverview, fresh ? n : n + ", acknowledged", fresh ? kAlert : kWarn, fresh ? kAlert : kWarn);
  }

  int ok = 0;
  for (const auto &h : snap->homelab) ok += h.ok;
  int total = snap->homelab.size();
  setTile(kHomelab, String(ok) + " of " + String(total) + " checks ok", ok == total ? kOk : kAlert,
          ok == total ? kBorder : kAlert);

  if (!snap->proxmoxConfigured) setTile(kProxmox, "Needs a token", kWarn);
  else if (proxmoxHave) {
    int hosts = 0, guests = 0, running = 0;
    for (JsonObjectConst n : proxmoxDoc["nodes"].as<JsonArrayConst>()) hosts += (n["online"] | false) ? 1 : 0;
    for (JsonObjectConst g : proxmoxDoc["guests"].as<JsonArrayConst>()) {
      guests++;
      running += (g["running"] | false) ? 1 : 0;
    }
    setTile(kProxmox, String(hosts) + " hosts, " + String(running) + "/" + String(guests) + " running", kOk);
  } else setTile(kProxmox, "Hosts and containers", kMuted);

  if (!snap->netReady) setTile(kNetwork, "Waiting for UniFi", kMuted);
  else if (!snap->netOnline) setTile(kNetwork, "Internet down", kAlert, kAlert);
  else setTile(kNetwork, String(snap->netLatency) + " ms, " + String(snap->netDeviceCount) + " devices", kOk);

  if (!snap->mcConfigured) setTile(kMinecraft, "Not set up", kMuted);
  else if (!snap->mcUp) setTile(kMinecraft, "Offline", kAlert, kAlert);
  else setTile(kMinecraft, snap->mcOnline ? "Online, " + String(snap->mcOnline) + " playing" : String("Online, nobody on"), kOk);

  setTile(kControls, snap->quietSeconds > 0 ? String("Quiet mode on") : String("Restarts and tools"),
          snap->quietSeconds > 0 ? kWarn : kMuted);
}

void setPill(const String &label, uint32_t bg, uint32_t fg) {
  setText(pill, label.c_str());
  setBgColor(pill, bg);
  setTextColor(pill, fg);
}

void applyTopBar() {
  net::WifiState wifi = net::wifiState();
  setTextColor(wifiIcon, wifi == net::WifiState::Connected ? kText : kWarn);
  bool alerting = snap && snap->unacked > 0;
  setBgColor(topBar, alerting ? 0x450A0A : kBg);
  if (snap) setText(clockLabel, snap->time.c_str());

  if (net::updatingFirmware()) setPill("Updating firmware...", 0x0C2A3A, 0x7DD3FC);
  else if (wifi == net::WifiState::NoCredentials) setPill("Wi-Fi not set up", 0x3A2A06, kWarn);
  else if (wifi == net::WifiState::Connecting) setPill("Connecting to " + net::wifiSsid(), 0x3A2A06, kWarn);
  else if (!net::arcReachable()) setPill(snap ? "Arc unreachable" : "Reaching Arc...", 0x2A2A2E, kMuted);
  else if (!snap->ready) setPill("Warming up", 0x2A2A2E, kMuted);
  else if (alerting) setPill(snap->summary, kAlert, 0xFFFFFF);
  else if (!snap->ok) setPill(snap->summary, 0x3A2A06, kWarn);
  else if (snap->quietSeconds > 0) setPill("All normal - quiet mode", 0x0F2A18, kOk);
  else setPill("All systems normal", 0x0F2A18, kOk);
}

// Always on — it lives on USB power (it used to go dark overnight and when
// the owner's phone left the Wi-Fi, which wasn't wanted). The only way off is the
// Screen off button; a touch (board.cpp swallows it as wake-only) or a new
// alert brings it back.
void applyBacklight() {
  static int lastUnacked = 0;
  int unacked = snap ? snap->unacked : 0;
  bool newAlert = unacked > lastUnacked;
  lastUnacked = unacked;
  if (screenOffByHand) {
    if (board::backlightOn()) screenOffByHand = false;  // a touch woke it
    else if (newAlert) screenOffByHand = false;
    else return;
  }
  if (!board::backlightOn()) board::setBacklight(true);
}

}  // namespace

void begin() {
  lv_display_t *display = lv_display_get_default();
  lv_theme_t *theme = lv_theme_default_init(display, lv_color_hex(kAccent), lv_color_hex(kButton), true,
                                            &lv_font_montserrat_16);
  lv_display_set_theme(display, theme);
  buildMain();
  buildWifiScreen();
  lv_screen_load(mainScreen);
  if (net::wifiState() == net::WifiState::NoCredentials) showWifiSetup();
}

void update() {
  static uint32_t lastTick = 0;
  if (millis() - lastTick < 100) return;
  lastTick = millis();

  if (scanning) {
    std::vector<String> ssids;
    if (net::scanDone(ssids)) {
      scanning = false;
      populateWifiList(ssids);
    }
  }

  uint32_t g = net::generation();
  if (g != seenGeneration) {
    seenGeneration = g;
    snap = net::latest();
    if (snap) {
      applyStatus();
      applyFeed();
      applyHomelab();
      applyMinecraft();
      applyActions();
      applyQuiet();
      applyNetwork();
      applyTiles();
      // A new alert while nobody's using the panel: show it, don't wait to be asked.
      static int lastUnacked = 0;
      if (snap->unacked > lastUnacked && lv_display_get_inactive_time(nullptr) > 30000) showSection(kOverview);
      lastUnacked = snap->unacked;
    }
  }

  String message;
  bool ok;
  if (net::takeResult(message, ok)) {
    showToast(message.length() ? message : String(ok ? "Done" : "Failed"), ok ? kOk : kAlert);
    if (openPage == Page::World) net::fetchDetail("/panel/minecraft/world");  // show the rule as it really is now
  }

  handleDetail();
  if (currentSection == kProxmox && millis() - proxmoxFetchedAt > kProxmoxRefreshMs) fetchProxmox();
  // Left alone for a while: drift back to the home tiles (unless an alert is showing).
  if (currentSection != kHome && lv_display_get_inactive_time(nullptr) > kHomeAfterMs &&
      !(snap && snap->unacked > 0 && currentSection == kOverview))
    showHome();
  if (openPage == Page::World && millis() - pageFetchedAt > kWorldRefreshMs) {
    pageFetchedAt = millis();
    net::fetchDetail("/panel/minecraft/world");
  }

  applyTopBar();
  applyBacklight();
}

}  // namespace ui
