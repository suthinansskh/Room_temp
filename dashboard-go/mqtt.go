package main

import (
	"crypto/tls"
	"encoding/json"
	"fmt"
	"log"
	"strings"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

// reading mirrors the JSON payload the firmware publishes to <base>/<device>:
//   {"device":"room1","temp":24.7,"hum":58,"rssi":-61,"ip":"...","uptime":1234,"fw":"1.4.0"}
type reading struct {
	Temp   *float64 `json:"temp"`
	Hum    *float64 `json:"hum"`
	RSSI   *int     `json:"rssi"`
	Uptime *int64   `json:"uptime"`
	IP     string   `json:"ip"`
	FW     string   `json:"fw"`
}

func startMQTT(cfg Config, h *Hub) mqtt.Client {
	broker := fmt.Sprintf("ssl://%s:%d", cfg.MQTTHost, cfg.MQTTPort)

	opts := mqtt.NewClientOptions()
	opts.AddBroker(broker)
	opts.SetClientID("rt-dash-go-" + randHex(6))
	opts.SetUsername(cfg.MQTTUser)
	opts.SetPassword(cfg.MQTTPass)
	opts.SetCleanSession(true)
	opts.SetAutoReconnect(true)
	opts.SetConnectRetry(true)
	opts.SetTLSConfig(&tls.Config{
		InsecureSkipVerify: cfg.MQTTInsecure,
		ServerName:         cfg.MQTTHost,
	})

	dataTopic := cfg.MQTTBase + "/+"
	statusTopic := cfg.MQTTBase + "/+/status"

	opts.SetOnConnectHandler(func(c mqtt.Client) {
		log.Printf("mqtt connected to %s", broker)
		h.setMQTTStatus("connected")
		// MQTT '+' matches a single level, so <base>/+ captures only the
		// data topics and <base>/+/status only the LWT topics — no overlap.
		// The handler still re-checks topic depth to route each message.
		c.Subscribe(dataTopic, 0, func(_ mqtt.Client, m mqtt.Message) { handleMessage(h, cfg.MQTTBase, m) })
		c.Subscribe(statusTopic, 0, func(_ mqtt.Client, m mqtt.Message) { handleMessage(h, cfg.MQTTBase, m) })
	})
	opts.SetConnectionLostHandler(func(_ mqtt.Client, err error) {
		log.Printf("mqtt connection lost: %v", err)
		h.setMQTTStatus("disconnected")
	})
	opts.SetReconnectingHandler(func(_ mqtt.Client, _ *mqtt.ClientOptions) {
		h.setMQTTStatus("reconnecting")
	})

	client := mqtt.NewClient(opts)
	go func() {
		if t := client.Connect(); t.Wait() && t.Error() != nil {
			log.Printf("mqtt initial connect failed: %v", t.Error())
			h.setMQTTStatus("error")
		}
	}()
	return client
}

func handleMessage(h *Hub, base string, m mqtt.Message) {
	parts := strings.Split(m.Topic(), "/")
	if len(parts) < 2 || parts[0] != base {
		return
	}
	id := cleanDeviceID(parts[1])
	if id == "" {
		return
	}

	// <base>/<device>/status  -> LWT online/offline
	if len(parts) >= 3 && parts[2] == "status" {
		h.applyStatus(id, strings.TrimSpace(string(m.Payload())))
		return
	}
	// <base>/<device>  -> JSON reading. Ignore any deeper topics (e.g. /cmd).
	if len(parts) != 2 {
		return
	}
	var r reading
	if err := json.Unmarshal(m.Payload(), &r); err != nil {
		log.Printf("bad payload on %s: %v", m.Topic(), err)
		return
	}
	h.applyReading(id, r)
}
