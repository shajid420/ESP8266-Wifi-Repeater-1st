# ESP8266 Wi-Fi Repeater — Build Guide

A real NAT-based repeater: the ESP8266 joins your router as a Wi-Fi client
(STA) and simultaneously runs its own access point (AP), with lwIP's NAPT
forwarding actual internet traffic between the two — not just an AP that
goes nowhere.

---

## 1. Verification: does this actually forward traffic?

Yes. The firmware uses `ip_napt_init()` / `ip_napt_enable_no(SOFTAP_IF, 1)`
from `lwip/napt.h`, which has shipped as a **standard part of the
esp8266/Arduino core since release 2.6.0** (no third-party NAT library
needed). It's the same mechanism used by the core's own bundled example,
`RangeExtender-NAPT.ino`, and by community "repeater" projects such as
`martin-ger/esp_wifi_repeater`. This is real Network Address & Port
Translation: packets from AP clients get their source IP/port rewritten to
the ESP8266's STA IP before being sent upstream, and replies get rewritten
back — the same technique any NAT router uses.

**The one thing that can silently defeat this:** the Arduino IDE's
`Tools > lwIP Variant` setting. Variants labelled **"(no features)"**
compile out IP forwarding/NAT entirely, and **IPv6** variants also disable
this code path. You must pick a plain non-IPv6, "with features" variant
(the sketch enforces this at compile time with a `#error` if you pick
wrong).

---

## 2. A. Recommended architecture

```
[Home Router] <==Wi-Fi STA link==> [ESP8266] <==Wi-Fi AP link==> [Phone/Laptop]
   192.168.x.x                    NAPT/NAT here        192.168.44.x (new subnet)
```

- **STA interface**: ESP8266 connects to your existing router like any
  normal Wi-Fi client, gets a DHCP lease and real DNS servers from it.
- **AP interface**: ESP8266 runs its own SSID on a separate `192.168.44.0/24`
  subnet, with its own DHCP server for connected clients.
- **NAPT**: rewrites packets between the two interfaces so AP clients reach
  the internet through the STA link. This is "double NAT" (router NATs
  once, ESP8266 NATs again) — normal for this kind of device and fine for
  browsing/streaming/calls; it just means AP-side devices aren't reachable
  by inbound connections from the router's LAN without port forwarding.

### Mode terminology, precisely

| Term | What it means | Available on ESP8266? |
|---|---|---|
| **STA mode** | Wi-Fi client, joins someone else's AP | Yes |
| **AP mode** | Device creates its own SSID | Yes |
| **AP+STA (what we use)** | Both simultaneously, one radio, time-shared | Yes |
| **NAT router mode** | AP+STA plus NAPT rewriting packets between them | Yes — this project |
| **True Wi-Fi repeater / WDS** | Retransmits the *same* SSID/BSSID at Layer 2 (4-address frames), transparent to clients, no new subnet | **No** — the ESP8266 SDK's closed-source Wi-Fi driver does not expose WDS/4-address-frame support to Arduino code. This is a hardware/SDK limitation, not something fixable in the sketch. |

So: what you get is a genuine, traffic-forwarding repeater — just on a
second subnet/SSID, not a transparent extension of your existing SSID.
Devices must be manually connected to the ESP8266's SSID; they won't
auto-roam between the router and the repeater.

---

## 3. B. Hardware

| Item | Notes |
|---|---|
| ESP8266 board | NodeMCU (ESP-12E/ESP-12F module) **or** ESP-07S if you need an external antenna (see §7) |
| USB cable / USB-serial adapter | NodeMCU has one built in; a bare ESP-07S needs a separate CP2102/CH340 adapter |
| 5V USB power supply | 1A+ recommended; Wi-Fi TX bursts draw noticeably more current than idle |
| (Optional) External 2.4 GHz antenna + SMA pigtail | Only usable with a U.FL/IPEX-equipped module — see §7 |

No extra microcontroller, no Ethernet, no Raspberry Pi involved — all
logic (Wi-Fi driver, TCP/IP stack, NAT, web server) runs on the ESP8266's
own Tensilica L106 core.

---

## 4. C. Firmware recommendation

**Stock esp8266/Arduino core (Arduino IDE or PlatformIO), using the
built-in lwIP NAPT feature.** This is deliberately chosen over:

- **martin-ger/esp_wifi_repeater** (standalone ESP-IDF-based firmware) —
  more configurable but a separate build system, harder to customize, and
  its functionality is now essentially built into the mainline Arduino
  core anyway.
- **Custom closed binaries** — no need; NAPT support has been mainline
  since 2019.

The sketch in this folder (`esp8266_wifi_repeater.ino`) is the complete
implementation: STA+AP+NAPT+DHCP+WPA2+web config+auto-reconnect+watchdog.

---

## 5. E. Arduino IDE setup

