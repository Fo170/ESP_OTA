// ════════════════════════════════════════════════════════════════
// ESP_OTA_Async — mise à jour du firmware par HTTP, en UNE étape
// ESP8266 + ESP32 · header-only · serveur web ASYNCHRONE
//
// Adaptateur pour ESPAsyncWebServer (testé contre esp32async 3.7.3).
// Tronc commun : ESP_OTA.h — adaptateur synchrone : ESP_OTA_Sync.h.
//
// ┌──────────────────────────────────────────────────────────────┐
// │ CE FICHIER EST OPT-IN. Un projet SYNCHRONE ne doit PAS       │
// │ l'inclure : il tire ESPAsyncWebServer dans l'arbre de        │
// │ dépendances. Utiliser <ESP_OTA_Sync.h> dans ce cas.          │
// └──────────────────────────────────────────────────────────────┘
//
// ── CE QUI EST IDENTIQUE AU SYNCHRONE ───────────────────────────
//   Update.begin() à l'ouverture du fichier (I1), code HTTP honnête (I2),
//   authentification avant écriture flash (I4), redémarrage différé (I5),
//   crochets, chien de garde (N1), garde temporelle (F1), OtaEtat,
//   suspendu()/enCours()/rebootDu(). Mêmes noms, même sémantique.
//
// ── CE QUI DIFFÈRE — 9 points, tous vérifiés dans le source de
//    ESPAsyncWebServer 3.7.3 (aucune supposition) ────────────────
//
//  A1 · GARDE ANTI-CRASH (I3) DEVIENT SANS OBJET
//       `WebRequest.cpp` : `handleUpload` n'est appelé QUE si `_isMultipart`.
//       Un POST non multipart emprunte `handleBody`, jamais le chemin RAW :
//       le pointeur nul de `server.upload()` n'existe pas ici. La garde est
//       conservée comme filtre défensif (au cas où le framework changerait),
//       mais le risque de reboot distant disparaît par construction.
//
//  A2 · FICHIER DE 0 OCTET : `final` N'EST JAMAIS DÉLIVRÉ
//       `WebRequest.cpp` : `if (_itemSize) { handleUpload(..., true); }`.
//       Un fichier vide ne produit donc AUCUN appel — ni début, ni fin.
//       Conséquence : Update n'est jamais armé (donc rien à abandonner), et
//       `_final()` répond 500. Si un transfert précédent avait laissé Update
//       armé, `_final()` l'abandonne (`_arme`). C'est l'équivalent de F6.
//
//  A3 · PAS DE `UPLOAD_FILE_ABORTED` → `onDisconnect()`
//       `request->onDisconnect()` est le seul signal de coupure. Il se
//       déclenche AUSSI après un transfert réussi (fermeture normale de la
//       socket) : la garde N2 (ne pas invalider une image déjà valide) est
//       donc INDISPENSABLE ici, plus encore qu'en synchrone.
//
//  A4 · I6 (ZÉRO ALLOCATION) NE PEUT PAS ÊTRE TENU
//       Pas de `sendContent()` chunked. `beginResponseStream()` alloue un
//       tampon de RESPONSE_STREAM_BUFFER_SIZE = 1460 o sur le tas, libéré
//       par le framework. La RAM STATIQUE reste à 0 (aucun tampon de page),
//       mais le pic de tas augmente. Mesuré et chiffré dans le CHANGELOG :
//       la mesure remplace l'invariant, elle ne le simule pas.
//
//  A5 · CONCURRENCE RÉELLE — `suspendu()` DEVIENT OBLIGATOIRE
//       Les callbacks d'upload tournent dans la tâche TCP asynchrone, pas
//       dans `loop()`. Sur ESP32 c'est le cœur 0 alors que `loop()` est sur
//       le cœur 1. Un `Update.write()` concurrent d'un `SPIFFS.write()`
//       corrompt le système de fichiers. En synchrone, oublier `suspendu()`
//       faisait échouer l'OTA ; ici, cela corrompt le disque. Ce n'est plus
//       un conseil.
//
//  A6 · PAS DE `collectHeaders()`
//       Tous les en-têtes sont accessibles en permanence. L'appel n'existe
//       pas et n'est pas nécessaire : un piège de moins que le synchrone.
//
//  A7 · `requestAuthentication()` EST DIGEST PAR DÉFAUT
//       `ESPAsyncWebServer.h` : `requestAuthentication(realm, isDigest = true)`.
//       Le synchrone envoie du BASIC. Sans forçage, `curl -F` — qui ne fait
//       pas de Digest — recevrait 401 en boucle. On force
//       `AsyncAuthType::AUTH_BASIC` pour rester compatible avec le parc.
//       `authenticate()` en revanche fonctionne tel quel : `_authMethod`
//       vaut AUTH_NONE, donc le chemin Basic est emprunté.
//
//  A8 · `Update.begin()` DANS LA TÂCHE TCP
//       Accepté sur ESP32. Sur ESP8266, `UpdaterClass` n'a pas été conçu
//       pour ce contexte : à valider par mesure avant tout déploiement,
//       ne pas supposer.
//
//  A9 · UN SEUL UPLOAD À LA FOIS, ET IL FAUT LE FAIRE RESPECTER
//       Deux fichiers multipart simultanés écriraient tous les deux dans le
//       même `Update` → image corrompue. Le premier arrivé gagne ; le second
//       est refusé (503) et ses morceaux sont ignorés grâce au pointeur
//       `_reqConflit`. Le synchrone n'avait pas ce problème (un seul client
//       traité à la fois).
//
// Auteur : Olivier FOURNET · Licence : GPL-3.0-only
// ════════════════════════════════════════════════════════════════
#ifndef ESP_OTA_ASYNC_H
#define ESP_OTA_ASYNC_H

