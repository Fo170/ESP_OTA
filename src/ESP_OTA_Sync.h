// ════════════════════════════════════════════════════════════════
// ESP_OTA_Sync — mise à jour du firmware par HTTP, en UNE étape
// ESP8266 + ESP32 · header-only · serveur web SYNCHRONE
//
// ┌──────────────────────────────────────────────────────────────┐
// │ CE FICHIER = ADAPTATEUR SYNCHRONE (WebServer / ESP8266WebServer)│
// │ Tronc commun : ESP_OTA.h      (version, état, accès Update)     │
// │ Autre adaptateur : ESP_OTA_Async.h (AsyncWebServer)            │
// │                                                              │
// │ ⚠️ `#include <ESP_OTA.h>` reste équivalent pour compatibilité  │
// │    1.x : le tronc inclut ce fichier sauf si ESP_OTA_ASYNC_H    │
// │    est défini ou si ESP_OTA_TRONC_SEUL l'est.                  │
// └──────────────────────────────────────────────────────────────┘
//
// Rôle : remplacer ElegantOTA, qui impose un protocole en DEUX étapes et
// répond « 200 OK » même quand rien n'a été écrit.
//
// ── CE QUE CE MODULE CORRIGE ────────────────────────────────────
//
//  1. PROTOCOLE EN DEUX ÉTAPES (ElegantOTA 3.x)
//        GET  /ota/start   → Update.begin()
//        POST /ota/upload  → Update.write() / Update.end()
//     Un POST direct — ce que fait `curl -F` — laisse Update NON initialisé.
//     L'échec est alors TOTALEMENT SILENCIEUX :
//        Update.begin()  jamais appelé  → _size == 0
//        Update.write()  renvoie 0, retour ignoré par la librairie
//        Update.end()    sort sur (_size == 0) SANS positionner _error
//        handler final   hasError() faux → 200 OK + redémarrage
//     La carte reboute sur l'ANCIEN firmware en annonçant un succès.
//     → ICI : Update.begin() est appelé à UPLOAD_FILE_START. Un seul POST
//       multipart suffit, et le code HTTP reflète l'état RÉEL.
//
//  2. CRASH DISTANT SANS AUTHENTIFICATION (constaté le 25/10/2026)
//     Tout POST NON multipart emprunte le chemin RAW de _parseRequest, où
//     _currentUpload n'existe pas : `server.upload()` déréférence un pointeur
//     nul → panique → la carte reboute. N'importe qui sur le LAN peut la
//     rebooter en boucle, sans mot de passe.
//     → ICI : le Content-Type est testé AVANT `server.upload()`.
//
//  3. DÉPENDANCES INUTILES
//     ElegantOTA tire ESPAsyncWebServer (~98 Ko d'archive) + ESPAsyncTCP
//     (~16 Ko) alors que le parc a des serveurs SYNCHRONES.
//     → ICI : Update.h du coeur Arduino, rien d'autre.
//     ⚠️ Ce fichier ne référence JAMAIS ESPAsyncWebServer : un projet
//        synchrone n'a pas la dépendance dans ses lib_deps et ne doit pas
//        la tirer. L'adaptateur asynchrone vit dans ESP_OTA_Async.h, qui
//        est OPT-IN.
//
//  4. REDÉMARRAGE TROP PRÉCOCE
//     Redémarrer dans le handler final coupe la réponse : le navigateur
//     affiche une erreur alors que l'OTA a réussi.
//     → ICI : redémarrage DIFFÉRÉ, piloté par loop().
//
// ⚠️ CONTRAINTES DE CONCEPTION (ne pas régresser)
//   - aucune allocation dynamique pour la page : tampon fixe, snprintf
//   - les handlers sont des FONCTIONS SIMPLES (pas de capture) : server.on()
//     n'accepte pas de lambda capturante sur toutes les versions
//   - un seul upload à la fois : `enCours()` doit être testé par l'appelant
//     avant tout ESP.restart() (redémarrage horaire, watchdog d'inactivité)
//
// Auteur : Olivier FOURNET · Licence : GPL-3.0-only
// ════════════════════════════════════════════════════════════════
#ifndef ESP_OTA_SYNC_H
#define ESP_OTA_SYNC_H

