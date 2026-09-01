package main

import (
	"encoding/json"
	"math"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

// point is one historical reading kept in the in-memory ring buffer.
type point struct {
	T float64  `json:"t"` // unix millis
	V *float64 `json:"T"` // temperature (capital T to match the old JS chart payload)
	H *float64 `json:"H"` // humidity
}

// Device holds the latest reading and status for a single sensor node.
// Pointers are used for fields that may be unknown so they serialize as
// JSON null and the frontend can render "--".
type Device struct {
	ID       string   `json:"id"`
	Temp     *float64 `json:"temp"`
	Hum      *float64 `json:"hum"`
	RSSI     *int     `json:"rssi"`
	Uptime   *int64   `json:"uptime"`
	IP       string   `json:"ip"`
	FW       string   `json:"fw"`
	LastTs   int64    `json:"lastTs"` // unix millis of last data message, 0 if none
	Status   string   `json:"status"` // online | stale | offline | unknown
	Fault    bool     `json:"fault"`  // sensor stopped answering; reading is frozen
	Expected bool     `json:"expected"`

	// Millis at which an LWT declared this device offline. A retained reading
	// older than this must not clear that status -- see applyReading.
	offlineAt int64

	history []point
}

// Summary is the fleet roll-up shown in the metric tiles.
type Summary struct {
	Total   int      `json:"total"`
	Online  int      `json:"online"`
	AvgTemp *float64 `json:"avgTemp"`
	AvgHum  *float64 `json:"avgHum"`
	MinTemp *float64 `json:"minTemp"`
	MaxTemp *float64 `json:"maxTemp"`
}

// Snapshot is the full payload pushed to every browser over SSE.
type Snapshot struct {
	MQTT    string    `json:"mqtt"`
	Base    string    `json:"base"`
	Now     int64     `json:"now"`
	Summary Summary   `json:"summary"`
	Devices []*Device `json:"devices"`
}

// Hub owns all device state and the set of connected SSE clients. Every
// mutation recomputes statuses and broadcasts a fresh snapshot.
type Hub struct {
	mu         sync.RWMutex
	devices    map[string]*Device
	clients    map[chan []byte]struct{}
	base       string
	staleSec   int
	historyMax int
	mqttStatus string
}

func newHub(cfg Config) *Hub {
	h := &Hub{
		devices:    make(map[string]*Device),
		clients:    make(map[chan []byte]struct{}),
		base:       cfg.MQTTBase,
		staleSec:   cfg.StaleSeconds,
		historyMax: cfg.HistoryMax,
		mqttStatus: "connecting",
	}
	for _, id := range cfg.Expected {
		h.devices[id] = &Device{ID: id, Status: "unknown", Expected: true}
	}
	return h
}

var deviceIDRe = regexp.MustCompile(`[^A-Za-z0-9_.-]`)

func cleanDeviceID(id string) string {
	return deviceIDRe.ReplaceAllString(strings.TrimSpace(id), "")
}

func (h *Hub) device(id string) *Device {
	d := h.devices[id]
	if d == nil {
		d = &Device{ID: id, Status: "unknown"}
		h.devices[id] = d
	}
	return d
}

// applyReading records a JSON data message published to <base>/<device>.
func (h *Hub) applyReading(id string, r reading) {
	h.mu.Lock()
	d := h.device(id)
	now := time.Now().UnixMilli()
	// A retained payload can be days old, so prefer the reading time the
	// firmware stamped (>= 1.7.0) over the moment it reached us. Values
	// outside a sane window mean an unsynced device clock: ignore those.
	if r.Ts != nil {
		if ms := *r.Ts * 1000; ms > 1700000000000 && ms < now+3600000 {
			now = ms
		}
	}
	d.Temp = r.Temp
	d.Hum = r.Hum
	d.RSSI = r.RSSI
	d.Uptime = r.Uptime
	if r.IP != "" {
		d.IP = r.IP
	}
	if r.FW != "" {
		d.FW = r.FW
	}
	d.LastTs = now
	d.Fault = r.Fault
	// On every (re)connect the broker replays two retained messages per device
	// -- the last reading and the last status -- with no ordering guarantee
	// between the two subscriptions. Since firmware 1.8.0 a node publishes a
	// retained "offline" before every deliberate reboot, so that pair is the
	// normal state of a rebooting device; letting the reading win would show a
	// node that never came back as online and healthy. Only a reading taken
	// after the device was declared offline may clear it. Firmware with no
	// "ts" is stamped with arrival time, which is always newer, so it behaves
	// exactly as before.
	if d.Status != "offline" || now > d.offlineAt {
		d.offlineAt = 0
		// A stamped reading can already be older than the stale cutoff (a
		// retained payload from a device that has since gone quiet), and a
		// faulted sensor republishes a frozen value. Classify both now rather
		// than flashing "online" until the next staleness sweep.
		if d.Fault || time.Now().UnixMilli()-now > int64(h.staleSec)*1000 {
			d.Status = "stale"
		} else {
			d.Status = "online"
		}
	}
	d.history = append(d.history, point{T: float64(now), V: r.Temp, H: r.Hum})
	if len(d.history) > h.historyMax {
		d.history = d.history[len(d.history)-h.historyMax:]
	}
	h.mu.Unlock()
	h.broadcast()
}

// applyStatus records an LWT status message on <base>/<device>/status.
func (h *Hub) applyStatus(id, state string) {
	h.mu.Lock()
	d := h.device(id)
	if state == "online" {
		d.Status = "online"
		d.offlineAt = 0
	} else if state == "offline" {
		d.Status = "offline"
		d.offlineAt = time.Now().UnixMilli()
	}
	// Other status payloads (e.g. "sensor_fault") are left as-is so the
	// staleness pass can still classify the device by reading age.
	h.mu.Unlock()
	h.broadcast()
}

func (h *Hub) setMQTTStatus(s string) {
	h.mu.Lock()
	h.mqttStatus = s
	h.mu.Unlock()
	h.broadcast()
}

func (h *Hub) historyFor(id string) []point {
	h.mu.RLock()
	defer h.mu.RUnlock()
	d := h.devices[id]
	if d == nil {
		return nil
	}
	out := make([]point, len(d.history))
	copy(out, d.history)
	return out
}

// refreshStaleness reclassifies online/stale by reading age. Devices marked
// offline by an LWT stay offline until they publish again.
func (h *Hub) refreshStaleness() {
	h.mu.Lock()
	now := time.Now().UnixMilli()
	cutoff := int64(h.staleSec) * 1000
	for _, d := range h.devices {
		if d.Status == "offline" {
			continue
		}
		if d.LastTs == 0 {
			d.Status = "unknown"
		} else if d.Fault || now-d.LastTs > cutoff {
			d.Status = "stale"
		} else {
			d.Status = "online"
		}
	}
	h.mu.Unlock()
	h.broadcast()
}

func (h *Hub) snapshot() Snapshot {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.snapshotLocked()
}

// snapshotJSON marshals the snapshot while holding the read lock, so device
// fields are never read by encoding/json concurrently with a writer holding
// the write lock. Callers must serialize using the returned bytes, not the
// live *Device pointers.
func (h *Hub) snapshotJSON() ([]byte, error) {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return json.Marshal(h.snapshotLocked())
}

// snapshotLocked builds the snapshot; the caller must already hold h.mu
// (read or write).
func (h *Hub) snapshotLocked() Snapshot {
	devs := make([]*Device, 0, len(h.devices))
	for _, d := range h.devices {
		devs = append(devs, d)
	}
	sort.Slice(devs, func(i, j int) bool { return devs[i].ID < devs[j].ID })

	var sum Summary
	sum.Total = len(devs)
	var temps, hums []float64
	for _, d := range devs {
		if d.Status != "online" {
			continue
		}
		sum.Online++
		if d.Temp != nil {
			temps = append(temps, *d.Temp)
		}
		if d.Hum != nil {
			hums = append(hums, *d.Hum)
		}
	}
	sum.AvgTemp = avg(temps)
	sum.AvgHum = avg(hums)
	sum.MinTemp = minOf(temps)
	sum.MaxTemp = maxOf(temps)

	return Snapshot{
		MQTT:    h.mqttStatus,
		Base:    h.base,
		Now:     time.Now().UnixMilli(),
		Summary: sum,
		Devices: devs,
	}
}

// --- SSE client registry --------------------------------------------------

func (h *Hub) addClient() chan []byte {
	ch := make(chan []byte, 8)
	h.mu.Lock()
	h.clients[ch] = struct{}{}
	h.mu.Unlock()
	return ch
}

func (h *Hub) removeClient(ch chan []byte) {
	h.mu.Lock()
	if _, ok := h.clients[ch]; ok {
		delete(h.clients, ch)
		close(ch)
	}
	h.mu.Unlock()
}

func (h *Hub) broadcast() {
	data, err := h.snapshotJSON()
	if err != nil {
		return
	}
	h.mu.RLock()
	for ch := range h.clients {
		select {
		case ch <- data:
		default: // slow client: drop this frame, next snapshot will catch it up
		}
	}
	h.mu.RUnlock()
}

func avg(v []float64) *float64 {
	if len(v) == 0 {
		return nil
	}
	var s float64
	for _, x := range v {
		s += x
	}
	r := math.Round(s/float64(len(v))*10) / 10
	return &r
}

func minOf(v []float64) *float64 {
	if len(v) == 0 {
		return nil
	}
	m := v[0]
	for _, x := range v[1:] {
		if x < m {
			m = x
		}
	}
	return &m
}

func maxOf(v []float64) *float64 {
	if len(v) == 0 {
		return nil
	}
	m := v[0]
	for _, x := range v[1:] {
		if x > m {
			m = x
		}
	}
	return &m
}
