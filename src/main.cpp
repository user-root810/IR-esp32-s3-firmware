/*
 * Télécommande IR universelle WiFi — ESP32-S3
 * ============================================
 * Apprend les codes IR de tes télécommandes, les stocke en flash,
 * et les rejoue depuis une page web hébergée par l'ESP32.
 *
 * Matériel :
 *   - Récepteur IR : OUT -> GPIO 4, VCC -> 3.3V, GND -> GND
 *   - LED IR       : GPIO 5 -> résistance -> LED IR -> GND
 *                    (idéalement via transistor NPN pour la portée)
 *
 * ⚠ Broches à éviter sur l'ESP32-S3 :
 *   GPIO 19/20 (USB natif), GPIO 26-32 (flash interne),
 *   GPIO 33-37 (PSRAM sur modules octal).
 *
 * Bibliothèques à installer (Library Manager) :
 *   - IRremoteESP8266
 *   - ArduinoJson  (v7)
 *
 * Carte : "ESP32S3 Dev Module"
 * ⚠ Si le moniteur série reste vide : Tools -> USB CDC On Boot -> Enabled
 */

#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// ---------------- À PERSONNALISER ----------------
const char* WIFI_SSID = "GrapheneOS";
const char* WIFI_PASS = "graphene";
// -------------------------------------------------

// Broches
const uint16_t kRecvPin = 4;    // DATA du récepteur IR
const uint16_t kIrLed   = 5;    // LED IR (via résistance/transistor)

// Objets
IRrecv irrecv(kRecvPin);
IRsend irsend(kIrLed);
WebServer server(80);
decode_results results;

// Stockage
const char* kCodesFile = "/codes.json";

// État de l'apprentissage
bool learning = false;
bool lastLearnOk = false;
unsigned long learnStart = 0;
const unsigned long kLearnTimeout = 10000;  // 10 secondes
String learnName;

