package main

import (
	"crypto/rand"
	"crypto/tls"
	"embed"
	"encoding/hex"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"net/url"
	"time"
)

//go:embed web/index.html
var webFS embed.FS

func main() {
	cfg := loadConfig()
	if cfg.MQTTHost == "" {
		log.Fatal("MQTT_HOST is required (set the HiveMQ cluster host)")
	}

	hub := newHub(cfg)
	startMQTT(cfg, hub)

	// Periodic staleness sweep, mirroring the old browser-side 2s interval.
	go func() {
		t := time.NewTicker(2 * time.Second)
		defer t.Stop()
		for range t.C {
			hub.refreshStaleness()
		}
	}()

	mux := http.NewServeMux()
	mux.HandleFunc("/", handleIndex)
	mux.HandleFunc("/events", hub.handleSSE)
	mux.HandleFunc("/api/devices", hub.handleDevices)
	mux.HandleFunc("/api/history", hub.handleHistory(cfg))

	log.Printf("dashboard on %s  (base=%q stale=%ds host=%s)", cfg.ListenAddr, cfg.MQTTBase, cfg.StaleSeconds, cfg.MQTTHost)
	log.Fatal(http.ListenAndServe(cfg.ListenAddr, mux))
}

func handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	b, err := webFS.ReadFile("web/index.html")
	if err != nil {
		http.Error(w, "index missing", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write(b)
}

func (h *Hub) handleDevices(w http.ResponseWriter, r *http.Request) {
	data, err := h.snapshotJSON()
	if err != nil {
		http.Error(w, "snapshot error", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write(data)
}

// handleSSE streams a fresh snapshot on connect and on every state change.
func (h *Hub) handleSSE(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")

	ch := h.addClient()
	defer h.removeClient(ch)

	// Prime the new client with the current state immediately.
	if data, err := h.snapshotJSON(); err == nil {
		writeSSE(w, data)
		flusher.Flush()
	}

	keepalive := time.NewTicker(20 * time.Second)
	defer keepalive.Stop()
	ctx := r.Context()
	for {
		select {
		case <-ctx.Done():
			return
		case data, ok := <-ch:
			if !ok {
				return
			}
			writeSSE(w, data)
			flusher.Flush()
		case <-keepalive.C:
			io.WriteString(w, ": ping\n\n")
			flusher.Flush()
		}
	}
}

// handleHistory serves chart history for one device. If a Google Sheets URL
// is configured it proxies that (server-side, so no browser CORS issue);
// otherwise it returns the in-memory ring buffer.
func (h *Hub) handleHistory(cfg Config) http.HandlerFunc {
	httpc := &http.Client{
		Timeout:   8 * time.Second,
		Transport: &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: cfg.MQTTInsecure}},
	}
	return func(w http.ResponseWriter, r *http.Request) {
		id := cleanDeviceID(r.URL.Query().Get("device"))
		if id == "" {
			http.Error(w, "device required", http.StatusBadRequest)
			return
		}
		if cfg.GoogleSheetsURL != "" {
			if pts, err := fetchSheetHistory(httpc, cfg.GoogleSheetsURL, id); err == nil && len(pts) > 0 {
				writeJSON(w, pts)
				return
			}
		}
		writeJSON(w, h.historyFor(id))
	}
}

// sheetRow matches the columns the Apps Script logger returns for ?action=history.
type sheetRow struct {
	Ts   string   `json:"ts"`
	Temp *float64 `json:"temp"`
	Hum  *float64 `json:"hum"`
}

func fetchSheetHistory(c *http.Client, base, device string) ([]point, error) {
	u, err := url.Parse(base)
	if err != nil {
		return nil, err
	}
	q := u.Query()
	q.Set("action", "history")
	q.Set("device", device)
	q.Set("n", "200")
	u.RawQuery = q.Encode()

	resp, err := c.Get(u.String())
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	var rows []sheetRow
	if err := json.NewDecoder(resp.Body).Decode(&rows); err != nil {
		return nil, err
	}
	out := make([]point, 0, len(rows))
	for _, row := range rows {
		ts, err := time.Parse(time.RFC3339, row.Ts)
		if err != nil {
			// Apps Script may serialize Date without a timezone; try a looser format.
			if ts, err = time.Parse("2006-01-02T15:04:05.000Z", row.Ts); err != nil {
				continue
			}
		}
		out = append(out, point{T: float64(ts.UnixMilli()), V: row.Temp, H: row.Hum})
	}
	return out, nil
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

func writeSSE(w io.Writer, data []byte) {
	io.WriteString(w, "data: ")
	w.Write(data)
	io.WriteString(w, "\n\n")
}

func randHex(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	return hex.EncodeToString(b)
}
