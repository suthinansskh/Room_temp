# Room Climate Dashboard — Go server

A Go rewrite of the static [dashboard/index.html](../dashboard/index.html). Instead of the
browser connecting to HiveMQ directly (with subscribe-only creds living in
`localStorage`), this server:

- subscribes to MQTT **server-side** over TLS, so broker credentials stay on the
  server and never reach the client;
- keeps device state + a per-device history ring buffer in memory;
- computes status (online/stale/offline) and the fleet summary;
- streams a fresh snapshot to every browser over **Server-Sent Events** (`/events`).

The frontend is the same UI as the static version — same Tailwind/Sarabun shell,
status tiles, zone filters, per-device thresholds, chart dialog, and printable
monthly report — with the MQTT-in-browser logic replaced by an
`EventSource('/events')` feed. There is no broker settings dialog here: the
server owns the credentials, so the settings dialog only shows server state and
the display thresholds.

```
browser ──HTTP/SSE──► Go server ──MQTT/TLS──► HiveMQ Cloud
```

## Run

```powershell
cd dashboard-go

# minimum: point it at your HiveMQ cluster with subscribe-only creds
$env:MQTT_HOST = "your-cluster.s1.eu.hivemq.cloud"
$env:MQTT_USER = "dash_sub"
$env:MQTT_PASS = "your-subscribe-only-password"
go run .
```

Then open <http://localhost:8080>.

Build a single self-contained binary (the HTML is embedded via `go:embed`):

```powershell
go build -o dashboard.exe .
./dashboard.exe
```

## Configuration (environment variables)

| Var | Default | Purpose |
|---|---|---|
| `MQTT_HOST` | *(required)* | HiveMQ cluster host |
| `MQTT_PORT` | `8883` | MQTT-over-TLS port |
| `MQTT_USER` / `MQTT_PASS` | — | subscribe-only MQTT credentials |
| `MQTT_BASE` | `room` | topic prefix; subscribes to `<base>/+` and `<base>/+/status` |
| `MQTT_INSECURE` | `false` | skip TLS cert verification (matches the firmware's `setInsecure()`) |
| `STALE_SECONDS` | `45` | a device with no reading for this long is marked **stale** |
| `DEVICES` | — | comma-separated IDs to pre-render before any data arrives, e.g. `room1,room2` |
| `HISTORY_MAX` | `300` | in-memory history points kept per device (for the chart) |
| `GS_URL` | — | optional Apps Script `/exec` URL; when set, `/api/history` proxies Google Sheets history (server-side, no browser CORS) |
| `LISTEN_ADDR` | `:8080` | HTTP bind address |

## Endpoints

| Path | What |
|---|---|
| `GET /` | dashboard HTML (embedded) |
| `GET /events` | SSE stream of snapshots (`{mqtt, base, now, summary, devices[]}`) |
| `GET /api/devices` | one snapshot as JSON |
| `GET /api/history?device=<id>` | chart history (`[{t,T,H}]`); Sheets-backed if `GS_URL` set, else in-memory |
| `GET /api/history?device=<id>&range=daily\|monthly[&n=]` | Apps Script daily/monthly aggregate rows (`[{period,avg_temp,min_temp,max_temp,avg_hum,…}]`) passed through verbatim; empty array when `GS_URL` is unset — powers the Daily/Monthly chart toggle and the monthly report |

## Notes

- The server owns online/stale/offline classification using `STALE_SECONDS`; the
  browser adds the temperature verdict (ปกติ / เฝ้าระวัง / อันตราย) from thresholds it
  holds locally. Search, sort, status/zone filters, device aliases, zones, and
  min/max limits are per-browser display state kept in `localStorage`.
- The page loads Tailwind, Sarabun, and Font Awesome from public CDNs; a display
  with no internet access needs those three assets vendored into `web/`.
- HiveMQ Cloud presents a valid (Let's Encrypt) certificate, so leave
  `MQTT_INSECURE=false` in production. Only enable it for brokers with self-signed certs.
- The original static [dashboard/index.html](../dashboard/index.html) is left in place; this
  is an alternative deployment, not a replacement for GitHub Pages hosting.
