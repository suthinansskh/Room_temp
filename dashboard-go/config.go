package main

import (
	"os"
	"strconv"
	"strings"
)

// Config is read once at startup from environment variables. The MQTT
// credentials that used to live in each visitor's browser localStorage now
// stay here on the server, so a public dashboard only needs a subscribe-only
// HiveMQ user configured in one place.
type Config struct {
	ListenAddr string // HTTP bind address, e.g. ":8080"

	MQTTHost     string // HiveMQ cluster host, e.g. xxxx.s1.eu.hivemq.cloud
	MQTTPort     int    // TLS MQTT port (8883 for HiveMQ Cloud)
	MQTTUser     string // subscribe-only user
	MQTTPass     string
	MQTTBase     string // topic prefix; devices publish to <base>/<device>
	MQTTInsecure bool   // skip TLS cert verification (matches firmware setInsecure)

	StaleSeconds int      // a device with no reading for this long is "stale"
	Expected     []string // device IDs to pre-render before any data arrives
	HistoryMax   int      // in-memory history points kept per device

	GoogleSheetsURL string // optional Apps Script /exec URL for chart history
}

func loadConfig() Config {
	c := Config{
		ListenAddr:      env("LISTEN_ADDR", ":8080"),
		MQTTHost:        env("MQTT_HOST", ""),
		MQTTPort:        envInt("MQTT_PORT", 8883),
		MQTTUser:        env("MQTT_USER", ""),
		MQTTPass:        env("MQTT_PASS", ""),
		MQTTBase:        env("MQTT_BASE", "room"),
		MQTTInsecure:    envBool("MQTT_INSECURE", false),
		StaleSeconds:    envInt("STALE_SECONDS", 45),
		HistoryMax:      envInt("HISTORY_MAX", 300),
		GoogleSheetsURL: env("GS_URL", ""),
	}
	for _, id := range strings.Split(env("DEVICES", ""), ",") {
		if id = cleanDeviceID(id); id != "" {
			c.Expected = append(c.Expected, id)
		}
	}
	if c.StaleSeconds < 5 {
		c.StaleSeconds = 5
	}
	return c
}

func env(key, def string) string {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		return v
	}
	return def
}

func envInt(key string, def int) int {
	if v, ok := os.LookupEnv(key); ok {
		if n, err := strconv.Atoi(strings.TrimSpace(v)); err == nil {
			return n
		}
	}
	return def
}

func envBool(key string, def bool) bool {
	if v, ok := os.LookupEnv(key); ok {
		if b, err := strconv.ParseBool(strings.TrimSpace(v)); err == nil {
			return b
		}
	}
	return def
}