// ============================================================
// Page web (intégrée dans le firmware)
// ============================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Télécommande IR</title>
<style>
 body{font-family:sans-serif;background:#16121f;color:#eee;text-align:center;padding:1em;max-width:480px;margin:auto}
 h1{font-weight:600}
 .btn{margin:.4em;padding:1em 1.6em;font-size:1.1em;border-radius:10px;border:none;background:#6d4aff;color:#fff;cursor:pointer}
 .del{background:#a33;padding:.5em .8em;font-size:.85em;border-radius:8px;border:none;color:#fff;cursor:pointer}
 input{padding:.8em;font-size:1em;border-radius:8px;border:none;width:60%}
 #msg{min-height:1.4em;color:#8f8;font-weight:600}
</style></head><body>
<h1>&#128250; Télécommande IR</h1>
<p id="msg"></p>
<input id="name" placeholder="ex : tv_power">
<br><button class="btn" onclick="learn()">&#128218; Apprendre</button>
<div id="buttons"></div>
<script>
const msg = t => document.getElementById('msg').textContent = t;

async function refresh(){
  const codes = await (await fetch('/codes')).json();
  const d = document.getElementById('buttons');
  d.innerHTML = '';
  for (const n in codes){
    d.innerHTML += `<p><button class="btn" onclick="send('${n}')">${n}</button>
      <button class="del" onclick="del('${n}')">&#10005;</button></p>`;
  }
}

async function send(n){
  msg('Envoi de ' + n + '...');
  await fetch('/send?name=' + encodeURIComponent(n));
  msg('Envoy&eacute; ✔');
}

async function del(n){
  await fetch('/delete?name=' + encodeURIComponent(n));
  msg(n + ' supprim&eacute;');
  refresh();
}

async function learn(){
  const n = document.getElementById('name').value.trim();
  if (!n){ msg("Donne d'abord un nom au bouton"); return; }
  msg('En attente du signal IR... pointe la t&eacute;l&eacute;commande ! (10 s)');
  await fetch('/learn?name=' + encodeURIComponent(n));
  for (let i = 0; i < 20; i++){
    await new Promise(r => setTimeout(r, 700));
    const s = await (await fetch('/status')).json();
    if (s.state == 'done'){ msg('&laquo; ' + n + ' &raquo; enregistr&eacute; ✔'); break; }
    if (s.state == 'idle') break;
  }
  document.getElementById('name').value = '';
  refresh();
}

refresh();
</script></body></html>
)rawliteral";

// ============================================================
// Stockage des codes (fichier JSON sur LittleFS)
// ============================================================
void loadCodes(JsonDocument& doc) {
  if (LittleFS.exists(kCodesFile)) {
    File f = LittleFS.open(kCodesFile, "r");
    if (f) deserializeJson(doc, f);
    f.close();
  }
  if (doc.isNull()) doc.to<JsonObject>();
}

bool saveCodes(JsonDocument& doc) {
  File f = LittleFS.open(kCodesFile, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

// ============================================================
// Sauvegarde du signal capturé
// ============================================================
void saveCapture() {
  JsonDocument doc;
  loadCodes(doc);

  JsonObject c = doc[learnName].to<JsonObject>();

  if (results.decode_type != UNKNOWN) {
    // Protocole reconnu : on stocke type / valeur / nb de bits
    c["type"]  = (int)results.decode_type;
    c["value"] = (uint64_t)results.value;
    c["bits"]  = results.bits;
    Serial.print("Code reconnu (");
    Serial.print(typeToString(results.decode_type));
    Serial.print(") : ");
    Serial.println(resultToHexidecimal(&results));
  } else {
    // Protocole inconnu : on stocke les timings bruts
    c["type"] = "raw";
    uint16_t* raw = resultToRawArray(&results);
    JsonArray t = c["timings"].to<JsonArray>();
    for (uint16_t i = 0; i < results.rawlen; i++) t.add(raw[i]);
    delete[] raw;
    Serial.printf("Signal inconnu capture en brut (%d timings)\n", results.rawlen);
  }

  if (saveCodes(doc)) Serial.println("Sauvegarde OK : " + learnName);
  else Serial.println("ERREUR de sauvegarde !");
}

// ============================================================
// Rejeu d'un code
// ============================================================
bool playCode(const char* name) {
  JsonDocument doc;
  loadCodes(doc);
  JsonObject c = doc[name];
  if (c.isNull()) return false;

  String type = c["type"];

  if (type == "raw") {
    JsonArray t = c["timings"];
    uint16_t n = t.size();
    if (n == 0 || n > 500) return false;
    static uint16_t buf[500];
    uint16_t i = 0;
    for (uint16_t v : t) buf[i++] = v;
    return irsend.sendRaw(buf, n, 38);  // 38 kHz : fréquence standard
  }

  int proto = type.toInt();
  uint64_t value = (uint64_t)(c["value"] | 0ULL);
  int bits = c["bits"] | 32;
  // Rejoue via l'envoi générique (nbytes = bits arrondis au multiple de 8)
  irsend.send((uint16_t)proto, value, (uint16_t)((bits + 7) / 8));
  return true;
}

// ============================================================
// Routes du serveur web
// ============================================================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleCodes() {
  JsonDocument doc;
  loadCodes(doc);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleLearn() {
  learnName = server.arg("name");
  if (learnName.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"nom manquant\"}");
    return;
  }
  lastLearnOk = false;
  learning = true;
  learnStart = millis();
  irrecv.enableIRIn();  // (ré)active le récepteur
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStatus() {
  String s = learning ? "learning" : (lastLearnOk ? "done" : "idle");
  server.send(200, "application/json", "{\"state\":\"" + s + "\"}");
}

void handleSend() {
  String name = server.arg("name");
  if (playCode(name.c_str())) {
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(404, "application/json", "{\"error\":\"code inconnu\"}");
  }
}

void handleDelete() {
  String name = server.arg("name");
  JsonDocument doc;
  loadCodes(doc);
  doc.remove(name);
  saveCodes(doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ============================================================
// Setup / Loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== IR WiFi (ESP32-S3) ===");

  // Stockage flash (formate au premier demarrage si besoin)
  if (!LittleFS.begin(true)) Serial.println("ERREUR LittleFS !");

  // LED IR
  irsend.begin();

  // Récepteur IR (pull-up interne nécessaire)
  pinMode(kRecvPin, INPUT_PULLUP);
  irrecv.setUnknownThreshold(12);  // capture brute des signaux inconnus

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connexion WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Page web disponible ici : http://");
  Serial.println(WiFi.localIP());

  // Serveur
  server.on("/",         HTTP_GET, handleRoot);
  server.on("/codes",    HTTP_GET, handleCodes);
  server.on("/learn",    HTTP_GET, handleLearn);
  server.on("/status",   HTTP_GET, handleStatus);
  server.on("/send",     HTTP_GET, handleSend);
  server.on("/delete",   HTTP_GET, handleDelete);
  server.begin();
  Serial.println("Serveur demarre.");
}

void loop() {
  server.handleClient();

  if (learning) {
    if (millis() - learnStart > kLearnTimeout) {
      learning = false;
      lastLearnOk = false;
      Serial.println("Apprentissage : timeout, rien capture.");
    } else if (irrecv.decode(&results)) {
      saveCapture();
      learning = false;
      lastLearnOk = true;
      irrecv.resume();
    }
  }
}