// ⚠️ Tronc commun (version, réglages, OtaEtat, accès Update).
//    ⚠️ La garde ESP_OTA_ASYNC_H définie juste au-dessus empêche le tronc
//       d'inclure ESP_OTA_Sync.h : c'est ce qui garantit qu'un projet
//       asynchrone ne tire PAS WebServer.h / ESP8266WebServer.h.
#include "ESP_OTA.h"

// ⚠️ ESPAsyncWebServer 3.x supporte ESP8266 ET ESP32 (library.json du dépôt
//    esp32async/ESPAsyncWebServer : platforms espressif32, espressif8266).
//    Sur ESP8266 il tire ESPAsyncTCP, sur ESP32 il tire AsyncTCP.
#include <ESPAsyncWebServer.h>

/**
 * Gestionnaire OTA pour un serveur web asynchrone.
 *
 * Utilisation minimale :
 *     ESP_OTA_Async<AsyncWebServer> ota;
 *     setup() : ota.begin(server);   // ⚠️ PAS de server.begin() ici, il est
 *                                    //    déjà fait par l'AsyncWebServer
 *     loop()  : ota.loop();          // AVANT le test suspendu()
 *               if (ota.suspendu()) return;
 *
 * ⚠️ L'API est VOLONTAIREMENT la même que ESP_OTA<Serveur>. Seuls les
 *    arguments de `begin()` qui n'ont pas de sens en asynchrone
 *    (`gardeMultipart`) sont conservés pour la symétrie et documentés.
 */
template <typename ServeurAsync>
class ESP_OTA_Async {
public:
  ESP_OTA_Async() { _self = this; }

  /** Enregistre les routes (GET page, POST upload).
   *
   *  @param server          l'AsyncWebServer, déjà instancié
   *  @param route           URL de la page et de l'upload (défaut `/update`)
   *  @param titre           nom affiché sur la page (défaut : « Mise a jour »)
   *  @param gardeMultipart  filtre défensif sur `Content-Type`. En asynchrone
   *                         le crash distant est impossible par construction
   *                         (A1) : ce drapeau ne protège de rien, il reste
   *                         pour la symétrie d'API. `false` ne dégrade donc
   *                         PAS la sécurité ici.
   *  @param routeUpload     URL du POST si elle diffère de la page.
   *                         nullptr = même URL que `route`.
   *  @return false si les routes n'ont pas pu être enregistrées
   *
   *  ⚠️ Aucun `collectHeaders()` n'est nécessaire (A6) : ne pas en ajouter
   *     après cet appel, ce serait sans effet et trompeur.
   */
  bool begin(ServeurAsync& server, const char* route = ESP_OTA_ROUTE_DEFAUT,
             const char* titre = nullptr, bool gardeMultipart = true,
             const char* routeUpload = nullptr)
  {
    if (!route || route[0] != '/') return false;
    if (routeUpload && routeUpload[0] != '/') return false;
    _server         = &server;
    _route          = route;
    _routeUpload    = routeUpload ? routeUpload : route;
    _titre          = (titre && titre[0]) ? titre : nullptr;
    _gardeMultipart = gardeMultipart;

    // ⚠️ Fonctions simples, pas de lambda : `ArRequestHandlerFunction` et
    //    `ArUploadHandlerFunction` sont des std::function, et une fonction
    //    libre s'y convertit directement. I7 est donc respecté à la lettre,
    //    pas contourné.
    _server->on(_route, HTTP_GET, _page);
    _server->on(_routeUpload, HTTP_POST, _final, _morceau);
    return true;
  }

