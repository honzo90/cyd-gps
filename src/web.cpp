#include "web.h"
#include <Arduino.h>
#include <SD.h>
#include <Update.h>
#include <WebServer.h>
#include <esp_ota_ops.h>
#include <string.h>
#include "gh_ota.h"
#include "net.h"

static WebServer s_server(80);
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static web_snapshot_t s_snap;
static bool s_update_ok;
static char s_update_err[64];

static const char k_page[] PROGMEM = R"HTML(<!doctype html>
<html lang="pl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CYD GPS</title>
<style>
body{font-family:system-ui,sans-serif;background:#101418;color:#e8ecef;margin:0;padding:16px;max-width:720px}
h1{font-size:20px;margin:0 0 12px}h2{font-size:16px;color:#9aa4ae;margin:20px 0 8px}
.card{background:#1a2026;border-radius:10px;padding:12px 14px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}
.k{color:#7f8a94;font-size:12px}.v{font-size:20px;font-variant-numeric:tabular-nums}
.big{font-size:56px;font-weight:700;line-height:1}
table{width:100%;border-collapse:collapse}td{padding:6px 4px;border-bottom:1px solid #2a3138}
a{color:#6cb6ff}button,input{font:inherit;border-radius:6px;border:1px solid #3a434c;background:#2a3138;color:#e8ecef;padding:6px 10px}
button.del{background:#5a2424;border-color:#7a3030}input{width:100%;box-sizing:border-box;margin:4px 0}
.ok{color:#4cd97b}.warn{color:#ffc145}.err{color:#ff5a5a}small{color:#7f8a94}
</style></head><body>
<h1>CYD GPS <small id="ver"></small></h1>
<div class="card">
 <div class="grid">
  <div><div class="k">Prędkość km/h</div><div class="big" id="spd">--</div></div>
  <div><div class="k">Status</div><div class="v" id="fix">--</div><div class="k" id="sig"></div></div>
  <div><div class="k">Trip</div><div class="v" id="trip">--</div></div>
  <div><div class="k">Całkowity</div><div class="v" id="total">--</div></div>
  <div><div class="k">Maks.</div><div class="v" id="max">--</div></div>
  <div><div class="k">Czas UTC</div><div class="v" id="utc">--</div></div>
 </div>
 <p style="margin:10px 0 0"><span class="k">Pozycja</span> <span id="pos">--</span> <a id="map" target="_blank" style="display:none">mapa</a></p>
 <p style="margin:4px 0 0" class="k" id="sd"></p>
</div>

<h2>Trasy GPX</h2>
<div class="card"><table id="files"><tr><td>…</td></tr></table></div>

<h2>Wi‑Fi domowe</h2>
<div class="card">
 <div id="wifi" class="k"></div>
 <form id="wf"><input name="ssid" placeholder="Nazwa sieci (SSID)" maxlength="32" required>
 <input name="pass" type="password" placeholder="Hasło" maxlength="64">
 <button>Zapisz i połącz</button> <button type="button" id="forget">Zapomnij sieć</button></form>
 <small>Sieć tylko 2,4 GHz. Gdy domowa sieć działa, hotspot CYD wyłącza się sam.</small>
</div>

<h2>Aktualizacja firmware</h2>
<div class="card">
 <div id="gh" class="v" style="font-size:16px"></div>
 <button id="ghcheck">Sprawdź teraz</button> <button id="ghinst" style="display:none">Zainstaluj z GitHub</button>
 <small style="display:block;margin:6px 0 12px">Działa w sieci domowej (z internetem). Instalacja trwa ok. minuty, potem restart.</small>
 <div class="k">albo plik z komputera:</div>
 <input type="file" id="fw" accept=".bin"> <button id="up">Wgraj</button>
 <div id="upst" class="k"></div>
 <small>Plik .pio/build/cyd2usb/firmware.bin. Jeśli nowa wersja nie uruchomi Wi‑Fi w 15 s, CYD wróci do poprzedniej.</small>
</div>

<script>
const $=id=>document.getElementById(id);
const fmtKm=v=>v.toFixed(2)+' km';
async function live(){
 try{
  const r=await fetch('/api/status');const d=await r.json();
  $('ver').textContent='v'+d.version;
  $('spd').textContent=d.fix?Math.round(d.speed_kmh<2?0:d.speed_kmh):'--';
  $('fix').innerHTML=d.fix?'<span class="ok">FIX</span> '+d.sats+' sat':'<span class="warn">szukam fix</span>';
  $('sig').textContent='sygnał: odb '+d.sat_tracked+', top4 '+d.snr_top4+' dB-Hz, HDOP '+(d.hdop||'--');
  $('trip').textContent=fmtKm(d.trip_km);$('total').textContent=fmtKm(d.total_km);
  $('max').textContent=d.max_kmh.toFixed(1)+' km/h';$('utc').textContent=d.utc||'--';
  if(d.fix){$('pos').textContent=d.lat.toFixed(6)+', '+d.lon.toFixed(6)+', '+d.alt_m.toFixed(0)+' m';
   $('map').href='https://www.openstreetmap.org/?mlat='+d.lat+'&mlon='+d.lon+'#map=17/'+d.lat+'/'+d.lon;$('map').style.display='';}
  $('sd').textContent=d.sd;
  $('wifi').innerHTML=d.wifi;
  $('gh').textContent=d.gh;$('ghinst').style.display=d.gh_available?'':'none';
 }catch(e){$('fix').innerHTML='<span class="err">brak połączenia</span>';}
}
async function files(){
 const r=await fetch('/api/files');const d=await r.json();const t=$('files');t.innerHTML='';
 if(!d.files.length){t.innerHTML='<tr><td class="k">'+(d.sd?'Brak tras':'Brak karty SD')+'</td></tr>';return;}
 d.files.sort((a,b)=>b.name.localeCompare(a.name));
 for(const f of d.files){
  const tr=document.createElement('tr');const kb=(f.size/1024).toFixed(1)+' KB';
  tr.innerHTML='<td><a href="/gpx/'+f.name+'" download>'+f.name+'</a></td><td class="k">'+kb+'</td><td style="text-align:right">'+
   (f.active?'<small>zapisywany</small>':'<button class="del">Usuń</button>')+'</td>';
  const b=tr.querySelector('button');
  if(b)b.onclick=async()=>{if(confirm('Usunąć '+f.name+'?')){await fetch('/api/delete?name='+encodeURIComponent(f.name),{method:'POST'});files();}};
  t.appendChild(tr);
 }
}
$('wf').onsubmit=async e=>{e.preventDefault();const fd=new FormData(e.target);
 await fetch('/api/wifi',{method:'POST',body:new URLSearchParams(fd)});e.target.reset();
 alert('Zapisano. CYD łączy się z siecią; nowy adres IP pokaże ekran WI-FI na CYD.');};
$('forget').onclick=async()=>{if(confirm('Zapomnieć sieć domową?'))await fetch('/api/forget',{method:'POST'});};
$('ghcheck').onclick=()=>fetch('/api/gh/check',{method:'POST'});
$('ghinst').onclick=async()=>{if(confirm('Zainstalować nową wersję z GitHub? CYD zrestartuje się po instalacji.'))await fetch('/api/gh/install',{method:'POST'});};
$('up').onclick=()=>{const f=$('fw').files[0];if(!f){return;}
 const x=new XMLHttpRequest();const fd=new FormData();fd.append('fw',f,f.name);
 x.upload.onprogress=e=>{$('upst').textContent='Wysyłanie '+Math.round(e.loaded*100/e.total)+'%';};
 x.onload=()=>{$('upst').textContent=x.responseText;};x.onerror=()=>{$('upst').textContent='Błąd połączenia';};
 x.open('POST','/update');x.send(fd);};
live();files();setInterval(live,1000);setInterval(files,15000);
</script></body></html>)HTML";

static bool auth()
{
    if (s_server.authenticate(net_user(), net_password())) {
        return true;
    }
    s_server.requestAuthentication(BASIC_AUTH, "CYD GPS");
    return false;
}

static void json_escape(String &out, const char *s)
{
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') {
            out += '\\';
        }
        if ((uint8_t)*s >= 0x20) {
            out += *s;
        }
    }
}

static void handle_root()
{
    if (!auth()) {
        return;
    }
    s_server.send_P(200, "text/html; charset=utf-8", k_page);
}

static void handle_status()
{
    if (!auth()) {
        return;
    }
    web_snapshot_t d;
    portENTER_CRITICAL(&s_snap_mux);
    d = s_snap;
    portEXIT_CRITICAL(&s_snap_mux);
    net_status_t n;
    net_get(&n);

    char wifi[200];
    if (!n.enabled) {
        snprintf(wifi, sizeof wifi, "Wi-Fi wyłączone");
    } else if (n.sta_connected) {
        snprintf(wifi, sizeof wifi, "Połączono z <b>%s</b>, IP %s, RSSI %d dBm%s", n.sta_ssid, n.sta_ip, n.rssi,
                 n.ap_on ? ", hotspot też włączony" : "");
    } else if (n.sta_configured) {
        snprintf(wifi, sizeof wifi, "Sieć <b>%s</b>: %s. Hotspot %s %s", n.sta_ssid,
                 n.sta_auth_failed ? "złe hasło" : "brak połączenia", n.ap_ssid, n.ap_on ? "włączony" : "");
    } else {
        snprintf(wifi, sizeof wifi, "Sieć domowa nie ustawiona. Hotspot %s włączony.", n.ap_ssid);
    }

    char num[256];
    snprintf(num, sizeof num,
             "\"version\":\"" FW_VERSION "\",\"fix\":%s,\"lat\":%.7f,\"lon\":%.7f,\"alt_m\":%.1f,\"speed_kmh\":%.1f,"
             "\"hdop\":%.1f,\"sats\":%u,\"sat_tracked\":%u,\"snr_top4\":%u,\"trip_km\":%.3f,\"total_km\":%.3f,"
             "\"max_kmh\":%.1f,\"moving_s\":%lu,",
             d.fix ? "true" : "false", d.lat, d.lon, d.alt_m, d.speed_kmh, d.hdop, d.sats, d.sat_tracked, d.snr_top4,
             d.trip_km, d.total_km, d.max_kmh, (unsigned long)d.moving_s);
    String out;
    out.reserve(640);
    out += '{';
    out += num;
    out += "\"utc\":\"";
    json_escape(out, d.utc);
    out += "\",\"sd\":\"";
    json_escape(out, d.sd);
    out += "\",\"wifi\":\"";
    json_escape(out, wifi);
    gh_status_t gh;
    gh_ota_get(&gh);
    char ght[80];
    gh_ota_text(ght, sizeof ght);
    out += "\",\"gh\":\"";
    json_escape(out, ght);
    out += "\",\"gh_available\":";
    out += gh.state == GH_AVAILABLE ? "true" : "false";
    out += '}';
    s_server.send(200, "application/json", out);
}

// Only plain GPX names produced by gpx_log ("20261123-114602.gpx"): no paths, no "..".
static bool valid_name(const String &n)
{
    if (n.length() < 5 || n.length() > 30 || !n.endsWith(".gpx")) {
        return false;
    }
    for (size_t i = 0; i < n.length() - 4; ++i) {
        const char c = n[i];
        if (!isdigit((unsigned char)c) && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

static bool is_active(const String &name)
{
    char active[32];
    portENTER_CRITICAL(&s_snap_mux);
    memcpy(active, s_snap.gpx_file, sizeof active);
    portEXIT_CRITICAL(&s_snap_mux);
    return active[0] && name == active;
}

static void handle_files()
{
    if (!auth()) {
        return;
    }
    String out = "{\"sd\":";
    File dir = SD.open("/gpx");
    out += dir ? "true" : "false";
    out += ",\"files\":[";
    bool first = true;
    if (dir) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String name = f.name();
            const int slash = name.lastIndexOf('/');
            if (slash >= 0) {
                name = name.substring(slash + 1);
            }
            if (!f.isDirectory() && valid_name(name)) {
                out += first ? "" : ",";
                first = false;
                out += "{\"name\":\"" + name + "\",\"size\":" + String((unsigned long)f.size()) +
                       ",\"active\":" + (is_active(name) ? "true" : "false") + "}";
            }
            f.close();
        }
        dir.close();
    }
    out += "]}";
    s_server.send(200, "application/json", out);
}

static void handle_download()
{
    if (!auth()) {
        return;
    }
    const String name = s_server.uri().substring(5);  // after "/gpx/"
    if (!valid_name(name)) {
        s_server.send(400, "text/plain", "bad name");
        return;
    }
    File f = SD.open("/gpx/" + name, FILE_READ);
    if (!f) {
        s_server.send(404, "text/plain", "not found");
        return;
    }
    s_server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    s_server.streamFile(f, "application/gpx+xml");
    f.close();
}

static void handle_delete()
{
    if (!auth()) {
        return;
    }
    const String name = s_server.arg("name");
    if (!valid_name(name)) {
        s_server.send(400, "text/plain", "bad name");
        return;
    }
    if (is_active(name)) {
        s_server.send(409, "text/plain", "file is being written");
        return;
    }
    const bool ok = SD.remove("/gpx/" + name);
    Serial.printf("WEB: delete %s %s\n", name.c_str(), ok ? "ok" : "failed");
    s_server.send(ok ? 200 : 500, "text/plain", ok ? "ok" : "failed");
}

static void handle_wifi()
{
    if (!auth()) {
        return;
    }
    const String ssid = s_server.arg("ssid");
    const String pass = s_server.arg("pass");
    if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 64 || (pass.length() && pass.length() < 8)) {
        s_server.send(400, "text/plain", "SSID 1-32 znaki, haslo puste albo 8-64 znaki");
        return;
    }
    net_request_home(ssid.c_str(), pass.c_str());
    s_server.send(200, "text/plain", "ok");
}

static void handle_forget()
{
    if (!auth()) {
        return;
    }
    net_request_forget();
    s_server.send(200, "text/plain", "ok");
}

static bool running_image_pending()
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void handle_update_upload()
{
    HTTPUpload &up = s_server.upload();
    if (up.status == UPLOAD_FILE_START) {
        s_update_ok = false;
        s_update_err[0] = 0;
        if (!s_server.authenticate(net_user(), net_password())) {
            snprintf(s_update_err, sizeof s_update_err, "brak autoryzacji");
            return;
        }
        if (gh_ota_busy()) {
            snprintf(s_update_err, sizeof s_update_err, "trwa aktualizacja z GitHub");
            return;
        }
        if (running_image_pending()) {  // never replace an image that has not proven itself yet
            snprintf(s_update_err, sizeof s_update_err, "poprzednia aktualizacja jeszcze niepotwierdzona, odczekaj 15 s");
            return;
        }
        Serial.printf("OTA: receiving %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            snprintf(s_update_err, sizeof s_update_err, "Update.begin: %s", Update.errorString());
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!s_update_err[0] && Update.write(up.buf, up.currentSize) != up.currentSize) {
            snprintf(s_update_err, sizeof s_update_err, "zapis: %s", Update.errorString());
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (!s_update_err[0]) {
            if (Update.end(true)) {  // checks the image (magic, size, SHA-256 appended by the build)
                s_update_ok = true;
                Serial.printf("OTA: %u bytes written\n", (unsigned)up.totalSize);
            } else {
                snprintf(s_update_err, sizeof s_update_err, "obraz odrzucony: %s", Update.errorString());
            }
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        snprintf(s_update_err, sizeof s_update_err, "przerwano");
    }
}

static void handle_update_done()
{
    if (!auth()) {
        return;
    }
    if (s_update_ok) {
        s_server.send(200, "text/plain", "OK, restart. Strona wróci za ~10 s.");
        net_request_reboot();
    } else {
        if (Update.isRunning()) {
            Update.abort();
        }
        Serial.printf("OTA: failed: %s\n", s_update_err[0] ? s_update_err : "no file");
        s_server.send(500, "text/plain", String("Błąd: ") + (s_update_err[0] ? s_update_err : "brak pliku"));
    }
}

static void handle_gh_check()
{
    if (!auth()) {
        return;
    }
    gh_ota_check_now();
    s_server.send(200, "text/plain", "ok");
}

static void handle_gh_install()
{
    if (!auth()) {
        return;
    }
    gh_ota_install();
    s_server.send(200, "text/plain", "ok");
}

static void server_task(void *)
{
    for (;;) {
        s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void web_begin()
{
    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/api/status", HTTP_GET, handle_status);
    s_server.on("/api/files", HTTP_GET, handle_files);
    s_server.on("/api/delete", HTTP_POST, handle_delete);
    s_server.on("/api/wifi", HTTP_POST, handle_wifi);
    s_server.on("/api/forget", HTTP_POST, handle_forget);
    s_server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
    s_server.on("/api/gh/check", HTTP_POST, handle_gh_check);
    s_server.on("/api/gh/install", HTTP_POST, handle_gh_install);
    s_server.onNotFound([] {
        if (s_server.uri().startsWith("/gpx/") && s_server.method() == HTTP_GET) {
            handle_download();
        } else {
            s_server.send(404, "text/plain", "not found");
        }
    });
    s_server.begin();
    // Core 0 next to the Wi-Fi stack; loop() (LVGL, GPS, SD logging) stays on core 1.
    xTaskCreatePinnedToCore(server_task, "web", 8192, NULL, 1, NULL, 0);
    Serial.println("WEB: server on port 80");
}

void web_publish(const web_snapshot_t *s)
{
    portENTER_CRITICAL(&s_snap_mux);
    s_snap = *s;
    portEXIT_CRITICAL(&s_snap_mux);
}