// ⚠️ Le tronc (version, réglages, OtaEtat, accès Update) est partagé avec
//    l'adaptateur asynchrone. Il ne contient AUCUN en-tête de serveur web,
//    ce qui permet de le réutiliser sans tirer une dépendance inutile.
//    ⚠️ Ne PAS inclure ESP_OTA.h depuis ESP_OTA.h : la garde ESP_OTA_ASYNC_H
//       empêche la récursion (voir la fin du tronc).
#include "ESP_OTA.h"

// ⚠️ Ticker : surveillance d'un transfert muet. La boucle `_uploadReadByte`
//    du coeur ESP8266 n'a AUCUN timeout ; `loop()` ne reprend pas la main
//    tant qu'elle bloque. Seul un minuteur peut couper la socket depuis la
//    tâche SDK, ce qui débloque la boucle (elle teste `client.connected()`).
#if defined(ESP8266)
  #include <Ticker.h>
#endif

// ⚠️ La librairie doit être AUTONOME : sans le header du serveur web, HTTP_GET,
//    HTTP_POST, HTTPUpload, UPLOAD_FILE_* et CONTENT_LENGTH_UNKNOWN ne sont pas
//    définis. Ne PAS compter sur l'ordre des #include du projet appelant.
//    ⚠️ SYNCHRONE UNIQUEMENT. Un projet qui utilise AsyncWebServer doit inclure
//       <ESP_OTA_Async.h> et NON ce fichier (voir README, section Asynchrone).
#if defined(ESP8266)
  #include <ESP8266WebServer.h>
#elif defined(ESP32)
  #include <WebServer.h>
#endif

/**
 * Gestionnaire OTA pour un serveur web synchrone.
 *
 * `Serveur` vaut `WebServer` (ESP32) ou `ESP8266WebServer` (ESP8266).
 *
 * Utilisation minimale :
 *     ESP_OTA<WebServer> ota;
 *     setup() : ota.begin(server);
 *     loop()  : ota.loop();
 */
template <typename Serveur>
class ESP_OTA {
public:
  ESP_OTA() { _self = this; }

  /** Enregistre les routes (GET page, POST upload).
   *
   *  @param server          le serveur web, déjà instancié
   *  @param route           URL de la page et de l'upload (défaut `/update`)
   *  @param titre           nom affiché sur la page (défaut : nom du modèle)
   *  @param gardeMultipart  refuse les POST non multipart (corrige le crash
   *                         distant). Exige que `Content-Type` et `Origin`
   *                         soient collectés — begin() s'en charge.
   *  @param routeUpload     URL du POST si elle diffère de la page
   *                         (ex. `/ota/upload`). nullptr = même URL que `route`.
   *  @return false si les routes n'ont pas pu être enregistrées
   *
   *  ⚠️ Si `gardeMultipart` est vrai, begin() appelle
   *     `server.collectHeaders({"Content-Type","Origin"}, 2)`. Ce doit être le DERNIER
   *     collectHeaders() du projet : il remplace la liste collectée
   *     (`Authorization` reste en position 0, donc `authenticate()` continue
   *     de fonctionner). Passer `false` si le projet collecte d'autres
   *     en-têtes et veut garder sa liste.
   */
  bool begin(Serveur& server, const char* route = ESP_OTA_ROUTE_DEFAUT,
             const char* titre = nullptr, bool gardeMultipart = true,
             const char* routeUpload = nullptr)
  {
    if (!route || route[0] != '/') return false;
    if (routeUpload && routeUpload[0] != '/') return false;
    _server      = &server;
    _route       = route;
    _routeUpload = routeUpload ? routeUpload : route;
    _titre       = (titre && titre[0]) ? titre : nullptr;

    if (gardeMultipart) {
      // ⚠️ API différente selon la plateforme :
      //    ESP8266 : surcharge VARIADIQUE collectHeaders("...") — la forme
      //              tableau serait capturée par le template variadique et
      //              échouerait à convertir const char*[1] en String.
      //    ESP32   : forme tableau classique.
      // ⚠️ DEUX en-têtes, pas un :
      //    - `Content-Type` : indispensable à la garde anti-crash (I3), qui
      //      distingue le chemin multipart du chemin RAW.
      //    - `Origin`       : indispensable au crochet `avantUpload()`. Un
      //      projet qui y fait un contrôle anti-CSRF lit `server.header("Origin")`
      //      — sans collecte, cet appel renvoie TOUJOURS "" et le contrôle
      //      laisse tout passer. C'est exactement le défaut constaté le
      //      14/11/2026 : l'envoi a été accepté avec une Origin étrangère.
#if defined(ESP8266)
      _server->collectHeaders("Content-Type", "Origin");
#else
      static const char* cles[] = { "Content-Type", "Origin" };
      _server->collectHeaders(cles, 2);
#endif
    }
    _gardeMultipart = gardeMultipart;

    _server->on(_route, HTTP_GET,  _page);
    _server->on(_routeUpload, HTTP_POST, _final, _morceau);
    return true;
  }