1. Install Arduino IDE (2.x recommended).
2. `File > Preferences > Additional Boards Manager URLs`, add:
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`
3. `Tools > Board > Boards Manager`, search `esp8266`, install "esp8266 by
   ESP8266 Community" (latest 3.x).
4. Open `esp8266_wifi_repeater.ino`.
5. Set these under `Tools`:
   - **Board**: `NodeMCU 1.0 (ESP-12E Module)` (or `Generic ESP8266
     Module` for ESP-07S)
   - **lwIP Variant**: **`v2 Higher Bandwidth`** — do NOT pick any
     "(no features)" or "IPv6" option
   - **Flash Size**: `4MB (FS:none, OTA:~1019KB)` (or similar — sketch
     doesn't use a filesystem)
   - **CPU Frequency**: `160 MHz`
   - **Upload Speed**: `921600` (or `115200` if uploads fail)
   - **Debug port**: `Disabled`

### PlatformIO equivalent (`platformio.ini`)

```ini
[env:nodemcuv2]
platform = espressif8266
board = nodemcuv2
framework = arduino
board_build.f_cpu = 160000000L
build_flags = -DPIO_FRAMEWORK_ARDUINO_LWIP2_HIGHER_BANDWIDTH
monitor_speed = 115200
```

---

## 6. F. Flashing instructions

1. Connect the board via USB.
2. Select the correct `Tools > Port`.
3. Click Upload. On a NodeMCU the DTR/RTS auto-reset usually handles
   entering flash mode automatically. On a bare ESP-07S you must manually
   pull `GPIO0` low and pulse `RST` to enter flash mode, then release
   `GPIO0` after flashing.
4. Open `Tools > Serial Monitor` at 115200 baud to watch boot logs —
   the sketch prints STA connection status, NAPT init result, and AP
   client count.
5. On **first boot**, the device uses the placeholder credentials baked
   into the sketch (`DEFAULT_ROUTER_SSID` etc.) and will fail to join
   your router. That's expected — proceed to configuration below.

---

## 7. H. Antenna / wiring — read before buying hardware

**Standard NodeMCU boards (ESP-12E/ESP-12F module) have only a printed
PCB trace antenna etched directly onto the module.** There is no antenna
connector, no coax pad, and no supported way to attach an external
antenna without physically reworking the module — not something
Espressif documents, and it risks detuning the RF matching network and
invalidating the module's radio certification. **Don't attempt it, and
never solder a wire antenna to a GPIO pin** — GPIOs are baseband digital
I/O with no RF matching network; an antenna there won't radiate correctly
and the static/RF coupling can damage the pin.

**If you need a real external antenna, use a different module: ESP-07S**
(same ESP8266EX chip, same Arduino "Generic ESP8266 Module" profile).
It has a genuine **U.FL/IPEX connector** feeding a matched RF path, plus
an onboard ceramic chip antenna. Notes:
- Only one antenna path should be active at a time. Most ESP-07S boards
  select between the ceramic antenna and the U.FL connector via a small
  0-ohm resistor/solder-jumper near the connector — check your board's
  silkscreen/datasheet and bridge only the U.FL side.
- Connect a **U.FL-to-SMA pigtail**, then screw on a certified 2.4 GHz
  antenna to the SMA end. Never connect the antenna element directly to
  the bare U.FL pad without the proper connector — U.FL connectors are
  fragile and not meant for repeated direct antenna swaps.
- ESP-07S is a bare module: you'll need a breakout/adapter board or your
  own wiring for 3.3V power (500 mA+, NOT 5V — the module is not
  5V-tolerant), `EN`/`CH_PD` pulled high, `GPIO15` pulled low, and a
  USB-serial adapter for flashing (it has no onboard USB).

---

## 8. G. Configuration instructions

1. After flashing, power the board. It boots its AP with the default
   SSID `ESP8266-Repeater` / password `repeater123` even though it can't
   reach the internet yet.
2. From a phone/laptop, join that SSID.
3. Browse to `http://192.168.44.1/` (or `http://repeater.local/` if your
   OS supports mDNS) — you'll see the status page.
4. Click **Edit configuration**, enter:
   - Your real router's SSID and password
   - A new repeater SSID/password if desired (min 8 characters, WPA2)
   - Wi-Fi channel (1–13; see §10 note on channel behavior)
5. Save. The device restarts, joins your router, and NAT comes up
   automatically — watch the status page's "NAT / internet forwarding"
   row flip to **Active**.
6. To change settings later, join the *repeater's* SSID (not the router's)
   and go back to `http://192.168.44.1/config`.

Fields left as `********` (password fields) on the form are left
unchanged — you don't have to retype a password just to change the SSID.

---

## 9. Auto-reconnect & failure recovery (already built in)

- Every 5 seconds the firmware checks `WiFi.status()`.
- If the STA link drops, it calls `WiFi.reconnect()` repeatedly.
- If it stays down for **2 continuous minutes**, the firmware assumes
  something is wedged (not just a brief router hiccup) and calls
  `ESP.restart()` for a clean recovery.
