# Room Temperature & Humidity Monitor — 10× ESP8266 + HiveMQ + Google Sheets + GitHub Pages

End-to-end IoT stack for **up to 10 (or more) NodeMCU/Wemos D1 mini** sensor nodes.

```
                        ┌─────────────┐
DHT11 ─► ESP8266 #1 ───►│             │
DHT11 ─► ESP8266 #2 ───►│  HiveMQ     │◄── WSS ──► GitHub Pages dashboard
   ...                  │  Cloud      │             (grid + per-device chart)
DHT11 ─► ESP8266 #10───►│  (MQTT TLS) │
                        └─────────────┘
              │
              │ HTTPS (every 30 min, per device)
              ▼
        Google Apps Script  ──►  Google Sheet (timestamp, device, temp, hum, rssi)
```

## Topic plan

Every device publishes to **`<base>/<device_id>`** (default base = `room`):

| Device | MQTT topic | Status (LWT) topic |
|---|---|---|
| room1  | `room/room1`  | `room/room1/status`  |
| room2  | `room/room2`  | `room/room2/status`  |
| …      | …             | …                    |
| room10 | `room/room10` | `room/room10/status` |

The dashboard subscribes to `room/+` and `room/+/status`. To add an 11th device, just flash another board with a unique `device_id` — no code changes needed anywhere.

Payload (retained on each device topic):

```json
{ "device":"room3", "temp":24.7, "hum":58, "rssi":-61, "uptime":1234 }
```

Status messages are `online` / `offline` (LWT-based) and are also retained. Nothing else is ever published there, so a subscriber can read `<base>/<device>/status` as a plain online flag.

Command acks and sensor events go to a separate, **non-retained** topic `<base>/<device>/ack` (`sensor_fault`, `sensor_ok`, `offset t=… h=…`) — an event that outlives the condition it reports has no business sitting retained on the status topic. Commands are received on `<base>/<device>/cmd` (`read`, `reboot`, `offset <T> <H>`).

## Repository layout

| Path | What |
|---|---|
| [firmware/Room_temp/Room_temp.ino](firmware/Room_temp/Room_temp.ino) | Arduino sketch for the ESP8266 |
| [firmware/platformio.ini](firmware/platformio.ini) | PlatformIO build config (NodeMCU / D1 mini / ESP-12E) |
| [google-apps-script/Code.gs](google-apps-script/Code.gs) | Web-app that logs JSON into Google Sheets |
| [dashboard/index.html](dashboard/index.html) | Static dashboard for GitHub Pages |

---

## 1. Hardware

Pins are referenced by **GPIO number**, so the same sketch runs on a bare ESP-12E module, a NodeMCU, or a Wemos D1 mini.

| Signal | GPIO | NodeMCU / D1 mini label |
|---|---|---|
| DHT11 DATA | GPIO2 | D4 |
| OLED SDA   | GPIO4 | D2 |
| OLED SCL   | GPIO5 | D1 |
| FLASH btn  | GPIO0 | D3 (hold LOW at boot → portal) |
| 3V3 / GND  | —     | — |

### Bare ESP-12E module wiring
   
If you are flashing a **raw ESP-12E** (not a dev board), you also need the standard boot-strap resistors and a USB-TTL adapter:

| ESP-12E pin | Connect to |
|---|---|
| VCC, EN (CH_PD) | 3V3 (use a stable LDO, ≥ 500 mA) |
| GND, GPIO15     | GND (10 kΩ pulldown on GPIO15) |
| GPIO2           | 3V3 via 10 kΩ (also DHT11 DATA) |
| GPIO0           | 3V3 via 10 kΩ; pull LOW + reset to flash |
| RST             | 3V3 via 10 kΩ; reset button to GND |
| TX (GPIO1)      | RX of USB-TTL |
| RX (GPIO3)      | TX of USB-TTL |

Flashing sequence on a bare module: hold GPIO0 LOW → tap RST → release GPIO0 → run `pio run -t upload`.

## 2. ESP8266 firmware

You can build the **same** sketch with either toolchain.

### Option A — Arduino IDE

Install these libraries from the Arduino Library Manager:

