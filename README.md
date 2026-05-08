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
              │ HTTPS (every 60 s, per device)
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

Status messages are `online` / `offline` (LWT-based) and are also retained.

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

On first boot the device creates the AP **`RoomTemp-Setup`**. Connect, open `http://192.168.4.1`, pick your Wi-Fi, and fill in:

- **MQTT host** — your HiveMQ cluster, e.g. `xxxxx.s1.eu.hivemq.cloud`
- **MQTT port** — `8883`
- **MQTT user / pass** — credentials you create in HiveMQ Cloud → Access Management
- **MQTT base topic** — e.g. `room` (the device automatically publishes to `<base>/<device_id>`)
- **GScript host** — leave as `script.google.com`
- **GScript path** — `/macros/s/<DEPLOYMENT_ID>/exec`
- **Device ID** — **must be unique per board**, e.g. `room1` … `room10`

Settings persist in EEPROM. To re-run the portal: hit `http://<device-ip>/reset` or hold FLASH at boot.

Local endpoints once connected:

- `http://<ip>/` — small status page (auto-refreshing)
- `http://<ip>/api` — JSON `{device, temp, hum, rssi, uptime, mqtt}`
- `http://<ip>/reset` — wipe Wi-Fi creds and reboot

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
3. Open the published URL, expand **⚙️ Configuration**, and enter:
   - HiveMQ host, port `8884`, the **subscribe-only** user/pass
   - **Base topic** = `room` (matches the device base)
   - **Expected device IDs** = `room1,room2,…,room10` (pre-renders the grid even before any device publishes)
   - (optional) Google Sheets `/exec` URL — pre-loads the chart with history when you click a device

The dashboard:

- Subscribes to `room/+` (data) and `room/+/status` (LWT online/offline).
- Renders one card per device in a responsive grid showing **temperature + humidity**.
- Marks a device **stale** (dimmed) after the configured timeout, **offline** when its LWT fires.
- Click a card to open a temp/humidity chart for that device (live, plus optional history from Sheets).
- Shows a fleet summary: online count, average temp, average humidity, min/max temp.

Credentials live only in the visitor's `localStorage`. For a public dashboard, always use the **subscribe-only** HiveMQ user.

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
- The Google Sheet receives **one row per device per minute** = 10 rows/min = 14 400 rows/day. After ~1 month rotate to a new sheet or summarise daily.
- Bandwidth: each MQTT message ≈ 120 B, 10 devices × every 10 s ≈ 1 KB/s — well inside the HiveMQ free tier.

## 7. Troubleshooting

- OLED blank → check I2C address (default `0x3C`) and SDA/SCL wiring.
- DHT11 reads `nan` → confirm 3V3 power and DATA on D4; allow ~2 s between reads (already handled).
- MQTT `rc=-2` → wrong host/port or TLS issue. The sketch uses `setInsecure()`; if you need cert pinning, replace with `setFingerprint()` / `setTrustAnchors()`.
- Google Sheets `302` repeats → make sure you redeployed after editing the script and that "Who has access" is **Anyone**.
- Dashboard shows "MQTT: error" → port must be `8884` and path `/mqtt` for HiveMQ Cloud WSS.