  /** Protège la page et l'upload par HTTP Basic. À appeler avant begin(). */
  void identifiants(const char* utilisateur, const char* motDePasse)
  {
    _user = utilisateur;
    _pass = motDePasse;
  }

  /** Abonne un chien de garde pendant TOUT le transfert (garde N1).
   *
   *  ⚠️ POURQUOI C'EST INDISPENSABLE
   *     `Parsing.cpp` (`_uploadReadByte`) fait
   *        while (!client.available() && client.connected()) delay(2);
   *     SANS AUCUN TIMEOUT. Un client qui envoie les en-têtes puis se tait en
   *     laissant la socket ouverte fige `handleClient()` À VIE : le serveur web
   *     meurt, `enCours()` reste armé, et aucun watchdog ne réagit si la tâche
   *     n'est pas abonnée. Panne totale, non auto-réparatrice, **déclenchable
   *     sans authentification**.
   *
   *  `ajouter` est appelé à UPLOAD_FILE_START, `rearmer` à chaque morceau reçu,
   *  `retirer` sur TOUS les chemins de sortie. Passer nullptr pour désactiver.
   *
   *  Exemple avec Crash_ESP_Monitor :
   *      ota.chienDeGarde(CM_WatchDogAdd, CM_WatchDogReset, CM_WatchDogRemove);
   */
  void chienDeGarde(void (*ajouter)(), void (*rearmer)(), void (*retirer)())
  {
    _chienAjouter = ajouter;
    _chienRearmer = rearmer;
    _chienRetirer = retirer;
  }

  /** Garde temporelle (F1) : au-delà de ce silence, le transfert est déclaré
   *  mort. 0 = désactivée. Défaut : 30 s. */
  void delaiSansOctet(uint32_t ms) { _delaiSansOctet = ms; }

  /** Surveillance ACTIVE du transfert muet (F7, ESP8266 uniquement).
   *
   *  ⚠️ POURQUOI ELLE EST NÉCESSAIRE
   *     `_uploadReadByte()` du coeur ESP8266 boucle
   *        while (!client.available() && client.connected()) yield();
   *     SANS timeout. Un client qui envoie les en-têtes puis se tait en
   *     gardant sa socket ouverte fige `handleClient()` À VIE : `loop()` ne
   *     reprend jamais la main, donc `transfertMort()` ne peut PAS s'exécuter
   *     et aucun redémarrage de panne ne se déclenche. Seule une coupure
   *     d'alimentation en sortait.
   *     → Un `Ticker` (tâche SDK, qui continue de tourner pendant que la
   *       tâche principale est suspendue) coupe la socket après
   *       `delaiSansOctet()`. La boucle bloquée voit `connected()` faux,
   *       sort, et le handler reçoit `UPLOAD_FILE_ABORTED` → image libérée.
   *
   *  `true` par défaut. `false` ne se justifie que pour reproduire le défaut.
   *  Sans effet sur ESP32 (autre pile, autre modèle de tâches). */
  void surveillance(bool v) { _survActif = v; }

