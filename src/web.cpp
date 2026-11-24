#include "web.h"
#include <Arduino.h>
#include <SD.h>
#include <Update.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_ota_ops.h>
#include <string.h>
#include "gh_ota.h"
#include "gpx_log.h"
#include "bootlog.h"
#include "acc_hist.h"
#include "net.h"

static WebServer s_server(80);
// "Przejazdy" view (ride list, map, speed chart): src/web/rides.js, embedded by platformio.ini.
extern const char k_rides_js[] asm("_binary_src_web_rides_js_start");
extern const char k_runs_js[] asm("_binary_src_web_runs_js_start");
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static web_snapshot_t s_snap;
static bool s_update_ok;
static char s_update_err[64];

// ---- QR login -------------------------------------------------------------------------------
// One-time code: written by the loop task (screen), checked and consumed by the web task.
static portMUX_TYPE s_login_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_code[21];
static uint32_t s_code_at;
static bool s_logout_req;
// Sessions: owned by the web task, kept in NVS ("web"/"sess") so an OTA reboot keeps phones logged in.
static const int k_sessions = 4;  // newest first; a 5th login drops the oldest
static const int k_tok = 32;
static char s_sess[k_sessions][k_tok + 1];
static Preferences s_web_nvs;

static void random_token(char *out, int len)
{
    static const char k_chars[] = "abcdefghjkmnpqrstuvwxyz23456789";  // 31 symbols, ~4.95 bits each
    for (int i = 0; i < len; ++i) {
        out[i] = k_chars[esp_random() % (sizeof k_chars - 1)];
    }
    out[len] = 0;
}

static bool same(const char *a, const char *b, size_t n)  // constant time
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0;
}

static void save_sessions()
{
    s_web_nvs.putBytes("sess", s_sess, sizeof s_sess);
}

static void handle_logout_request()
{
    bool req;
    portENTER_CRITICAL(&s_login_mux);
    req = s_logout_req;
    s_logout_req = false;
    portEXIT_CRITICAL(&s_login_mux);
    if (req) {
        memset(s_sess, 0, sizeof s_sess);
        save_sessions();
        Serial.println("WEB: all browser logins forgotten");
    }
}

static bool session_ok()
{
    const String c = s_server.header("Cookie");
    const int at = c.indexOf("cyd_s=");
    if (at < 0 || c.length() < (unsigned)(at + 6 + k_tok)) {
        return false;
    }
    const char *tok = c.c_str() + at + 6;
    for (int i = 0; i < k_sessions; ++i) {
        if (s_sess[i][0] && same(tok, s_sess[i], k_tok)) {
            return true;
        }
    }
    return false;
}

bool web_login_url(char *buf, size_t n)
{
    net_status_t ns;
    net_get(&ns);
    const char *ip = ns.sta_connected ? ns.sta_ip : ns.ap_on ? ns.ap_ip : "";
    if (!ip[0]) {
        return false;
    }
    char code[sizeof s_code];
    random_token(code, sizeof code - 1);
    portENTER_CRITICAL(&s_login_mux);
    memcpy(s_code, code, sizeof s_code);
    s_code_at = millis();
    portEXIT_CRITICAL(&s_login_mux);
    snprintf(buf, n, "http://%s/login?t=%s", ip, code);
    return true;
}

void web_login_cancel()
{
    portENTER_CRITICAL(&s_login_mux);
    memset(s_code, 0, sizeof s_code);
    portEXIT_CRITICAL(&s_login_mux);
}

void web_logout_all()
{
    web_login_cancel();
    portENTER_CRITICAL(&s_login_mux);
    s_logout_req = true;
    portEXIT_CRITICAL(&s_login_mux);
}

