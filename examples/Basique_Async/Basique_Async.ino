// Exemple minimal — ESP_OTA_Async sur un serveur web ASYNCHRONE.
//
// Testé en compilation sur ESP32 (esp32dev) et ESP8266 (esp12e) avec
// esp32async/ESPAsyncWebServer 3.7.3.
//
//   curl -F firmware=@firmware.bin http://<ip>/update
//
// ⚠️ DEUX DIFFÉRENCES AVEC L'EXEMPLE SYNCHRONE
//    1. `server.begin()` n'est pas appelé : l'AsyncWebServer démarre seul.
//    2. `if (ota.suspendu()) return;` n'est PAS une recommandation mais une
//       OBLIGATION : les callbacks d'upload tournent dans la tâche TCP, donc
//       sur un autre cœur que loop() sur ESP32. Écrire en flash pendant ce
//       temps corrompt le système de fichiers.

#include <Arduino.h>
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif
#include <ESPAsyncWebServer.h>
#include <ESP_OTA_Async.h>

const char* WIFI_SSID = "mon-reseau";
const char* WIFI_PASS = "mon-mot-de-passe";

AsyncWebServer server(80);
ESP_OTA_Async<AsyncWebServer> ota;

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
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/plain", "OK");
  });

  // ── OTA ────────────────────────────────────────────────────────
  ota.surDebut(onOtaDebut);
  ota.surProgression(onOtaProgression);
  ota.surFin(onOtaFin);

  // Décommenter pour protéger la page et l'upload (HTTP Basic).
  // ota.identifiants("admin", "motdepasse");

  ota.begin(server);          // /update — GET page, POST upload

  server.begin();
  Serial.println(F("Serveur demarre — OTA sur /update"));
}

void loop() {
  ota.loop();                 // redemarrage differe apres un upload reussi

  // ⚠️ OBLIGATOIRE en asynchrone : rien d'autre pendant une OTA.
  if (ota.suspendu()) return;

  // ... ici : mesures, envois Domoticz/Telegram, persistance ...
}