  /** Vrai si le transfert en cours n'a plus donné signe de vie depuis
   *  `delaiSansOctet()`. À tester depuis la boucle pour couper court. */
  bool transfertMort() const
  {
    return _e.enCours && _delaiSansOctet > 0
        && (millis() - _e.dernierOctetMs) > _delaiSansOctet;
  }

  /** Crochet de REFUS, appelé à UPLOAD_FILE_START **avant** `Update.begin()`,
   *  donc avant le moindre octet écrit en flash.
   *
   *  Retourner `false` annule l'envoi : rien n'est écrit, et le handler final
   *  répond 403. C'est l'endroit pour un contrôle anti-CSRF (`Origin`), une
   *  authentification applicative, un quota, etc.
   *
   *  ⚠️ Ne pas confondre avec `identifiants()` : celui-ci est un HTTP Basic
   *     standard, celui-là est un contrôle libre décidé par le projet.
   *
   *  Exemple :
   *      static bool origineOk() { return origineLocale(); }
   *      ota.avantUpload(origineOk);
   */
  void avantUpload(bool (*f)()) { _avant = f; }

  /** Taille annoncée à `Update.begin()`.
   *  `true` = UPDATE_SIZE_UNKNOWN (ESP32 uniquement) : le coeur choisit la
   *  partition OTA. C'est le comportement éprouvé en production sur
   *  l'Ordonnanceur. `false` (défaut) = espace sketch libre moins la marge.
   *  Sans effet sur ESP8266, où la constante n'existe pas. */
  void tailleInconnue(bool v) { _tailleInconnue = v; }

  /** Libère l'état du transfert et désabonne le chien de garde.
   *  Idempotent — appelé sur tous les chemins de sortie. */
  void liberer()
  {
    if (_e.chienAbonne) {
      _e.chienAbonne = false;
      if (_chienRetirer) _chienRetirer();
    }
    _e.enCours = false;
#if defined(ESP8266)
    _surv.detach();      // F7 : plus rien à surveiller
    _survTire = false;
#endif
  }

  /** Redémarrage différé : à appeler à chaque tour de boucle. */
  void loop()
  {
    if (_e.redemarrageDu && !_e.enCours) {
      if (millis() - _e.debutMs > _delaiMs) {
        _e.redemarrageDu = false;
        ESP.restart();
      }
    }
  }

  // ── Lecture d'état ──────────────────────────────────────────────
  bool            enCours()  const { return _e.enCours; }
  bool            rebootDu() const { return _e.redemarrageDu; }
  const OtaEtat&  etat()     const { return _e; }

  /** VRAI quand l'application doit S'ABSTENIR de toute activité.
   *
   *  C'est LE test à mettre en tête de la boucle, avant tout envoi réseau
   *  (Domoticz, Telegram, MQTT), toute écriture flash (SMB, EEPROM, CSV) et
   *  toute mesure coûteuse.
   *
   *  ⚠️ POURQUOI C'EST INDISPENSABLE
   *     1. Une écriture flash entrecoupée par une requête HTTP synchrone
   *        (Domoticz, Telegram) fait ÉCHOUER l'OTA : le coeur bloque sur le
   *        réseau pendant que l'Updater attend le morceau suivant.
   *     2. Sur ESP8266 le tas est déjà serré : une String de réponse construite
   *        pendant l'upload provoque un abort OOM au pire moment.
   *     3. Le redémarrage est imminent : envoyer une mesure puis couper la
   *        carte laisse le récepteur avec une valeur orpheline.
   *
   *  Couvre les DEUX fenêtres : le transfert lui-même ET l'attente du
   *  redémarrage (1,5 s après la réponse HTTP).
   *
   *  Utilisation :
   *      loop() {
   *        server.handleClient();
   *        ota.loop();
   *        if (ota.suspendu()) return;   // rien d'autre pendant une OTA
   *        ... envois, mesures, persistance ...
   *      }
   */
  bool suspendu() const { return _e.enCours || _e.redemarrageDu; }

