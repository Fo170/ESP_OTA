// ════════════════════════════════════════════════════════════════
// ESP_OTA — TRONC COMMUN de la librairie
// ESP8266 + ESP32 · header-only
//
// ┌──────────────────────────────────────────────────────────────┐
// │ ARCHITECTURE (3 fichiers, une seule responsabilité chacun)   │
// │                                                              │
// │   ESP_OTA.h        ← CE FICHIER : le tronc commun            │
// │                      version, réglages, OtaEtat, accès Update │
// │                      AUCUN en-tête de serveur web            │
// │   ESP_OTA_Sync.h   ← adaptateur WebServer / ESP8266WebServer  │
// │   ESP_OTA_Async.h  ← adaptateur AsyncWebServer                │
// └──────────────────────────────────────────────────────────────┘
//
// Quel fichier inclure ?
//
//   Serveur synchrone (WebServer / ESP8266WebServer) → <ESP_OTA_Sync.h>
//   Serveur asynchrone (AsyncWebServer)              → <ESP_OTA_Async.h>
//
// ⚠️ COMPATIBILITÉ ASCENDANTE 1.x
//    Les projets 1.x font `#include <ESP_OTA.h>` puis `ESP_OTA<WebServer> ota;`.
//    Ce fichier inclut donc l'adaptateur SYNCHRONE à la fin, sauf si
//    `ESP_OTA_ASYNC_H` est déjà défini (cas d'ESP_OTA_Async.h, qui ne veut
//    que le tronc) ou si l'appelant définit `ESP_OTA_TRONC_SEUL`.
//    → Aucun des 10 binaires du parc n'a besoin d'être modifié.
//
// ⚠️ CE FICHIER NE DOIT JAMAIS INCLURE UN EN-TÊTE DE SERVEUR WEB
//    C'est la raison d'être de la découpe : un projet synchrone n'a pas
//    ESPAsyncWebServer dans ses lib_deps et ne doit pas la tirer ; un projet
//    asynchrone ne doit pas tirer WebServer.h. Toute inclusion de serveur
//    ici annule la découpe.
//
// Contenu du tronc : tout ce qui ne dépend pas du transport HTTP.
//   - version de la librairie
//   - réglages par défaut (route, marge, délais)
//   - OtaResultat / OtaEtat (état exposé à l'application)
//   - esp_otaMsgErreur() / esp_otaAbandonner() (accès à Update)
//
// Auteur : Olivier FOURNET · Licence : GPL-3.0-only
// ════════════════════════════════════════════════════════════════
#ifndef ESP_OTA_H
#define ESP_OTA_H

#include <Arduino.h>

// ⚠️ Le nom du header diffère selon la plateforme :
//    ESP8266 (coeur 3.x) : `Updater.h` — classe UpdaterClass, objet global Update
//    ESP32               : `Update.h`  — classe UpdateClass,  objet global Update
//    L'API utilisée ici (begin/write/end/hasError/printError) est identique.
#if defined(ESP8266)
  #include <Updater.h>
#else
  #include <Update.h>
#endif
#include <string.h>
#include <stdio.h>

#define __ESP_OTA_H_VERSION__ "1.1.0"
#define ESP_OTA_VERSION       __ESP_OTA_H_VERSION__

// Route par défaut. Doit rester `/update` : c'est ce que les scripts, les
// favoris et les pages existantes du parc utilisent.
#ifndef ESP_OTA_ROUTE_DEFAUT
  #define ESP_OTA_ROUTE_DEFAUT  "/update"
#endif

// Marge sous l'espace sketch réel. Au-delà, Update.begin() échoue de toute
// façon au moment d'écrire ; mieux vaut refuser tout de suite.
#ifndef ESP_OTA_MARGE
  #define ESP_OTA_MARGE  0x1000UL
#endif

// Délai entre la réponse HTTP et le redémarrage. Le navigateur doit avoir reçu
// la réponse AVANT que la carte ne coupe la connexion.
#ifndef ESP_OTA_DELAI_REBOOT_MS
  #define ESP_OTA_DELAI_REBOOT_MS  1500UL
#endif

// Garde temporelle : au-delà de ce silence entre deux morceaux, le transfert
// est déclaré mort. En LAN un client légitime envoie en continu (trou < 1 ms) ;
// 30 s laisse une marge énorme sans jamais geler la carte.
#ifndef ESP_OTA_DELAI_MORT_MS
  #define ESP_OTA_DELAI_MORT_MS  30000UL
#endif

/** Résultat du dernier transfert. */
enum OtaResultat : uint8_t {
  OTA_AUCUN   = 0,
  OTA_OK      = 1,
  OTA_ECHEC   = 2,
  OTA_ABANDON = 3
};

/** État exposé à l'application (télémétrie, page web, /sante…). */
struct OtaEtat {
  bool     enCours       = false;   // un upload est en cours
  bool     redemarrageDu = false;   // redémarrage programmé
  uint32_t taille        = 0;       // octets écrits dans le transfert courant
  uint32_t debutMs       = 0;       // millis() du début du transfert
  uint32_t dernierOctetMs= 0;       // millis() du dernier morceau reçu (garde F1)
  uint32_t dureeMs       = 0;       // durée du DERNIER transfert terminé
  uint32_t tentatives    = 0;       // POST multipart reçus
  uint32_t succes        = 0;       // transferts réussis
  uint32_t echecs        = 0;       // transferts en échec
  bool     chienAbonne   = false;   // le chien de garde est abonné (garde N1)
  OtaResultat dernier    = OTA_AUCUN;
  char     message[48]   = {0};     // dernier diagnostic lisible
};

/** Compose "<prefixe>: <erreur Update>" dans dst.
 *  ⚠️ API différente selon la plateforme :
 *     ESP8266 `getErrorString()` renvoie une String
 *     ESP32   `errorString()`    renvoie un const char* */
static inline void esp_otaMsgErreur(char* dst, size_t cap, const char* prefixe)
{
  if (!dst || cap == 0) return;
#if defined(ESP8266)
  snprintf(dst, cap, "%s: %s", prefixe, Update.getErrorString().c_str());
#else
  snprintf(dst, cap, "%s: %s", prefixe, Update.errorString());
#endif
}

/** Abandon propre. ESP8266 n'a pas `abort()` : `end()` suffit, l'image
 *  vide ne sera pas marquée démarrable. */
static inline void esp_otaAbandonner()
{
#if defined(ESP32)
  if (!Update.hasError()) Update.abort();
#else
  Update.end();
#endif
}

// ════════════════════════════════════════════════════════════════
// Compatibilité ascendante 1.x — voir l'encadré en tête de fichier.
//
// `ESP_OTA_ASYNC_H` est défini par ESP_OTA_Async.h AVANT qu'il n'inclue ce
// tronc : c'est ce qui empêche un projet asynchrone de tirer WebServer.h.
// ════════════════════════════════════════════════════════════════
#if !defined(ESP_OTA_TRONC_SEUL) && !defined(ESP_OTA_ASYNC_H)
  #include "ESP_OTA_Sync.h"
#endif

#endif // ESP_OTA_H
