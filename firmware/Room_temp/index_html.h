// Embedded local web UI. Kept in a header so the Arduino prototype
// generator does not parse JS keywords inside the raw string literal.
#pragma once

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Room Temp</title>
<style>body{font-family:system-ui;margin:2rem;background:#0b1020;color:#e6e9ef}
.card{background:#141a33;padding:1.5rem;border-radius:12px;max-width:420px}
h1{margin:0 0 .5rem}.v{font-size:2.2rem;font-weight:700}.row{display:flex;gap:2rem;margin:1rem 0}
small{color:#90a0c0}a{color:#7cc}</style></head>
<body><div class="card"><h1>Room Temp</h1>
<div class="row"><div><small>Temperature</small><div class="v" id="t">--.-</div></div>
<div><small>Humidity</small><div class="v" id="h">--</div></div></div>
<small id="s"></small><br><br>
<a href="/api">/api</a> &middot; <a href="/config">/config</a> &middot; <a href="/reset">/reset (Wi-Fi)</a></div>
<script>
function poll(){fetch('/api').then(function(r){return r.json();}).then(function(j){
document.getElementById('t').textContent=j.temp.toFixed(1)+' \u00B0C';
document.getElementById('h').textContent=Math.round(j.hum)+' %';
document.getElementById('s').textContent='Device '+j.device+' \u00B7 RSSI '+j.rssi+' dBm \u00B7 up '+j.uptime+'s';
}).catch(function(){});}
setInterval(poll,2000);poll();
</script></body></html>
)HTML";
