// Embedded local web UI. Kept in a header so the Arduino prototype
// generator does not parse JS keywords inside the raw string literal.
#pragma once

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Room Temp</title>
<style>body{font-family:system-ui;margin:2rem;background:#0b1020;color:#e6e9ef}
.card{background:#141a33;padding:1.5rem;border-radius:12px;max-width:420px}
h1{margin:0 0 .5rem}.v{font-size:2.2rem;font-weight:700}.row{display:flex;gap:2rem;margin:1rem 0}
small{color:#90a0c0}a{color:#7cc}
#warn{display:none;margin:.6rem 0;padding:.5rem .7rem;border-radius:8px;background:#3b1d24;color:#ffb4b4;font-size:.85rem}
button.link{background:none;border:0;color:#7cc;font:inherit;padding:0;cursor:pointer;text-decoration:underline}</style>
</head>
<body><div class="card"><h1>Room Temp</h1>
<div class="row"><div><small>Temperature</small><div class="v" id="t">--.-</div></div>
<div><small>Humidity</small><div class="v" id="h">--</div></div></div>
<div id="warn"></div>
<small id="s"></small><br><br>
<a href="/api">/api</a> &middot; <a href="/config">/config</a> &middot;
<button class="link" id="rst" type="button">reset Wi-Fi</button></div>
<script>
function poll(){fetch('/api').then(function(r){return r.json();}).then(function(j){
document.getElementById('t').textContent=(j.temp==null?'--.-':j.temp.toFixed(1))+' °C';
document.getElementById('h').textContent=(j.hum==null?'--':Math.round(j.hum))+' %';
document.getElementById('s').textContent='Device '+j.device+' · RSSI '+j.rssi+' dBm · up '+j.uptime+'s · MQTT '+(j.mqtt?'ok':'down');
var w=document.getElementById('warn');
if(j.fault||j.temp==null){w.style.display='block';w.textContent=j.fault?'Sensor fault - no valid DHT reading':'Waiting for a valid DHT reading...';}
else{w.style.display='none';}
}).catch(function(){});}
setInterval(poll,2000);poll();
document.getElementById('rst').onclick=function(){
if(!confirm('Erase Wi-Fi credentials and reboot into the setup portal?'))return;
fetch('/reset',{method:'POST'}).then(function(r){return r.text();}).then(function(t){alert(t);}).catch(function(){alert('Request failed');});};
</script></body></html>
)HTML";