  // ── Réglages ────────────────────────────────────────────────────
  void delaiRedemarrage(uint32_t ms) { _delaiMs = ms; }
  void titre(const char* t)          { _titre = t; }

  // ── Crochets (remplacent onStart/onProgress/onEnd d'ElegantOTA) ──
  void surDebut(void (*f)())                    { _surDebut = f; }
  void surProgression(void (*f)(uint32_t))      { _surProg  = f; }
  void surFin(void (*f)(bool))                  { _surFin   = f; }

private:
  // ── Page de téléversement ───────────────────────────────────────
  // ⚠️ Envoyée en FLUX (chunked), sans tampon intermédiaire : la page n'est
  //    jamais construite en RAM. Sur ESP8266 c'est 900 o de pile économisés,
  //    et la taille de la page n'est plus bornée par un buffer.
  static void _page()
  {
    ESP_OTA* o = _self;
    if (!o || !o->_server) return;
    if (!o->_autorise()) { o->_refuser(); return; }

    o->_server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    o->_server->send(200, "text/html; charset=utf-8", "");
    o->_server->sendContent(F(
      "<!DOCTYPE html><html lang='fr'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>OTA</title><style>"
      "body{font-family:system-ui,Arial,sans-serif;margin:0;background:#f4f6f8;color:#222}"
      "header{background:#20324a;color:#fff;padding:12px 16px}"
      "header h1{margin:0;font-size:18px}"
      ".wrap{padding:16px;max-width:720px;margin:auto}"
      ".card{background:#fff;border-radius:8px;padding:16px;margin-bottom:16px;"
      "box-shadow:0 1px 3px rgba(0,0,0,.12)}"
      "input[type=submit]{background:#2c6ed5;color:#fff;border:0;padding:9px 16px;"
      "border-radius:6px;cursor:pointer}"
      "code{background:#eef;padding:1px 4px;border-radius:3px}"
      "</style></head><body><header><h1>"));
    o->_server->sendContent(o->_titre ? o->_titre : "Mise a jour");
    o->_server->sendContent(F("</h1></header><div class='wrap'><div class='card'>"
                              "<h3>Mise a jour du firmware</h3><form method='POST' action='"));
    o->_server->sendContent(o->_routeUpload);
    o->_server->sendContent(F("' enctype='multipart/form-data'>"
                              "<input type='file' name='firmware' accept='.bin'> "
                              "<input type='submit' value='Envoyer'></form>"));

    // ⚠️ 256, PAS 160 : les DEUX messages partagent ce tampon et le second en
    //    réclame 231 o (mesuré). À 160 la fin était coupée — la commande curl
    //    affichée s'arrêtait au milieu du nom de fichier et les balises
    //    `</div></div></body></html>` disparaissaient. Le navigateur rattrapant
    //    le HTML incomplet, le défaut restait invisible (relevé le 14/11/2026
    //    via -Wformat-truncation sur les 10 projets du parc).
    char b[256];
    snprintf(b, sizeof(b),
             "<p>Espace libre pour l'OTA : <b>%lu o</b></p>"
             "<p>Version installee : <code>%s</code></p>",
             (unsigned long)ESP.getFreeSketchSpace(), __ESP_OTA_H_VERSION__);
    o->_server->sendContent(b);

    snprintf(b, sizeof(b),
             "<p><b>Avertissement</b> : cette page n'est protegee que si "
             "<code>identifiants()</code> a ete appele.</p>"
             "<p>En ligne de commande :<br><code>curl -F firmware=@firmware.bin "
             "http://&lt;ip&gt;%s</code></p></div></div></body></html>",
             o->_routeUpload);
    o->_server->sendContent(b);
    o->_server->sendContent("");
  }