  /** Protège la page et l'upload par HTTP Basic. À appeler avant begin().
   *  ⚠️ A7 : le refus force explicitement AUTH_BASIC — le défaut
   *     d'ESPAsyncWebServer est DIGEST et casserait `curl -F`. */
  void identifiants(const char* utilisateur, const char* motDePasse)
  {
    _user = utilisateur;
    _pass = motDePasse;
  }

  /** Abonne un chien de garde pendant TOUT le transfert (garde N1).
   *
   *  ⚠️ MOINS CRITIQUE QU'EN SYNCHRONE, MAIS CONSERVÉ
   *     `_uploadReadByte()` et sa boucle sans timeout n'existent pas ici :
   *     le framework livre les octets au fil de l'eau et gère ses propres
   *     délais. Le chien de garde reste utile pour la reprise après blocage
   *     applicatif pendant l'écriture flash.
   *
   *  `ajouter` au premier morceau, `rearmer` à chaque morceau, `retirer` sur
   *  TOUS les chemins de sortie (`liberer()`). nullptr pour désactiver.
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

  /** Vrai si le transfert en cours n'a plus donné signe de vie depuis
   *  `delaiSansOctet()`. À tester depuis la boucle. */
  bool transfertMort() const
  {
    return _e.enCours && _delaiSansOctet > 0
        && (millis() - _e.dernierOctetMs) > _delaiSansOctet;
  }

  /** Crochet de REFUS, appelé **avant** `Update.begin()`, donc avant le
   *  moindre octet écrit en flash. `false` annule l'envoi : le handler final
   *  répond 403. Même contrat que la version synchrone. */
  void avantUpload(bool (*f)()) { _avant = f; }

  /** Taille annoncée à `Update.begin()`.
   *  `true` = UPDATE_SIZE_UNKNOWN (ESP32 uniquement).
   *  `false` (défaut) = espace sketch libre moins la marge. */
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
   *  ⚠️⚠️ EN ASYNCHRONE CET APPEL EST **OBLIGATOIRE**, PAS CONSEILLÉ (A5).
   *       Les callbacks d'upload tournent dans la tâche TCP : sur ESP32 le
   *       cœur 0, alors que `loop()` tourne sur le cœur 1. Un `Update.write()`
   *       concurrent d'un `SPIFFS.write()` / `LittleFS.write()` / écriture
   *       NVS corrompt le système de fichiers. En synchrone le pire était
   *       une OTA ratée ; ici le pire est un disque illisible après reboot.
   *
   *  Couvre les DEUX fenêtres : le transfert ET l'attente du redémarrage.
   *
   *  Utilisation :
   *      loop() {
   *        ota.loop();                   // AVANT le test
   *        if (ota.suspendu()) return;   // rien d'autre pendant une OTA
   *        ... envois, mesures, persistance ...
   *      }
   */
  bool suspendu() const { return _e.enCours || _e.redemarrageDu; }

  // ── Réglages ────────────────────────────────────────────────────
  void delaiRedemarrage(uint32_t ms) { _delaiMs = ms; }
  void titre(const char* t)          { _titre = t; }

