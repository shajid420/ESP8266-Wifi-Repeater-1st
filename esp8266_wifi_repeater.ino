/*
  ===========================================================================
  ESP8266 Wi-Fi Repeater (STA + AP, with REAL lwIP NAPT routing)
  ===========================================================================

  This is a NAT-router-style repeater, NOT a Layer-2/WDS transparent
  repeater (the ESP8266 SDK does not expose WDS/4-address-frame support,
  so a transparent same-SSID repeater is not achievable with this chip).

  Architecture:
    Upstream router  <---- STA link ---->  ESP8266  <---- AP link ---->  Phone/laptop
                                          (NAPT / NAT here)

  The ESP8266 joins your existing Wi-Fi as a normal client (STA), and at
  the same time runs its own access point (AP) with a different SSID and
  a different IP subnet. lwIP's NAPT (Network Address & Port Translation)
  rewrites packets between the two interfaces so AP clients get real
  internet access through the STA link -- this is the same architecture
  used by "travel routers" / MiFi devices, and is what actually forwards
  traffic (as opposed to just creating an AP that goes nowhere).

  ---------------------------------------------------------------------
  REQUIRED Arduino IDE build settings (Tools menu) -- read this first:
  ---------------------------------------------------------------------
    Board:        NodeMCU 1.0 (ESP-12E Module)   [or "Generic ESP8266 Module"
                   if you're using an ESP-07S board -- see README]
    Flash Size:   4MB (FS:none, or any split that keeps >=1MB for sketch)
    lwIP Variant: "v2 Higher Bandwidth"                <-- MUST NOT be a
                                                            "(no features)"
                                                            or "IPv6" variant.
                  The "(no features)" variants strip out IP forwarding/NAT
                  support entirely -- the sketch will compile but NAPT will
                  silently do nothing. "IPv6" variants also disable the
                  NAPT code path used here (LWIP_IPV6 must be 0).
    CPU Frequency: 160 MHz (reduces latency/jitter under load)
    Debug port:    Disabled (saves RAM)

  Libraries used are all part of the stock esp8266/Arduino core (>=2.6.0,
  tested against 3.x) -- no third-party NAT library needed.
  ===========================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <string.h>
#include <lwip/napt.h>
#include <lwip/dns.h>

#if !LWIP_FEATURES || LWIP_IPV6
#error "Wrong lwIP variant selected. Tools > lwIP Variant must be a non-IPv6, " \
       "non-'(no features)' variant (e.g. 'v2 Higher Bandwidth') so NAPT is available."
#endif

// ---------------------------------------------------------------------
// NAPT table sizing -- tuned for ONE TO A FEW client devices.
//   NAPT_MAX_ENTRIES  = max simultaneous NAT sessions (TCP+UDP flows).
//                        A phone doing a voice call + YouTube typically
//                        uses well under 100 flows; 512 leaves headroom
//                        for a browser tab or two without wasting RAM.
//   NAPT_MAX_PORTMAPS = max port-forwarding rules (not used by this
//                        sketch; kept small on purpose).
// Raise NAPT_MAX_ENTRIES (e.g. to 1000, the upstream example's default)
// if pages fail to load because many parallel connections are in use.
// ---------------------------------------------------------------------
#define NAPT_MAX_ENTRIES   512
#define NAPT_MAX_PORTMAPS  10

// ---------------------------------------------------------------------
// Defaults -- only used on first boot / if EEPROM config is invalid.
// Everything below can be changed later from the web config page.
// ---------------------------------------------------------------------
#define DEFAULT_ROUTER_SSID  "YourHomeRouterSSID"
#define DEFAULT_ROUTER_PASS  "YourHomeRouterPassword"
#define DEFAULT_AP_SSID      "ESP8266-Repeater"
#define DEFAULT_AP_PASS      "repeater123"     // WPA2 requires >= 8 chars
#define DEFAULT_AP_CHANNEL   6

#define CONFIG_MAGIC 0x52504931UL // "RPI1" -- bump this if you change the struct

struct Config {
  uint32_t magic;
  char routerSsid[33];
  char routerPass[65];
  char apSsid[33];
  char apPass[65];
  uint8_t apChannel;
};

Config cfg;
ESP8266WebServer server(80);

bool naptStarted = false;

// reconnect / watchdog state
unsigned long lastStaCheckMs = 0;
unsigned long staDownSinceMs = 0;
bool staWasConnected = false;
const unsigned long STA_CHECK_INTERVAL_MS = 5000;         // poll every 5s
const unsigned long STA_MAX_DOWNTIME_MS   = 120UL * 1000; // hard restart after 2 min down

// =======================================================================
// EEPROM config
// =======================================================================
void loadConfig() {
  EEPROM.begin(512);
  EEPROM.get(0, cfg);
  if (cfg.magic != CONFIG_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC;
    strlcpy(cfg.routerSsid, DEFAULT_ROUTER_SSID, sizeof(cfg.routerSsid));
    strlcpy(cfg.routerPass, DEFAULT_ROUTER_PASS, sizeof(cfg.routerPass));
    strlcpy(cfg.apSsid,     DEFAULT_AP_SSID,     sizeof(cfg.apSsid));
    strlcpy(cfg.apPass,     DEFAULT_AP_PASS,     sizeof(cfg.apPass));
    cfg.apChannel = DEFAULT_AP_CHANNEL;
    EEPROM.put(0, cfg);
    EEPROM.commit();
    Serial.println(F("[CFG] No valid config found, wrote defaults."));
  } else {
    Serial.println(F("[CFG] Loaded config from EEPROM."));
  }
}

void saveConfig() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// =======================================================================
// Wi-Fi bring-up
// =======================================================================
void startStation() {
  WiFi.begin(cfg.routerSsid, cfg.routerPass);
  Serial.printf("[STA] Connecting to '%s'...\n", cfg.routerSsid);
}

void startAP() {
  // Private /24 chosen to avoid colliding with common home router ranges
  // (192.168.0.x, 192.168.1.x) so double-NAT routing doesn't get confused.
  IPAddress apIP(192, 168, 44, 1);
  IPAddress apGW(192, 168, 44, 1);
  IPAddress apMask(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apGW, apMask);
  // max 4 simultaneous AP clients -- keeps NAT/CPU load appropriate for
  // "one or a few devices"; raise if you really need more.
  WiFi.softAP(cfg.apSsid, cfg.apPass, cfg.apChannel, false, 4);
  Serial.printf("[AP] '%s' up on channel %d, IP %s\n",
                cfg.apSsid, cfg.apChannel, WiFi.softAPIP().toString().c_str());
}

// Must be called only after STA is actually connected (NAPT needs to know
// the real upstream DNS server, obtained via DHCP from the router).
void startNaptIfReady() {
  if (naptStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  // Point AP-side DHCP clients at the router's real DNS server instead of
  // at the ESP8266 itself.
  auto &dhcp = WiFi.softAPDhcpServer();
  dhcp.setDns(WiFi.dnsIP(0));

  err_t ret = ip_napt_init(NAPT_MAX_ENTRIES, NAPT_MAX_PORTMAPS);
  Serial.printf("[NAPT] ip_napt_init -> %d (OK=%d)\n", (int)ret, (int)ERR_OK);
  if (ret == ERR_OK) {
    ret = ip_napt_enable_no(SOFTAP_IF, 1);
    Serial.printf("[NAPT] ip_napt_enable_no(SOFTAP_IF) -> %d\n", (int)ret);
  }
  naptStarted = (ret == ERR_OK);
  if (naptStarted) {
    Serial.println(F("[NAPT] Internet forwarding is ACTIVE."));
  } else {
    Serial.println(F("[NAPT] FAILED to enable NAT -- check lwIP Variant build setting!"));
  }
}

// =======================================================================
// Reconnect / watchdog logic
// =======================================================================
void handleStaSupervision() {
  unsigned long now = millis();
  if (now - lastStaCheckMs < STA_CHECK_INTERVAL_MS) return;
  lastStaCheckMs = now;

  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected) {
    if (!staWasConnected) {
      Serial.printf("[STA] Connected. IP=%s RSSI=%ddBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    staWasConnected = true;
    staDownSinceMs = 0;
    startNaptIfReady();
    return;
  }

  // Not connected right now.
  if (staWasConnected) {
    // Just dropped.
    Serial.println(F("[STA] Link lost. Attempting reconnect..."));
    staWasConnected = false;
    staDownSinceMs = now;
  }
  if (staDownSinceMs == 0) staDownSinceMs = now;

  WiFi.reconnect(); // cheap retry using stored credentials

  unsigned long downFor = now - staDownSinceMs;
  Serial.printf("[STA] Still down for %lus\n", downFor / 1000);

  if (downFor >= STA_MAX_DOWNTIME_MS) {
    Serial.println(F("[STA] Down too long -- restarting device to recover."));
    delay(200);
    ESP.restart();
  }
}

// =======================================================================
// Small HTML helpers
// =======================================================================
String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    switch (c) {
      case '&':  out += F("&amp;");  break;
      case '"':  out += F("&quot;"); break;
      case '<':  out += F("&lt;");   break;
      case '>':  out += F("&gt;");   break;
      default:   out += c;
    }
  }
  return out;
}

String pageHeader(const String &title) {
  String s;
  s += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>");
  s += title;
  s += F("</title><style>"
         "body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 12px;background:#111;color:#eee}"
         "h1{font-size:1.2em}"
         "label{display:block;margin-top:10px;font-size:.9em;color:#aaa}"
         "input{width:100%;box-sizing:border-box;padding:8px;margin-top:4px;"
         "background:#222;border:1px solid #444;color:#eee;border-radius:4px}"
         "button,a.btn{display:inline-block;margin-top:16px;padding:10px 16px;"
         "background:#2d7;border:none;color:#000;border-radius:4px;text-decoration:none;"
         "font-weight:bold;cursor:pointer}"
         "a.btn.secondary{background:#555;color:#eee}"
         ".row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #333}"
         ".ok{color:#2d7}.bad{color:#e55}"
         "</style></head><body>");
  return s;
}
const char *PAGE_FOOTER = "</body></html>";

// =======================================================================
// Web handlers
// =======================================================================
void handleRoot() {
  bool staUp = (WiFi.status() == WL_CONNECTED);
  String s = pageHeader(F("Repeater status"));
  s += F("<meta http-equiv='refresh' content='5'>"); // auto-refresh status
  s += F("<h1>ESP8266 Repeater</h1>");

  s += F("<div class='row'><span>Upstream (STA) link</span><span class='");
  s += staUp ? F("ok'>Connected") : F("bad'>Disconnected");
  s += F("</span></div>");

  s += F("<div class='row'><span>Router SSID</span><span>");
  s += htmlEscape(cfg.routerSsid);
  s += F("</span></div>");

  s += F("<div class='row'><span>Signal (RSSI)</span><span>");
  s += staUp ? (String(WiFi.RSSI()) + " dBm") : String("--");
  s += F("</span></div>");

  s += F("<div class='row'><span>STA IP address</span><span>");
  s += staUp ? WiFi.localIP().toString() : String("--");
  s += F("</span></div>");

  s += F("<div class='row'><span>NAT / internet forwarding</span><span class='");
  s += naptStarted ? F("ok'>Active") : F("bad'>Inactive");
  s += F("</span></div>");

  s += F("<div class='row'><span>Repeater SSID</span><span>");
  s += htmlEscape(cfg.apSsid);
  s += F("</span></div>");

  s += F("<div class='row'><span>Repeater (AP) IP</span><span>");
  s += WiFi.softAPIP().toString();
  s += F("</span></div>");

  s += F("<div class='row'><span>Connected devices</span><span>");
  s += String(WiFi.softAPgetStationNum());
  s += F("</span></div>");

  s += F("<div class='row'><span>Uptime</span><span>");
  s += String(millis() / 1000) + " s";
  s += F("</span></div>");

  s += F("<a class='btn' href='/config'>Edit configuration</a> ");
  s += F("<a class='btn secondary' href='/restart' onclick=\"return confirm('Restart the repeater now?')\">Restart</a>");
  s += PAGE_FOOTER;
  server.send(200, "text/html", s);
}

void handleConfigForm() {
  String s = pageHeader(F("Configure repeater"));
  s += F("<h1>Configuration</h1>"
         "<form method='POST' action='/save'>"
         "<label>Main router SSID</label>"
         "<input name='router_ssid' maxlength='32' value='");
  s += htmlEscape(cfg.routerSsid);
  s += F("' required>"
         "<label>Main router password (leave as-is to keep unchanged)</label>"
         "<input name='router_pass' maxlength='64' type='password' placeholder='********'>"
         "<label>Repeater SSID</label>"
         "<input name='ap_ssid' maxlength='32' value='");
  s += htmlEscape(cfg.apSsid);
  s += F("' required>"
         "<label>Repeater password (min 8 chars, leave as-is to keep unchanged)</label>"
         "<input name='ap_pass' maxlength='64' type='password' placeholder='********'>"
         "<label>Repeater Wi-Fi channel (1-13)</label>"
         "<input name='ap_channel' type='number' min='1' max='13' value='");
  s += String(cfg.apChannel);
  s += F("'>"
         "<button type='submit'>Save &amp; restart</button>"
         "</form>"
         "<p><a class='btn secondary' href='/'>Back</a></p>");
  s += PAGE_FOOTER;
  server.send(200, "text/html", s);
}

void handleSave() {
  String routerSsid = server.arg("router_ssid");
  String routerPass = server.arg("router_pass");
  String apSsid     = server.arg("ap_ssid");
  String apPass     = server.arg("ap_pass");
  String apChanStr  = server.arg("ap_channel");

  if (routerSsid.length() == 0 || apSsid.length() == 0) {
    server.send(400, "text/plain", "SSID fields cannot be empty.");
    return;
  }
  if (routerPass.length() > 0 && routerPass.length() < 8) {
    server.send(400, "text/plain", "Router password must be empty (open network) or >= 8 characters.");
    return;
  }
  if (apPass.length() > 0 && apPass.length() < 8) {
    server.send(400, "text/plain", "Repeater password must be >= 8 characters (WPA2 minimum).");
    return;
  }

  strlcpy(cfg.routerSsid, routerSsid.c_str(), sizeof(cfg.routerSsid));
  if (routerPass.length() > 0) {
    strlcpy(cfg.routerPass, routerPass.c_str(), sizeof(cfg.routerPass));
  }
  strlcpy(cfg.apSsid, apSsid.c_str(), sizeof(cfg.apSsid));
  if (apPass.length() > 0) {
    strlcpy(cfg.apPass, apPass.c_str(), sizeof(cfg.apPass));
  }
  int ch = apChanStr.toInt();
  if (ch >= 1 && ch <= 13) cfg.apChannel = (uint8_t)ch;

  saveConfig();

  String s = pageHeader(F("Saved"));
  s += F("<h1>Saved</h1><p>Configuration written. Restarting now...</p>");
  s += PAGE_FOOTER;
  server.send(200, "text/html", s);
  delay(400);
  ESP.restart();
}

void handleRestart() {
  String s = pageHeader(F("Restarting"));
  s += F("<h1>Restarting...</h1><p>The device will reboot now.</p>");
  s += PAGE_FOOTER;
  server.send(200, "text/html", s);
  delay(300);
  ESP.restart();
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// =======================================================================
// setup / loop
// =======================================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n\nESP8266 Wi-Fi Repeater booting..."));

  loadConfig();

  WiFi.mode(WIFI_AP_STA);

  // Disabling modem sleep is the single biggest latency/jitter win on the
  // ESP8266 -- without this, the radio periodically naps to save power,
  // which adds tens of ms of jitter that hurts VoIP calls noticeably.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  // Reconnect automatically at the driver level too (belt-and-suspenders
  // alongside the manual supervision loop below).
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false); // we manage credentials ourselves via EEPROM

  startAP();
  startStation();

  MDNS.begin("repeater"); // reachable at http://repeater.local/

  server.on("/", handleRoot);
  server.on("/config", HTTP_GET, handleConfigForm);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/restart", handleRestart);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println(F("Web config server started on port 80."));
}

void loop() {
  server.handleClient();
  MDNS.update();
  handleStaSupervision();
}