  // ── Réception de l'image (callback d'upload) ────────────────────
  static void _morceau()
  {
    ESP_OTA* o = _self;
    if (!o || !o->_server) return;

    // ⚠️ GARDE OBLIGATOIRE : un POST non multipart arrive par le chemin RAW,
    //    où _currentUpload n'existe pas. server.upload() déréférencerait un
    //    pointeur nul → panique. On sort AVANT tout appel.
    if (o->_gardeMultipart) {
      if (!o->_server->header("Content-Type").startsWith("multipart/")) return;
    }

    HTTPUpload& up = o->_server->upload();

    switch (up.status) {
      case UPLOAD_FILE_START: {
        o->_e.tentatives++;
        o->_e.enCours        = true;
        // Un nouvel envoi annule un redemarrage differe encore en attente :
        // sinon ota.loop() pourrait rebooter au milieu du nouveau transfert
        // (le test `!enCours` protege, mais autant ne pas compter dessus).
        o->_e.redemarrageDu  = false;
        o->_e.taille         = 0;
        o->_e.debutMs        = millis();
        o->_e.dernierOctetMs = millis();
        o->_e.dernier        = OTA_AUCUN;
        o->_e.message[0]     = '\0';

        // Authentification AVANT toute écriture en flash : sinon les ~1 Mo du
        // firmware seraient écrits avant le contrôle.
        if (!o->_autorise()) {
          o->_authKo = true;
          o->liberer();
          snprintf(o->_e.message, sizeof(o->_e.message), "refus (auth)");
          return;
        }
        o->_authKo = false;

        // Refus applicatif AVANT toute écriture en flash (anti-CSRF, quota…).
        o->_refusee = false;
        if (o->_avant && !o->_avant()) {
          o->_refusee = true;
          o->_e.echecs++;
          o->_e.dernier = OTA_ECHEC;
          snprintf(o->_e.message, sizeof(o->_e.message), "refus (crochet)");
          o->liberer();
          return;
        }

        // N1 : abonner le chien de garde AVANT le premier bloc. Sans cela, un
        // client qui envoie les en-têtes puis se tait fige handleClient() à
        // vie, sans que rien ne réagisse — et sans authentification.
        if (o->_chienAjouter && !o->_e.chienAbonne) {
          o->_e.chienAbonne = true;
          o->_chienAjouter();
        }
        if (o->_chienRearmer) o->_chienRearmer();

#if defined(ESP8266)
        // F7 : armer la surveillance du transfert muet. À partir d'ici, la
        // boucle de lecture peut bloquer sans timeout ; le minuteur est le
        // seul moyen de reprendre la main.
        if (o->_survActif && o->_delaiSansOctet > 0) {
          o->_survTire = false;
          o->_surv.attach_ms(500, _survTick);
        }
#endif

        // Taille annoncée : en multipart elle n'est pas connue ici.
        //   ESP32   : UPDATE_SIZE_UNKNOWN laisse le coeur choisir la partition
        //             OTA — comportement éprouvé en production.
        //   ESP8266 : la constante n'existe pas, on borne sous l'espace sketch.
        bool commence = false;
#if defined(ESP32)
        if (o->_tailleInconnue) {
          commence = Update.begin(UPDATE_SIZE_UNKNOWN);
        } else
#endif
        {
          uint32_t max = ESP.getFreeSketchSpace();
          max = (max > ESP_OTA_MARGE) ? ((max - ESP_OTA_MARGE) & 0xFFFFF000UL) : 0;
          if (max != 0) commence = Update.begin(max);
        }

        if (!commence) {
          o->_e.echecs++;
          o->_e.dernier = OTA_ECHEC;
          esp_otaMsgErreur(o->_e.message, sizeof(o->_e.message), "begin");
          Update.printError(Serial);
          o->liberer();
          return;
        }
        if (o->_surDebut) o->_surDebut();
        break;
      }

      case UPLOAD_FILE_WRITE: {
        if (!o->_e.enCours) break;              // begin() a échoué : on ignore
        if (o->_authKo) break;                  // non authentifié : on ignore
        size_t ecrit = Update.write(up.buf, up.currentSize);
        if (ecrit != up.currentSize) {
          o->_e.echecs++;
          o->_e.dernier = OTA_ECHEC;
          esp_otaMsgErreur(o->_e.message, sizeof(o->_e.message), "write");
          Update.printError(Serial);
          o->liberer();
          break;
        }
        o->_e.taille += (uint32_t)ecrit;
        // F1 : chaque morceau repousse la garde temporelle.
        o->_e.dernierOctetMs = millis();
        // N1 : réarmer le chien prouve que le transfert progresse. Sans cet
        // appel, un blocage dans _uploadReadByte (aucun timeout côté
        // framework) ferait paniquer le watchdog → reboot sur l'ancien
        // firmware (défaillance sûre, mais au moins détectée).
        if (o->_chienRearmer) o->_chienRearmer();
        if (o->_surProg) o->_surProg(o->_e.taille);
        break;
      }

      case UPLOAD_FILE_END: {
        if (!o->_e.enCours) break;
        if (o->_authKo) { o->liberer(); break; }

        // F6 : un fichier de 0 octet n'écrit rien ; Update.end() lirait un
        // tampon de saut jamais alloué et répondrait « Flash Read Failed » —
        // message trompeur pour un cas banal.
        if (up.totalSize == 0) {
          esp_otaAbandonner();
          o->_e.echecs++;
          o->_e.dernier = OTA_ECHEC;
          snprintf(o->_e.message, sizeof(o->_e.message), "fichier vide (0 octet)");
          o->liberer();
          if (o->_surFin) o->_surFin(false);
          break;
        }

        o->_e.dureeMs = millis() - o->_e.debutMs;
        // `true` : terminer même si le dernier paquet est incomplet.
        if (!Update.hasError() && Update.end(true) && o->_e.taille > 0) {
          o->_e.succes++;
          o->_e.dernier = OTA_OK;
          snprintf(o->_e.message, sizeof(o->_e.message), "%lu o ecrits",
                   (unsigned long)o->_e.taille);
        } else {
          o->_e.echecs++;
          o->_e.dernier = OTA_ECHEC;
          esp_otaMsgErreur(o->_e.message, sizeof(o->_e.message), "end");
          Update.printError(Serial);
        }
        o->liberer();
        if (o->_surFin) o->_surFin(o->_e.dernier == OTA_OK);
        break;
      }

      case UPLOAD_FILE_ABORTED:
      default: {
        // N2 : `Parsing.cpp` teste `!client.connected()` APRÈS avoir livré
        // UPLOAD_FILE_END. Un client qui ferme juste après le dernier octet
        // déclenche donc END (l'image est DÉJÀ marquée démarrable) PUIS
        // ABORTED. Sans ce garde-fou on appellerait Update.end() sur une
        // image valide et on annoncerait « interrompu » alors que la mise à
        // jour a réussi : l'utilisateur relancerait un transfert sur une
        // carte déjà à jour, et aucun redémarrage ne serait programmé.
        if (o->_e.dernier == OTA_OK) {
          o->_e.redemarrageDu = true;
          o->_e.debutMs       = millis();
          return;
        }
        if (o->_e.enCours) {
          o->_e.echecs++;
          o->_e.dernier = OTA_ABANDON;
          snprintf(o->_e.message, sizeof(o->_e.message), "transfert interrompu");
          Update.end();
          o->liberer();
          if (o->_surFin) o->_surFin(false);
        }
        break;
      }
    }
  }

