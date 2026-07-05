package main

import "testing"

func TestReadingFlow(t *testing.T) {
	cfg := Config{MQTTBase: "room", StaleSeconds: 45, HistoryMax: 10, Expected: []string{"room1"}}
	h := newHub(cfg)
	temp, hum := 24.7, 58.0
	rssi := -61
	h.applyReading("room1", reading{Temp: &temp, Hum: &hum, RSSI: &rssi, IP: "192.168.1.5", FW: "1.4.0"})
	snap := h.snapshot()
	if snap.Summary.Online != 1 {
		t.Fatalf("online=%d want 1", snap.Summary.Online)
	}
	if snap.Summary.AvgTemp == nil || *snap.Summary.AvgTemp != 24.7 {
		t.Fatalf("avgTemp=%v want 24.7", snap.Summary.AvgTemp)
	}
	if got := h.historyFor("room1"); len(got) != 1 {
		t.Fatalf("history len=%d want 1", len(got))
	}
	h.applyStatus("room1", "offline")
	if h.snapshot().Devices[0].Status != "offline" {
		t.Fatalf("status not offline after LWT")
	}
}