static void handle_login()
{
    const String t = s_server.arg("t");
    bool ok = false;
    portENTER_CRITICAL(&s_login_mux);
    if (s_code[0] && t.length() == sizeof s_code - 1 && millis() - s_code_at < WEB_LOGIN_CODE_MS &&
        same(t.c_str(), s_code, sizeof s_code - 1)) {
        ok = true;
        memset(s_code, 0, sizeof s_code);  // one use only
    }
    portEXIT_CRITICAL(&s_login_mux);
    if (!ok) {
        s_server.send(403, "text/html; charset=utf-8",
                      "<meta name=viewport content='width=device-width'><body style='font-family:system-ui'>"
                      "<h3>Kod logowania wygasł albo został już użyty</h3>"
                      "<p>Na CYD: ekran WI-FI, przytrzymaj „QR” i zeskanuj nowy kod.</p>");
        return;
    }
    memmove(s_sess[1], s_sess[0], sizeof s_sess - sizeof s_sess[0]);
    random_token(s_sess[0], k_tok);
    save_sessions();
    Serial.println("WEB: browser logged in with the QR code");
    s_server.sendHeader("Set-Cookie", String("cyd_s=") + s_sess[0] +
                                          "; Max-Age=2592000; Path=/; HttpOnly; SameSite=Lax");
    s_server.sendHeader("Location", "/");
    s_server.send(302, "text/plain", "");
}

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
button.del{background:#5a2424;border-color:#7a3030}
.ride{display:block;padding:10px 2px;border-bottom:1px solid #2a3138;color:inherit;text-decoration:none}
.ride:last-child{border-bottom:0}.rtop{display:flex;justify-content:space-between;align-items:center;gap:8px}
.rwhen{font-weight:600}.badge{background:#1f5a36;color:#4cd97b;border-radius:10px;padding:1px 8px;font-size:12px}
#ride{position:fixed;inset:0;background:#101418;overflow:auto;padding:12px 16px;z-index:1000}
#ride[hidden]{display:none}body.noscroll{overflow:hidden}
.rhead{display:flex;gap:12px;align-items:center;margin-bottom:10px}
.map{height:46vh;min-height:240px;border-radius:10px;margin-bottom:6px}
svg.trk,svg.chart{display:block;width:100%;height:auto;background:#1a2026;border-radius:10px;margin:6px 0}
.stats{margin:12px 0}.ract{display:flex;gap:10px;align-items:center;margin:16px 0 28px}
a.btn{border-radius:6px;border:1px solid #3a434c;background:#2a3138;color:#e8ecef;padding:6px 10px;text-decoration:none}input{width:100%;box-sizing:border-box;margin:4px 0}
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

<h2>Pomiary przyspieszenia</h2>
<div class="card"><table id="acc"></table>
<small id="accnote">Na CYD: ekran POMIAR. Stań na 1 s, ruszaj — czas startuje sam. Tylko na zamkniętym torze.</small>
<div id="runs"></div></div>
<h2>Przejazdy</h2>
<div class="card" id="rides"><div class="k">…</div></div>
<div id="ride" hidden></div>

<h2>Wi‑Fi domowe</h2>
<div class="card">
 <div id="wifi" class="k"></div>
 <form id="wf"><input name="ssid" placeholder="Nazwa sieci (SSID)" maxlength="32" required>
 <input name="pass" type="password" placeholder="Hasło" maxlength="64">
 <button>Zapisz i połącz</button> <button type="button" id="forget">Zapomnij sieć</button></form>
 <small>Sieć tylko 2,4 GHz. Gdy domowa sieć działa, hotspot CYD wyłącza się sam.</small>
</div>

<h2>Ekran CYD</h2>
<div class="card"><div class="k">Obrót</div>
 <button class="rot" data-r="0">0°</button> <button class="rot" data-r="1">90°</button>
 <button class="rot" data-r="2">180°</button> <button class="rot" data-r="3">270°</button>
 <small style="display:block;margin-top:6px">Zapamiętany po restarcie. Na CYD: przycisk ⟳ na ekranie WI‑FI.</small>
 <label style="display:flex;gap:8px;align-items:center;margin-top:12px"><input type="checkbox" id="dev" style="width:auto">
 Tryb dewelopera</label>
 <small style="display:block">Pokazuje na CYD dane techniczne: HDOP, sygnał w dB, częstotliwość GPS, plik GPX, wersję.</small></div>

<h2>Aktualizacja firmware</h2>
<div class="card">
 <div id="gh" class="v" style="font-size:16px"></div>
 <button id="ghcheck">Sprawdź teraz</button> <button id="ghinst" style="display:none">Zainstaluj z GitHub</button>
 <small style="display:block;margin:6px 0 12px">Działa w sieci domowej (z internetem). Instalacja trwa ok. minuty, potem restart.</small>
 <div class="k">albo plik z komputera:</div>
 <input type="file" id="fw" accept=".bin"> <button id="up">Wgraj</button>
 <div id="upst" class="k"></div>
 <small>Plik .pio/build/cyd2usb/firmware.bin. Jeśli nowa wersja nie uruchomi Wi‑Fi w 15 s, CYD wróci do poprzedniej.</small>
 <small id="boots" style="display:block;margin-top:8px"></small>
</div>

<script src="/rides.js?v=)HTML" FW_VERSION R"HTML("></script>
<script src="/runs.js?v=)HTML" FW_VERSION R"HTML("></script>
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
  $('boots').textContent=d.boots||'';$('gh').textContent=d.gh;$('ghinst').style.display=d.gh_available?'':'none';
  if(document.activeElement!==$('dev'))$('dev').checked=!!d.dev;
  document.querySelectorAll('.rot').forEach(b=>{b.style.outline=+b.dataset.r===d.rot?'2px solid #4cd97b':'';});
  if(d.acc){const a=d.acc,n=['0–50 km/h','0–100 km/h','0–200 km/h','100–200 km/h','100 m','1/4 mili'],f=v=>v>0?v.toFixed(2).replace('.',',')+' s':'—';
   const gr=g=>g==null?'':Math.abs(g)<0.5?'płasko':(g>0?'pod górę ':'z górki ')+Math.abs(g).toFixed(1).replace('.',',')+' %';
   const sp=[0,0,0,0,a.v100m,a.vquarter];let h='<tr><td class="k"></td><td class="k">ostatni'+(a.running?' (trwa)':'')+'</td><td class="k">najlepszy</td></tr>';
   n.forEach((x,i)=>{h+='<tr><td>'+x+'</td><td class="v" style="font-size:18px">'+(a.has_last?f(a.last[i]):'—')+
    (a.has_last&&a.last[i]>0&&sp[i]?' <small>'+sp[i]+' km/h</small>':'')+'</td><td>'+f(a.best[i])+
    (a.best[i]>0&&a.best_grade[i]!=null?' <small>'+gr(a.best_grade[i])+'</small>':'')+'</td></tr>';});
   $('acc').innerHTML=h;
   const road=a.has_last&&!a.running&&a.grade!=null?'Droga ostatniego pomiaru: '+gr(a.grade)+' ('+(a.climb>0?'+':'')+a.climb.toFixed(1).replace('.',',')+' m wysokości). Z górki czasy są lepsze. ':'';
   $('accnote').textContent=a.has_last&&a.aborted?'Ostatni przerwany: utrata GPS.':a.has_last&&a.low_rate?'Ostatni przy GPS 1 Hz: wynik przybliżony, nie liczy się do najlepszych.':road+'Na CYD: ekran POMIAR. Stań na 1 s, ruszaj — czas startuje sam. Nachylenie z wysokości GPS: orientacyjne (±1–2 %). Tylko na zamkniętym torze.';}
 }catch(e){$('fix').innerHTML='<span class="err">brak połączenia</span>';}
}
$('wf').onsubmit=async e=>{e.preventDefault();const fd=new FormData(e.target);
 await fetch('/api/wifi',{method:'POST',body:new URLSearchParams(fd)});e.target.reset();
 alert('Zapisano. CYD łączy się z siecią; nowy adres IP pokaże ekran WI-FI na CYD.');};
$('forget').onclick=async()=>{if(confirm('Zapomnieć sieć domową?'))await fetch('/api/forget',{method:'POST'});};
$('dev').onchange=e=>fetch('/api/dev',{method:'POST',body:new URLSearchParams({on:e.target.checked?1:0})}).then(live);
document.querySelectorAll('.rot').forEach(b=>{b.onclick=()=>fetch('/api/rotate',{method:'POST',body:new URLSearchParams({r:b.dataset.r})}).then(live);});
$('ghcheck').onclick=()=>fetch('/api/gh/check',{method:'POST'});
$('ghinst').onclick=async()=>{if(confirm('Zainstalować nową wersję z GitHub? CYD zrestartuje się po instalacji.'))await fetch('/api/gh/install',{method:'POST'});};
$('up').onclick=()=>{const f=$('fw').files[0];if(!f){return;}
 const x=new XMLHttpRequest();const fd=new FormData();fd.append('fw',f,f.name);
 x.upload.onprogress=e=>{$('upst').textContent='Wysyłanie '+Math.round(e.loaded*100/e.total)+'%';};
 x.onload=()=>{$('upst').textContent=x.responseText;};x.onerror=()=>{$('upst').textContent='Błąd połączenia';};
 x.open('POST','/update');x.send(fd);};
live();setInterval(live,1000);
if(window.CydRides)CydRides.init();else $('rides').textContent='Nie udało się wczytać widoku przejazdów.';
if(window.CydRuns)CydRuns.init($('runs'));
</script></body></html>)HTML";

static bool auth()
{
    if (session_ok() || s_server.authenticate(net_user(), net_password())) {
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

static void handle_rides_js()
{
    if (!auth()) {
        return;
    }
    s_server.sendHeader("Cache-Control", "private, max-age=3600");
    s_server.send_P(200, "application/javascript; charset=utf-8", k_rides_js);
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
    // NAN -> null (JSON has no NaN)
    auto jnum = [&out](float v, const char *fmt) {
        char b[16];
        if (v != v) {
            out += "null";
        } else {
            snprintf(b, sizeof b, fmt, v);
            out += b;
        }
    };
    const int acc_n = sizeof d.acc_last / sizeof d.acc_last[0];
    out += ",\"acc\":{\"last\":[";
    for (int i = 0; i < acc_n; ++i) {
        out += i ? "," : "";
        jnum(d.acc_last[i], "%.2f");
    }
    out += "],\"best\":[";
    for (int i = 0; i < acc_n; ++i) {
        out += i ? "," : "";
        jnum(d.acc_best[i], "%.2f");
    }
    out += "],\"best_grade\":[";
    for (int i = 0; i < acc_n; ++i) {
        out += i ? "," : "";
        jnum(d.acc_best_grade[i], "%.1f");
    }
    out += "],\"grade\":";
    jnum(d.acc_grade, "%.1f");
    out += ",\"climb\":";
    jnum(d.acc_climb, "%.1f");
    char acc[200];
    snprintf(acc, sizeof acc,
             ",\"v100m\":%.0f,\"vquarter\":%.0f,\"vmax\":%.0f,\"has_last\":%s,\"running\":%s,\"aborted\":%s,"
             "\"low_rate\":%s},\"rot\":%u,\"dev\":%s",
             d.acc_v100m, d.acc_vquarter, d.acc_vmax, d.acc_has_last ? "true" : "false",
             d.acc_running ? "true" : "false", d.acc_aborted ? "true" : "false", d.acc_low_rate ? "true" : "false",
             (unsigned)d.rot, d.dev ? "true" : "false");
    out += acc;
    char boots[400];
    bootlog_text(boots, sizeof boots);
    out += ",\"boots\":\"";
    json_escape(out, boots);
    out += "\"}";
    s_server.send(200, "application/json", out);
}

// Only plain GPX names produced by gpx_log ("20261123-114602.gpx"): no paths, no "..".
static bool valid_name(const String &n)
{
    return gpx_log_valid_name(n.c_str());
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
        if (!session_ok() && !s_server.authenticate(net_user(), net_password())) {
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

static volatile int s_rot_req = -1, s_dev_req = -1;

int web_take_dev()
{
    const int r = s_dev_req;
    if (r >= 0) {
        s_dev_req = -1;
    }
    return r;
}

static void handle_dev()
{
    if (!auth()) {
        return;
    }
    const String v = s_server.arg("on");
    if (v != "0" && v != "1") {
        s_server.send(400, "text/plain", "on=0|1");
        return;
    }
    s_dev_req = v == "1";
    s_server.send(200, "text/plain", "ok");
}

int web_take_rotation()
{
    const int r = s_rot_req;
    if (r >= 0) {
        s_rot_req = -1;
    }
    return r;
}

// ---- acceleration run history (files written by src/accel.cpp) ----------------------------
static volatile bool s_clear_runs;

bool web_take_clear_runs()
{
    const bool r = s_clear_runs;
    s_clear_runs = false;
    return r;
}

static void handle_runs_js()
{
    if (!auth()) {
        return;
    }
    s_server.sendHeader("Cache-Control", "private, max-age=3600");
    s_server.send_P(200, "application/javascript; charset=utf-8", k_runs_js);
}

// GET /api/runs: the run list (CSV); ?dl=1 as a download (export to a spreadsheet)
static void handle_runs()
{
    if (!auth()) {
        return;
    }
    if (s_server.hasArg("dl")) {
        s_server.sendHeader("Content-Disposition", "attachment; filename=\"pomiary.csv\"");
    }
    File f = SD.open("/runs/runs.csv", FILE_READ);
    if (!f) {
        s_server.send(200, "text/csv; charset=utf-8", acc_hist_header());  // no runs yet
        return;
    }
    s_server.streamFile(f, "text/csv; charset=utf-8");
    f.close();
}

// GET /api/run?id=20261124-153012: speed trace of one run (t_s,kmh)
static void handle_run()
{
    if (!auth()) {
        return;
    }
    const String id = s_server.arg("id");
    if (!acc_hist_valid_id(id.c_str())) {
        s_server.send(400, "text/plain", "bad id");
        return;
    }
    File f = SD.open("/runs/" + id + ".csv", FILE_READ);
    if (!f) {
        s_server.send(404, "text/plain", "not found");
        return;
    }
    s_server.sendHeader("Cache-Control", "private, max-age=86400");  // a saved trace never changes
    s_server.streamFile(f, "text/csv; charset=utf-8");
    f.close();
}

static void handle_runs_clear()
{
    if (!auth()) {
        return;
    }
    s_clear_runs = true;
    s_server.send(200, "text/plain", "ok");
}

static void handle_rotate()
{
    if (!auth()) {
        return;
    }
    const String r = s_server.arg("r");
    if (r.length() != 1 || r[0] < '0' || r[0] > '3') {
        s_server.send(400, "text/plain", "r=0..3");
        return;
    }
    s_rot_req = r[0] - '0';
    s_server.send(200, "text/plain", "ok");
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
        handle_logout_request();
        s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void web_begin()
{
    s_web_nvs.begin("web", false);
    if (s_web_nvs.getBytes("sess", s_sess, sizeof s_sess) != sizeof s_sess) {
        memset(s_sess, 0, sizeof s_sess);
    }
    for (auto &s : s_sess) {
        s[k_tok] = 0;
    }
    static const char *k_headers[] = {"Cookie"};
    s_server.collectHeaders(k_headers, 1);
    s_server.on("/login", HTTP_GET, handle_login);
    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/rides.js", HTTP_GET, handle_rides_js);
    s_server.on("/runs.js", HTTP_GET, handle_runs_js);
    s_server.on("/api/runs", HTTP_GET, handle_runs);
    s_server.on("/api/run", HTTP_GET, handle_run);
    s_server.on("/api/runs/clear", HTTP_POST, handle_runs_clear);
    s_server.on("/api/status", HTTP_GET, handle_status);
    s_server.on("/api/files", HTTP_GET, handle_files);
    s_server.on("/api/delete", HTTP_POST, handle_delete);
    s_server.on("/api/wifi", HTTP_POST, handle_wifi);
    s_server.on("/api/forget", HTTP_POST, handle_forget);
    s_server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
    s_server.on("/api/gh/check", HTTP_POST, handle_gh_check);
    s_server.on("/api/rotate", HTTP_POST, handle_rotate);
    s_server.on("/api/dev", HTTP_POST, handle_dev);
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