  // ── Réponse finale ──────────────────────────────────────────────
  // ⚠️ Le code HTTP reflète l'état RÉEL. C'est tout l'intérêt du module :
  //    ElegantOTA répondait 200 même quand zéro octet avait été écrit.
  static void _final()
  {
    ESP_OTA* o = _self;
    if (!o || !o->_server) return;

    if (o->_refusee) {
      o->_refusee = false;
      o->_server->sendHeader("Connection", "close");
      o->_server->send(403, "text/plain; charset=utf-8",
                       "Envoi refuse par l'application (crochet avantUpload)");
      return;
    }
    if (o->_authKo) { o->_authKo = false; o->_refuser(); return; }
    if (!o->_autorise()) { o->_refuser(); return; }

    const bool ok = (o->_e.dernier == OTA_OK);

    o->_server->sendHeader("Connection", "close");
    if (ok) {
      o->_server->send(200, "text/plain; charset=utf-8",
                       "OK - redemarrage en cours");
      o->_e.redemarrageDu = true;
      o->_e.debutMs       = millis();
    } else {
      // Message court : le détail est dans `etat().message` et sur le port série.
      o->_server->send(500, "text/plain; charset=utf-8",
                       "ECHEC - rien n'a ete ecrit (voir le port serie)");
    }
  }

