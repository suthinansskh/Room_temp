# Room Climate Dashboard — Go server

A Go rewrite of the static [dashboard/index.html](../dashboard/index.html). Instead of the
browser connecting to HiveMQ directly (with subscribe-only creds living in
`localStorage`), this server:

- subscribes to MQTT **server-side** over TLS, so broker credentials stay on the
  server and never reach the client;
- keeps device state + a per-device history ring buffer in memory;
- computes status (online/stale/offline) and the fleet summary;
- streams a fresh snapshot to every browser over **Server-Sent Events** (`/events`).

The frontend is the same UI as the static version — same CSS/layout and Chart.js —
with the MQTT-in-browser logic replaced by an `EventSource('/events')` feed.

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

## Notes

- The server owns status classification using `STALE_SECONDS`; the browser only
  renders. Search / sort / status-filter remain client-side display controls.
- HiveMQ Cloud presents a valid (Let's Encrypt) certificate, so leave
  `MQTT_INSECURE=false` in production. Only enable it for brokers with self-signed certs.
- The original static [dashboard/index.html](../dashboard/index.html) is left in place; this
  is an alternative deployment, not a replacement for GitHub Pages hosting.