  // ── Crochets ────────────────────────────────────────────────────
  void surDebut(void (*f)())                    { _surDebut = f; }
  void surProgression(void (*f)(uint32_t))      { _surProg  = f; }
  void surFin(void (*f)(bool))                  { _surFin   = f; }

private:
  // ── Page de téléversement ───────────────────────────────────────
  // A4 : pas de `sendContent()` chunked côté asynchrone. On utilise
  // `beginResponseStream()`, qui alloue UN tampon de 1460 o sur le tas
  // (RESPONSE_STREAM_BUFFER_SIZE), libéré par le framework après envoi.
  // La RAM STATIQUE reste à 0 : aucun tampon de la taille de la page.
  // ⚠️ Ne PAS remplacer par un `static char[N]` : deux GET simultanés
  //    écraseraient le tampon de l'autre.
  static void _page(AsyncWebServerRequest* req)
  {
    ESP_OTA_Async* o = _self;
    if (!o || !o->_server) return;
    if (!o->_autorise(req)) { o->_refuser(req); return; }

    AsyncResponseStream* r = req->beginResponseStream("text/html; charset=utf-8");
    if (!r) {
      // Tas insuffisant : refus explicite plutôt qu'une page tronquée.
      req->send(503, "text/plain; charset=utf-8", "Memoire insuffisante");
      return;
    }

    r->print(F(
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
    r->print(o->_titre ? o->_titre : "Mise a jour");
    r->print(F("</h1></header><div class='wrap'><div class='card'>"
               "<h3>Mise a jour du firmware</h3><form method='POST' action='"));
    r->print(o->_routeUpload);
    r->print(F("' enctype='multipart/form-data'>"
               "<input type='file' name='firmware' accept='.bin'> "
               "<input type='submit' value='Envoyer'></form>"));

    // ⚠️ 256 o, comme en synchrone : le second message en réclame 231.
    //    À 160 la fin était coupée (constat du 14/11/2026).
    char b[256];
    snprintf(b, sizeof(b),
             "<p>Espace libre pour l'OTA : <b>%lu o</b></p>"
             "<p>Version installee : <code>%s</code></p>",
             (unsigned long)ESP.getFreeSketchSpace(), __ESP_OTA_H_VERSION__);
    r->print(b);

    snprintf(b, sizeof(b),
             "<p><b>Avertissement</b> : cette page n'est protegee que si "
             "<code>identifiants()</code> a ete appele.</p>"
             "<p>En ligne de commande :<br><code>curl -F firmware=@firmware.bin "
             "http://&lt;ip&gt;%s</code></p></div></div></body></html>",
             o->_routeUpload);
    r->print(b);

    req->send(r);
  }

  // ── Réception de l'image ────────────────────────────────────────
  //
  // ⚠️ SÉQUENCE RÉELLE DU FRAMEWORK — à ne pas réinventer
  //    `WebRequest.cpp` livre les morceaux ainsi :
  //      · `_handleUploadByte()` : `final` est TOUJOURS `false` (littéral).
  //        Il est appelé à chaque fin de tampon réseau OU tous les 1460 o.
  //      · `handleUpload(..., true)` : UNE seule fois, à la frontière
  //        multipart finale, et SEULEMENT si `_itemSize > 0`.
  //    Donc :
  //      - un petit fichier (< 1460 o) arrive en UN SEUL appel :
  //        index = 0, len = taille, final = true  → début + écriture + fin
  //        dans le même appel. `index == 0` reste fiable comme signal de
  //        début : `index` vaut `_itemSize - _itemBufferIndex`, qui ne peut
  //        être 0 qu'au premier vidage.
  //      - un fichier vide ne produit AUCUN appel (voir A2).
  static void _morceau(AsyncWebServerRequest* req, const String& nomFichier,
                       size_t index, uint8_t* data, size_t len, bool final)
  {
    ESP_OTA_Async* o = _self;
    if (!o || !o->_server) return;
    (void)nomFichier;   // le nom n'est pas utilisé : l'image vient de `data`

    // A9 : requête déjà refusée (conflit d'upload ou crochet) → on ignore
    //      tous ses morceaux, sinon elle écrirait dans l'Update d'un autre.
    if (req == o->_reqRefusee || req == o->_reqAuthKo) return;

    // A1 : filtre défensif. En asynchrone `handleUpload` n'est appelé que
    //      pour un corps multipart, donc ce test ne peut pas « sauver » une
    //      carte — il documente l'invariant et protège d'un changement futur.
    if (o->_gardeMultipart && !o->_estMultipart(req)) return;

    // ── DÉBUT : index == 0 ───────────────────────────────────────
    if (index == 0) {
      // A9 : un autre transfert est déjà en cours → conflit. On refuse
      //      CELUI-CI et on n'y touche plus. Le premier garde ses données.
      if (o->_e.enCours) {
        o->_reqRefusee = req;
        o->_codeRefus = 503;
        o->_e.echecs++;
        o->_e.dernier = OTA_ECHEC;
        snprintf(o->_e.message, sizeof(o->_e.message), "refus (transfert en cours)");
        return;
      }

      o->_e.tentatives++;
      o->_e.enCours        = true;
      o->_e.redemarrageDu  = false;
      o->_e.taille         = 0;
      o->_e.debutMs        = millis();
      o->_e.dernierOctetMs = millis();
      o->_e.dernier        = OTA_AUCUN;
      o->_e.message[0]     = '\0';

      // I4 : authentification AVANT toute écriture en flash.
      if (!o->_autorise(req)) {
        o->_reqAuthKo = req;
        o->liberer();
        snprintf(o->_e.message, sizeof(o->_e.message), "refus (auth)");
        return;
      }

      // Crochet applicatif AVANT toute écriture en flash (anti-CSRF, quota…).
      if (o->_avant && !o->_avant()) {
        o->_reqRefusee = req;
        o->_codeRefus = 403;
        o->_e.echecs++;
        o->_e.dernier = OTA_ECHEC;
        snprintf(o->_e.message, sizeof(o->_e.message), "refus (crochet)");
        o->liberer();
        return;
      }

      // N1 : abonner le chien de garde AVANT le premier bloc.
      if (o->_chienAjouter && !o->_e.chienAbonne) {
        o->_e.chienAbonne = true;
        o->_chienAjouter();
      }
      if (o->_chienRearmer) o->_chienRearmer();

      // A3 : la coupure n'a pas d'autre signal que onDisconnect(). On
      //      l'arme ici, une fois, pour cette requête.
      req->onDisconnect(_deconnexion);

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
      // A2 : mémorisé pour pouvoir abandonner si `_final()` n'a jamais de
      //      signal de fin (fichier vide, coupure silencieuse).
      o->_arme = true;
      if (o->_surDebut) o->_surDebut();
    }

    // ── ÉCRITURE ─────────────────────────────────────────────────
    // Rien à écrire si le début a échoué ou si l'auth a refusé.
    if (!o->_e.enCours || o->_reqAuthKo == req) return;

    if (len > 0) {
      size_t ecrit = Update.write(data, len);
      if (ecrit != len) {
        o->_e.echecs++;
        o->_e.dernier = OTA_ECHEC;
        esp_otaMsgErreur(o->_e.message, sizeof(o->_e.message), "write");
        Update.printError(Serial);
        o->_arme = false;
        o->liberer();
        return;
      }
      o->_e.taille += (uint32_t)ecrit;
      // F1 : chaque morceau repousse la garde temporelle.
      o->_e.dernierOctetMs = millis();
      if (o->_chienRearmer) o->_chienRearmer();
      if (o->_surProg) o->_surProg(o->_e.taille);
    }

    // ── FIN : `final` ────────────────────────────────────────────
    if (!final) return;

    const size_t total = index + len;

    // F6 / A2 : un fichier de 0 octet n'arrive normalement jamais ici
    // (`if (_itemSize)` côté framework). Ce test couvre le cas où le
    // framework changerait, et évite Update.end() sur un tampon jamais
    // alloué — message trompeur « Flash Read Failed ».
    if (total == 0) {
      esp_otaAbandonner();
      o->_arme = false;
      o->_e.echecs++;
      o->_e.dernier = OTA_ECHEC;
      snprintf(o->_e.message, sizeof(o->_e.message), "fichier vide (0 octet)");
      o->liberer();
      if (o->_surFin) o->_surFin(false);
      return;
    }

    o->_e.dureeMs = millis() - o->_e.debutMs;
    // `true` : terminer même si le dernier paquet est incomplet.
    if (!Update.hasError() && Update.end(true) && o->_e.taille > 0) {
      o->_arme = false;
      o->_e.succes++;
      o->_e.dernier = OTA_OK;
      snprintf(o->_e.message, sizeof(o->_e.message), "%lu o ecrits",
               (unsigned long)o->_e.taille);
    } else {
      o->_arme = false;
      o->_e.echecs++;
      o->_e.dernier = OTA_ECHEC;
      esp_otaMsgErreur(o->_e.message, sizeof(o->_e.message), "end");
      Update.printError(Serial);
    }
    o->liberer();
    if (o->_surFin) o->_surFin(o->_e.dernier == OTA_OK);
  }

  // ── Coupure de la connexion (A3) ────────────────────────────────
  // ⚠️ Appelé AUSSI après un transfert réussi : le client ferme la socket
  //    normalement. La garde N2 (ne pas invalider une image déjà valide)
  //    est donc OBLIGATOIRE ici — même constat qu'en synchrone, où
  //    `Parsing.cpp` livrait ABORTED après END.
  static void _deconnexion()
  {
    ESP_OTA_Async* o = _self;
    if (!o) return;

    if (o->_e.dernier == OTA_OK) {
      // Image déjà marquée démarrable : on ne fait que programmer le reboot.
      if (!o->_e.redemarrageDu) {
        o->_e.redemarrageDu = true;
        o->_e.debutMs       = millis();
      }
      return;
    }
    if (o->_e.enCours) {
      o->_e.echecs++;
      o->_e.dernier = OTA_ABANDON;
      snprintf(o->_e.message, sizeof(o->_e.message), "transfert interrompu");
      o->_arme = false;
      esp_otaAbandonner();
      o->liberer();
      if (o->_surFin) o->_surFin(false);
    }
  }

  // ── Réponse finale ──────────────────────────────────────────────
  static void _final(AsyncWebServerRequest* req)
  {
    ESP_OTA_Async* o = _self;
    if (!o || !o->_server) return;

    // A9 : requête écartée (conflit d'upload ou crochet) → sa réponse.
    if (req == o->_reqRefusee) {
      const uint16_t code = o->_codeRefus;
      o->_reqRefusee = nullptr;
      req->send(code, "text/plain; charset=utf-8",
                code == 503 ? "Un transfert est deja en cours"
                            : "Envoi refuse par l'application (crochet avantUpload)");
      return;
    }
    if (req == o->_reqAuthKo) { o->_reqAuthKo = nullptr; o->_refuser(req); return; }
    if (!o->_autorise(req)) { o->_refuser(req); return; }

    const bool ok = (o->_e.dernier == OTA_OK);

    // A2 : aucun signal de fin n'a été délivré (fichier vide) mais Update
    //      était armé → abandonner, sinon l'envoi suivant serait bloqué.
    if (!ok && o->_arme) { esp_otaAbandonner(); o->_arme = false; }

    if (ok) {
      req->send(200, "text/plain; charset=utf-8", "OK - redemarrage en cours");
      o->_e.redemarrageDu = true;
      o->_e.debutMs       = millis();
    } else {
      // Message court : le détail est dans `etat().message` et sur le port série.
      req->send(500, "text/plain; charset=utf-8",
                "ECHEC - rien n'a ete ecrit (voir le port serie)");
    }
  }

  // ── Helpers ─────────────────────────────────────────────────────
  bool _estMultipart(AsyncWebServerRequest* req) const
  {
    if (!req->hasHeader("Content-Type")) return false;
    const AsyncWebHeader* h = req->getHeader("Content-Type");
    return h && h->value().startsWith("multipart/");
  }

  bool _autorise(AsyncWebServerRequest* req) const
  {
    if (!_user || !_pass) return true;
    // `_authMethod` vaut AUTH_NONE par défaut → chemin Basic emprunté (A7).
    return req->authenticate(_user, _pass);
  }

  void _refuser(AsyncWebServerRequest* req) const
  {
    // A7 : forcer BASIC. Le défaut d'ESPAsyncWebServer est DIGEST, que
    //      `curl -F` ne sait pas négocier : le parc recevrait 401 en boucle.
    req->requestAuthentication(AsyncAuthType::AUTH_BASIC);
  }

  static ESP_OTA_Async* _self;   // instance unique (handlers statiques)

  ServeurAsync* _server        = nullptr;
  const char*   _route         = ESP_OTA_ROUTE_DEFAUT;
  const char*   _routeUpload   = ESP_OTA_ROUTE_DEFAUT;
  const char*   _titre         = nullptr;
  const char*   _user          = nullptr;
  const char*   _pass          = nullptr;
  bool          _gardeMultipart= true;
  bool          _tailleInconnue= false;
  bool          _arme          = false;   // Update.begin() a réussi (A2)
  uint32_t      _delaiMs       = ESP_OTA_DELAI_REBOOT_MS;
  uint32_t      _delaiSansOctet= ESP_OTA_DELAI_MORT_MS;
  OtaEtat       _e;

  // A9 : requêtes écartées. Un seul upload à la fois, et chaque refus ne
  //      concerne QUE la requête qui l'a provoqué.
  AsyncWebServerRequest* _reqRefusee = nullptr;
  AsyncWebServerRequest* _reqAuthKo  = nullptr;
  uint16_t               _codeRefus  = 403;

  void (*_surDebut)()             = nullptr;
  void (*_surProg)(uint32_t)      = nullptr;
  void (*_surFin)(bool)           = nullptr;
  bool (*_avant)()                = nullptr;
  void (*_chienAjouter)()         = nullptr;
  void (*_chienRearmer)()         = nullptr;
  void (*_chienRetirer)()         = nullptr;
};

template <typename ServeurAsync>
ESP_OTA_Async<ServeurAsync>* ESP_OTA_Async<ServeurAsync>::_self = nullptr;

#endif // ESP_OTA_ASYNC_H