  // ── Helpers ─────────────────────────────────────────────────────
  // ⚠️ esp_otaMsgErreur() et esp_otaAbandonner() sont dans ESP_OTA.h (tronc) :
  //    l'adaptateur asynchrone en a besoin à l'identique.

  bool _autorise() const
  {
    if (!_user || !_pass) return true;
    return _server->authenticate(_user, _pass);
  }

  void _refuser() const
  {
    _server->requestAuthentication();
  }

  static ESP_OTA* _self;   // instance unique (les handlers sont statiques)

  Serveur*    _server        = nullptr;
  const char* _route         = ESP_OTA_ROUTE_DEFAUT;
  const char* _routeUpload   = ESP_OTA_ROUTE_DEFAUT;
  const char* _titre         = nullptr;
  const char* _user          = nullptr;
  const char* _pass          = nullptr;
  bool        _gardeMultipart= true;
  bool        _authKo        = false;
  bool        _refusee       = false;   // refus par le crochet avantUpload
  bool        _tailleInconnue= false;
  uint32_t    _delaiMs       = ESP_OTA_DELAI_REBOOT_MS;
  uint32_t    _delaiSansOctet= ESP_OTA_DELAI_MORT_MS;
  OtaEtat     _e;

  // ── F7 : surveillance du transfert muet (ESP8266) ──────────────
  // `_uploadReadByte()` du coeur n'a pas de timeout : un client silencieux
  // bloque `handleClient()` à vie, donc `loop()` ne peut rien faire. Le
  // minuteur ci-dessous tourne dans la tâche SDK et coupe la socket.
  bool        _survActif     = true;
#if defined(ESP8266)
  Ticker      _surv;
  bool        _survTire      = false;   // une seule coupure par transfert
  static void _survTick();
#endif

  void (*_surDebut)()             = nullptr;
  void (*_surProg)(uint32_t)      = nullptr;
  void (*_surFin)(bool)           = nullptr;
  bool (*_avant)()                = nullptr;   // crochet avantUpload
  void (*_chienAjouter)()         = nullptr;
  void (*_chienRearmer)()         = nullptr;
  void (*_chienRetirer)()         = nullptr;
};

template <typename Serveur>
ESP_OTA<Serveur>* ESP_OTA<Serveur>::_self = nullptr;

// ── F7 : minuteur de surveillance du transfert muet (ESP8266) ──────────────
// Exécuté dans la tâche SDK, PENDANT que la tâche principale est suspendue
// dans `_uploadReadByte()`. Couper la socket la débloque : la boucle du coeur
// teste `client.connected()` à chaque tour.
template <typename Serveur>
void ESP_OTA<Serveur>::_survTick()
{
#if defined(ESP8266)
  ESP_OTA* o = _self;
  if (!o || !o->_server) return;
  if (!o->_e.enCours) return;            // transfert terminé entre deux tics
  if (o->_survTire) return;              // une seule coupure par transfert
  if ((millis() - o->_e.dernierOctetMs) <= o->_delaiSansOctet) return;

  o->_survTire      = true;
  o->_e.echecs++;
  o->_e.dernier     = OTA_ABANDON;
  snprintf(o->_e.message, sizeof(o->_e.message), "transfert muet");

  // La boucle bloquée voit connected() faux et sort ; le handler reçoit
  // ensuite UPLOAD_FILE_ABORTED, qui libère l'image et l'état.
  o->_server->client().stop();
#endif
}

#endif // ESP_OTA_SYNC_H