- NAPT is (re-)armed automatically the moment the STA link is confirmed
  connected — no NAT-specific recovery logic is needed since the NAT
  binding is to the *interface*, not the specific connection instance.

---

## 10. J/K. Expected performance, and single-radio limitations

The ESP8266 has **one 2.4 GHz radio, time-shared between AP and STA
roles** — this shapes everything below.

**Throughput**: expect roughly **1–4 Mbps of sustained real-world TCP
throughput** through the repeater, even though the 802.11 PHY nominally
supports much more. The bottleneck isn't the radio's raw bitrate — it's
the single 80/160 MHz core doing Wi-Fi driver work, the lwIP stack, NAT
table lookups, *and* the web server, all in time-sliced fashion. That's
still comfortably enough for:
- WhatsApp/Messenger voice calls (~24–100 kbps)
- One phone streaming YouTube at up to ~720p/1080p with normal buffering

It is **not** enough for multiple simultaneous HD streams, large file
transfers, or many browser tabs open at once (each tab can open 6–8
parallel TCP connections, which adds up against the NAT table quickly)
— hence this build being deliberately tuned for "one or a few devices."

**Single-radio limitations to know about:**

1. **AP and STA are forced onto the same Wi-Fi channel.** There's only
   one radio, so when the STA interface connects to your router, the
   SDK forces the AP interface onto that same channel — even if you set
   a different channel in configuration. If your router changes channel
   (auto-channel-select routers do this), already-connected AP clients
   can see a brief (sub-second to a few seconds) drop while the ESP8266
   re-syncs. Setting your router to a **fixed, non-auto channel** avoids
   this.
2. **Airtime is shared, not doubled.** Every packet in either direction
   uses the same radio, so uplink and downlink effectively compete for
   airtime — unlike a proper repeater/router with two separate radios.
3. **Limited RAM (~30–50 KB typical free heap)** caps how many
   simultaneous NAT sessions and AP clients are practical — this is why
   the sketch limits AP clients to 4 and NAT table entries to 512 by
   default rather than maximizing both.
4. **No WMM/QoS control** is exposed by the Arduino core, so the
   firmware can't explicitly prioritize voice packets over other
   traffic — it relies on there being little competing traffic, which
   holds true for "one phone, a couple of apps."
5. **WPA2-PSK only** on the AP side (no WPA3, no enterprise auth).
6. **Only 2.4 GHz**, no 5 GHz — as specified for this build already.

**How to get the best results:**
- Place the ESP8266 where its STA-side signal to the router is strong
  (the status page shows live RSSI — aim for better than about -65 dBm).
- Fix your router to a non-overlapping 2.4 GHz channel (1, 6, or 11) and
  disable auto-channel to avoid the AP-channel-follows-STA glitch above.
- Keep the number of AP clients to one or two.
- Don't leave the web config page open/refreshing while making a call —
  it adds a small, avoidable amount of CPU/radio contention.
- `WiFi.setSleepMode(WIFI_NONE_SLEEP)` (already in the sketch) is the
  single biggest latency win available in software — modem sleep
  otherwise adds tens of ms of jitter, which is very noticeable on VoIP.
- Power the board from a solid 5V/1A+ source — brownouts during Wi-Fi TX
  bursts are a common cause of random resets on marginal supplies/cables.

---

## 11. I. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Status page shows NAT "Inactive" forever | Wrong `lwIP Variant` selected — must not be a "(no features)" or IPv6 variant. Re-flash after fixing the Tools menu setting. |
| AP clients connect but have no internet | Check STA status first — if STA itself isn't "Connected," fix the router credentials via `/config`. If STA *is* connected but NAT still shows inactive, see above. |
| Can't reach `192.168.44.1` after joining the AP | Some phones cache an old IP or block "no internet" networks from resolving — try `http://repeater.local` or manually check the IP your phone was assigned (Settings > Wi-Fi > network details). |
| AP briefly drops every so often | Likely the router changed channel and forced the AP to follow (see §10.1) — pin your router to a fixed channel. |
| Device reboots randomly under load | Usually power supply/cable — try a different 5V/1A+ supply and a short, quality USB cable. |
| Voice calls choppy despite good signal | Confirm only one or two devices are on the AP; close other bandwidth-heavy apps; verify RSSI on the status page is better than ~-70 dBm. |
| Forgot the SSID/password you set | Re-flash the sketch (this resets EEPROM only if you change `CONFIG_MAGIC`), or connect via serial and add a temporary `Serial.println` of `cfg` fields in `setup()` to read them back. |
| Upload fails / "Failed to connect to ESP8266" | Hold/flash-mode issue — on NodeMCU, try a slower upload speed (115200); on bare ESP-07S, confirm GPIO0 is pulled low during reset. |

---

## Files in this folder

- `esp8266_wifi_repeater.ino` — complete firmware (flash this)
- `README.md` — this guide