- WiFiManager (tzapu)
- PubSubClient (Nick O'Leary)
- DHT sensor library (Adafruit) + Adafruit Unified Sensor
- Adafruit GFX + Adafruit SSD1306
- ArduinoJson (v6)

Board: **NodeMCU 1.0 (ESP-12E)** or **Wemos D1 mini**. Open and flash [firmware/Room_temp/Room_temp.ino](firmware/Room_temp/Room_temp.ino).

### Option B — PlatformIO (recommended for 10 boards)

[Install PlatformIO Core](https://platformio.org/install/cli) (or the VS Code extension), then from the [firmware/](firmware/) folder:

```powershell
cd firmware
pio run                    # compile (default env: esp12e)
pio run -t upload          # flash over USB (bare module: pull GPIO0 LOW, tap RST)
pio device monitor         # serial @ 115200

# Dev boards (auto-reset, no buttons needed)
pio run -e nodemcuv2 -t upload
pio run -e d1_mini   -t upload
```

[firmware/platformio.ini](firmware/platformio.ini) reuses the `Room_temp/` folder as `src_dir`, so the same `.ino` builds in both IDEs and you can pin exact library versions across all 10 nodes. An OTA env is provided as a commented-out template.

### First boot (either toolchain)

On first boot the device creates the AP **`RoomTemp-Setup`**. Connect, open `http://192.168.4.1`, pick your primary Wi-Fi (**Wi-Fi #1**), and fill in:

- **MQTT host** — your HiveMQ cluster, e.g. `xxxxx.s1.eu.hivemq.cloud`
- **MQTT port** — `8883`
- **MQTT user / pass** — credentials you create in HiveMQ Cloud → Access Management
- **MQTT base topic** — e.g. `room` (the device automatically publishes to `<base>/<device_id>`)
- **GScript host** — leave as `script.google.com`
- **GScript path** — `/macros/s/<DEPLOYMENT_ID>/exec`
- **Device ID** — **must be unique per board**, e.g. `room1` … `room10`
- **Wi-Fi #2 / #3 (optional)** — extra SSID + password fields for fallback networks

### Multi-network auto-connect

Each board can store **up to 3 Wi-Fi networks**. At boot it uses `ESP8266WiFiMulti`
to scan and connect to whichever configured network is **in range with the strongest
signal** — no manual selection needed. If the active AP goes down for 30 s at runtime,
it re-scans and fails over to another saved network automatically. If *none* of the
saved networks are reachable, it falls back to the `RoomTemp-Setup` captive portal.

Set the networks any of three ways:

- **Captive portal** — Wi-Fi #1 via the built-in scanner, #2/#3 via the extra fields.
- **`/config` page** — edit all three SSID/password slots on a running device (clear an SSID to disable that slot).
- **Compile-time defaults** — `DEFAULT_WIFI{1,2,3}_SSID/PASS` in `secrets.h`, so a whole fleet ships pre-loaded with the known office APs (see [secrets.h.example](firmware/Room_temp/secrets.h.example)).

### Firmware 1.6.0 — reliability fixes

Flash this over any 1.4.x/1.5.x board; EEPROM config migrates in place.

- **MQTT keepalive raised to 90 s.** `postToSheet()` blocks for several seconds per TLS hop and cannot pump `mqtt.loop()` meanwhile; PubSubClient's 15 s default let the broker drop the client mid-POST and fire the LWT, so a device flapped offline around every 30 min.
- **`/status` is LWT-only.** `sensor_fault` and command acks moved to the non-retained `<base>/<device>/ack`. A retained `sensor_fault` used to sit on the status topic forever, leaving the device permanently "offline" in the dashboard even while data flowed; recovery now re-asserts `online`.
- **Sensor watchdog arms at boot.** It used to skip entirely when the DHT never produced a first reading, so a sensor dead at power-on meant a device that was online but silent, with no re-init and no reboot.
- **Captive portal times out after 3 min** and reboots to retry the saved networks, instead of parking in AP mode until someone visits it.
- **TLS footprint**: probes RFC 6066 max-fragment-length and, when the broker supports it, runs the MQTT session on a 1 KB buffer instead of BearSSL's default 16 KB — leaving room for the second TLS session the Sheets POST opens.
- Config strings are always NUL-terminated on load; `/api` reports `null` instead of `0` for a missing reading; the device page's Wi-Fi reset is a real POST with a confirmation instead of a dead GET link.

Settings persist in EEPROM. To re-run the portal: `POST` to `http://<device-ip>/reset` (admin auth required) or hold FLASH at boot (5 s long-press also wipes Wi-Fi at runtime).

Local endpoints once connected:

- `http://<ip>/` — small status page (auto-refreshing)
- `http://<ip>/api` — JSON `{device, temp, hum, rssi, ip, uptime, mqtt, fault, fw}` (`temp`/`hum` are `null`, not `0`, when the DHT has no valid reading)
- `http://<ip>/config` — settings form (admin auth; GET to view, POST to save)
- `http://<ip>/metrics` — JSON diagnostics: boots, reconnects, sensor faults, heap (admin auth)
- `POST http://<ip>/reset` — wipe Wi-Fi creds and reboot (admin auth)

Write endpoints (`/config/save`, `/reset`, `/metrics`) use HTTP Basic auth — user `admin`, password from the `/config` **Admin password** field, falling back to `OTA_PASSWORD`.

## 3. HiveMQ Cloud

1. Create a free **Serverless** cluster at https://www.hivemq.com/mqtt-cloud-broker/.
2. **Access Management** → add **two** users:
   - `esp_pub` — publish to `room/#` (used by all 10 devices, same creds).
   - `dash_sub` — subscribe-only on `room/#` (used by the public dashboard).
3. The cluster URL is shown on the overview page. Use it for both:
   - MQTT/TLS on port **8883** (devices)
   - MQTT over WSS on port **8884** path `/mqtt` (dashboard)

The free Serverless tier is fine for 10 nodes publishing every 10 s (~1 msg/s total).

## 4. Google Sheets logger

1. Create a new Google Sheet, copy its ID from the URL.
2. https://script.google.com → New project, paste [google-apps-script/Code.gs](google-apps-script/Code.gs), set `SHEET_ID`.
3. **Deploy → New deployment → Web app**
   - Execute as: **Me**
   - Who has access: **Anyone**
4. Copy the deployment URL (`https://script.google.com/macros/s/.../exec`). The path part goes in the device's `gs_path` field; the full URL goes in the dashboard's "Google Sheets /exec URL" field.

The script appends `[timestamp, device, temp, hum, rssi]` per POST and exposes:

- `GET …/exec?action=latest&device=room1`
- `GET …/exec?action=history&device=room1&n=200`
- `GET …/exec?action=summary` — latest reading per device (handy for a dashboard refresh)
- `GET …/exec?action=daily_history&device=room1&n=60` — daily avg/min/max temp & humidity per device (feeds the dashboard's **Daily** chart)
- `GET …/exec?action=monthly_history&device=room1&n=24` — monthly aggregates (feeds the dashboard's **Monthly** chart)

### Deploy with `clasp` (CLI, recommended)

Push/deploy the script straight from this repo instead of copy-pasting into the Apps Script editor.

**One-time setup:**

```powershell
npm install -g @google/clasp
clasp login                            # opens a browser, stores creds in %USERPROFILE%\.clasprc.json
```

Then enable the **Apps Script API** for your account once: https://script.google.com/home/usersettings → toggle ON.

**Bind the local folder to a script project:**

```powershell
cd google-apps-script

# Option A — create a brand-new standalone script project
clasp create --type webapp --title "Room Temp Logger"

# Option B — bind to an existing script (open it in the editor → Project Settings → copy Script ID)
#   then edit .clasp.json and replace PASTE_SCRIPT_ID_AFTER_clasp_create with that ID
```

**Push, deploy, tail logs:**

```powershell
clasp push                             # uploads Code.gs + appsscript.json
clasp deploy -d "v1"                   # creates a versioned web-app deployment; prints the /exec URL
clasp deployments                      # list all deployments + URLs
clasp logs --watch                     # live Stackdriver logs
clasp open                             # open the project in the browser
```

The first deployment URL is the one you put into the device's **GScript path** field (or `DEFAULT_GS_PATH` in [firmware/Room_temp/secrets.h](firmware/Room_temp/secrets.h)). Re-running `clasp deploy -d <description>` updates the **same** deployment ID so the URL doesn't change between releases.

> ⚠️ `.clasprc.json` contains your OAuth tokens. It's already in the repo's [.gitignore](.gitignore) — never commit it.

## 5. GitHub Pages dashboard

1. Push this repo to GitHub.
2. Repo **Settings → Pages → Build from branch** → choose `main` and `/dashboard` (or move `dashboard/index.html` to repo root).
3. Open the published URL. On a fresh browser a start screen offers **ตั้งค่า & เชื่อมต่อ MQTT** (opens the settings dialog) or **โหมดทดสอบ (Demo)** — a built-in simulated feed for walking through the UI without a broker. In the settings dialog enter:
   - HiveMQ host, port `8884`, the **subscribe-only** user/pass
   - **Base topic** = `room` (matches the device base)
   - **Stale after** — seconds without a reading before a device is dimmed
   - **Visible device IDs** = `room1,room2,…,room10` (pre-renders the grid even before any device publishes; blank shows everything discovered)
   - (optional) Google Sheets `/exec` URL — backfills the chart and feeds the Daily / Monthly views and the monthly report
   - **Default thresholds** — the °C band that counts as normal, with one-click presets for a vaccine fridge (2–8 °C) and an ordinary room (20–30 °C)

The dashboard (Thai/English, MOPH-green theme, auto light/dark):

- Subscribes to `room/+` (data) and `room/+/status` (LWT online/offline).
- Renders one card per device in a responsive grid showing **temperature + humidity**, its threshold band, RSSI, last-seen age, and a link to the device's own web page.
- Classifies each device as **ปกติ / เฝ้าระวัง / อันตราย / ขาดการติดต่อ** — a critical card pulses red, the sidebar carries an alert badge, a toast fires on every status change, and 🔊 in the top bar turns on an audible alarm.
- Summary tiles count devices per status and show average temp, average humidity, and the live min/max range. Chips filter by status and by **zone**; the search box matches id, alias, or zone; the sort menu orders by name, temperature, status, or recency.
- ⚙️ on a card sets a per-device **alias, zone, and min/max thresholds** (kept in `localStorage`, so each viewer can label their own rooms).
- ⏻ on a card (or the switch in the device dialog) **turns monitoring off for that device** — use it for a sensor that is removed or under maintenance. A disabled device is dimmed and sorted last, drops out of the status counts, the averages, the alerts/sound, and the monthly report, and gets its own filter chip; the sensor itself keeps publishing and logging to Sheets. The total tile shows `+n ปิดใช้งาน` so the count still adds up.
- Click a card for the detail dialog: temp/humidity charts with a **Live / Daily / Monthly** toggle — Live streams MQTT (backfilled from Sheets), Daily and Monthly chart the avg temp/humidity aggregates straight from Google Sheets — plus a data table and IP / RSSI / uptime / last-update.
- **รายงานรายเดือน** builds a printable A4 report for a month and zone (daily-average chart, per-device summary table with out-of-range day counts, signature block) or downloads the same data as CSV; **ส่งออกข้อมูล** exports the buffered live readings.

Credentials live only in the visitor's `localStorage`. For a public dashboard, always use the **subscribe-only** HiveMQ user.

Styling comes from the Tailwind Play CDN plus Google Fonts (Sarabun) and Font Awesome, so the page needs internet access for more than data — for an air-gapped display use the Go dashboard below with vendored assets, or compile Tailwind ahead of time.

## 6. Rolling out 10 devices

For each of the 10 boards, **flash the same firmware** and only change the **Device ID** in the captive portal. Suggested plan:

| # | Device ID | Suggested room | Notes |
|---|---|---|---|
| 1  | `room1`  | Living room       | reference node |
| 2  | `room2`  | Kitchen           | near oven? avoid heat |
| 3  | `room3`  | Bedroom 1         | |
| 4  | `room4`  | Bedroom 2         | |
| 5  | `room5`  | Office            | |
| 6  | `room6`  | Bathroom          | DHT11 max RH = 90% |
| 7  | `room7`  | Garage            | check Wi-Fi signal |
| 8  | `room8`  | Outdoor (covered) | swap DHT11 → DHT22 for –10 °C |
| 9  | `room9`  | Server closet     | |
| 10 | `room10` | Spare / mobile    | |

Tips:

- Use the **same MQTT user/pass** on all 10 boards; only `device_id` differs.
- Stagger first-power-on by a few seconds so the broker doesn't see 10 simultaneous TLS handshakes.
- DHT11 accuracy is ±2 °C / ±5 % RH; for tighter readings swap to DHT22 (same wiring, change `DHT_TYPE` in the sketch).
- The Google Sheet receives **one row per device every 30 min** (plus one immediate row on boot) = 2 rows/device/hour = 10 devices × 48/day ≈ 480 rows/day. Sheets logging cadence is set by `SHEET_INTERVAL` in the firmware.
- Bandwidth: each MQTT message ≈ 120 B, 10 devices × every 10 s ≈ 1 KB/s — well inside the HiveMQ free tier.

## 7. Troubleshooting

- OLED blank → check I2C address (default `0x3C`) and SDA/SCL wiring.
- DHT11 reads `nan` → confirm 3V3 power and DATA on D4; allow ~2 s between reads (already handled).
- MQTT `rc=-2` → wrong host/port or TLS issue. The sketch uses `setInsecure()`; if you need cert pinning, replace with `setFingerprint()` / `setTrustAnchors()`.
- Google Sheets `302` repeats → make sure you redeployed after editing the script and that "Who has access" is **Anyone**.
- Dashboard shows "MQTT: error" → port must be `8884` and path `/mqtt` for HiveMQ Cloud WSS.
