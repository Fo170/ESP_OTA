// Exemple minimal — ESP_OTA sur un serveur web synchrone.
//
// Testé sur ESP8266 (esp12e) et ESP32 (esp32dev).
// L'URL est /update en GET (page) et POST (upload).
//
//   curl -F firmware=@firmware.bin http://<ip>/update

#include <Arduino.h>
#include <ESP8266WiFi.h>          // ou <WiFi.h> sur ESP32
#include <ESP8266WebServer.h>     // ou <WebServer.h> sur ESP32
#include <ESP_OTA_Sync.h>

const char* WIFI_SSID = "mon-reseau";
const char* WIFI_PASS = "mon-mot-de-passe";

ESP8266WebServer server(80);       // WebServer sur ESP32
ESP_OTA<ESP8266WebServer> ota;     // ESP_OTA<WebServer> sur ESP32

void onOtaDebut() {
  Serial.println(F("OTA : debut du transfert"));
}

void onOtaProgression(uint32_t ecrit) {
  Serial.print(F("OTA : "));
  Serial.print(ecrit);
  Serial.println(F(" o"));
}

void onOtaFin(bool ok) {
  Serial.print(F("OTA : "));
  Serial.println(ok ? F("reussi") : F("echec"));
}

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
  Serial.print(F("\nIP : "));
  Serial.println(WiFi.localIP());

  // Une route métier, pour montrer la cohabitation.
  server.on("/", []() { server.send(200, "text/plain", "OK"); });

  // ── OTA ────────────────────────────────────────────────────────
  ota.surDebut(onOtaDebut);
  ota.surProgression(onOtaProgression);
  ota.surFin(onOtaFin);

  // Décommenter pour protéger la page et l'upload.
  // ota.identifiants("admin", "motdepasse");

  ota.begin(server);          // /update — GET page, POST upload

  server.begin();
  Serial.println(F("Serveur demarre — OTA sur /update"));
}

void loop() {
  server.handleClient();
  ota.loop();                 // redemarrage differe apres un upload reussi
}
