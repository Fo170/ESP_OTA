# AGENTS — ESP_OTA

Librairie header-only de mise à jour OTA par HTTP, pour serveur web
**synchrone** (`WebServer` / `ESP8266WebServer`) et **asynchrone**
(`AsyncWebServer`) ESP8266 / ESP32. Version 1.1.0.

---

## 0. Les trois fichiers — ne pas confondre

| Fichier | Rôle | En-tête de serveur inclus |
|---|---|---|
| `src/ESP_OTA.h` | **tronc commun** : version, réglages, `OtaEtat`, `esp_otaMsgErreur`, `esp_otaAbandonner` | **aucun** |
| `src/ESP_OTA_Sync.h` | adaptateur `WebServer` / `ESP8266WebServer` — classe `ESP_OTA<Serveur>` | oui |
| `src/ESP_OTA_Async.h` | adaptateur `AsyncWebServer` — classe `ESP_OTA_Async<Serveur>` | oui |

### I0 — Le tronc ne doit JAMAIS inclure un en-tête de serveur web

C'est la raison d'être de la découpe. Un projet synchrone n'a pas
`ESPAsyncWebServer` dans ses `lib_deps` ; un projet asynchrone ne doit pas tirer
`WebServer.h`. Toute inclusion de serveur dans `ESP_OTA.h` annule la découpe.

La compatibilité 1.x repose sur la garde `ESP_OTA_ASYNC_H`, définie par
`ESP_OTA_Async.h` **avant** qu'il n'inclue le tronc : c'est elle qui empêche le
tronc d'inclure `ESP_OTA_Sync.h`. Ne pas la déplacer.

Trois façons d'inclure, à ne pas confondre :

| Écriture | Ce qui est compilé | Usage |
|---|---|---|
| `#include <ESP_OTA.h>` | tronc **+** `ESP_OTA_Sync.h` | code 1.x, inchangé |
| `#include <ESP_OTA_Sync.h>` | tronc + adaptateur synchrone | forme explicite |
| `#include <ESP_OTA_Async.h>` | tronc seul + adaptateur asynchrone | asynchrone |
| `#define ESP_OTA_TRONC_SEUL` puis `#include <ESP_OTA.h>` | tronc seul | test / wrapper tiers |

---

## 1. Ce que fait la librairie

| Élément | Rôle |
|---|---|
| `src/ESP_OTA.h` | tronc commun (aucun serveur web) |
| `src/ESP_OTA_Sync.h` | page, upload, état, crochets — synchrone |
| `src/ESP_OTA_Async.h` | idem — asynchrone |
| `examples/Basique/` | exemple synchrone minimal |
| `examples/Basique_Async/` | exemple asynchrone minimal |

Synchrone : `ESP_OTA<WebServer>` (ESP32) ou `ESP_OTA<ESP8266WebServer>` (ESP8266).
Asynchrone : `ESP_OTA_Async<AsyncWebServer>`. **Même API dans les deux cas.**

---

## 2. Invariants — NE PAS RÉGRESSER

### I1 — `Update.begin()` à `UPLOAD_FILE_START`

C'est **la** raison d'être de la librairie. Le déplacer dans un handler
`GET /ota/start` ramènerait le protocole en deux étapes et le faux `200 OK`.

### I2 — Le code HTTP reflète l'état réel

`200` seulement si `Update.end()` a réussi **et** `taille > 0`. Sinon `500`.
ElegantOTA répondait `200` quand zéro octet avait été écrit : la carte reboutait
sur l'ancien firmware en annonçant un succès.

### I3 — Tester le `Content-Type` AVANT `server.upload()` (SYNCHRONE)

Un POST non multipart arrive par le chemin RAW de `_parseRequest`, où
`_currentUpload` n'existe pas. `server.upload()` déréférence un pointeur nul →
panique → reboot à distance **sans authentification**.

⚠️ Ce test exige que `Content-Type` soit dans la liste `collectHeaders()`.
`begin()` s'en charge quand `gardeMultipart` est vrai.

⚠️ **SANS OBJET EN ASYNCHRONE.** `WebRequest.cpp:172` : `handleUpload` n'est
appelé que si `_isMultipart`. Le chemin RAW et son pointeur nul n'existent pas.
La garde y subsiste comme filtre défensif, mais elle ne protège de rien : ne pas
croire qu'elle « sauve » une carte côté asynchrone.

### I4 — Authentifier AVANT d'écrire en flash

Le contrôle doit être à `UPLOAD_FILE_START`. Placé dans le handler final, il
arriverait **après** l'écriture des ~1 Mo de firmware.

### I5 — Redémarrage différé

Jamais `ESP.restart()` dans le handler final : la réponse HTTP serait coupée et
le navigateur afficherait une erreur alors que l'OTA a réussi. Le redémarrage
est piloté par `loop()` après `ESP_OTA_DELAI_REBOOT_MS`.

### I6 — Aucune allocation dynamique pour la page (SYNCHRONE)

Page envoyée en **chunked** (`setContentLength(CONTENT_LENGTH_UNKNOWN)` +
`sendContent`). Pas de tampon de la taille de la page : une première version
bufferisait 900 o en pile, et la mesure a montré que ce tampon était compté
dans la RAM statique (−960 o après passage au flux).

⚠️ **NON TENU EN ASYNCHRONE.** `sendContent()` n'existe pas. L'adaptateur
utilise `beginResponseStream()`, qui réserve `RESPONSE_STREAM_BUFFER_SIZE`
= 1460 o sur le tas, libérés après envoi. La RAM **statique** reste nulle, le
pic de tas augmente (mesuré, voir CHANGELOG). Ne pas remplacer par un
`static char[N]` : deux GET simultanés écraseraient le tampon de l'autre.

### I7 — Handlers en fonctions simples

Pas de lambda capturante dans `server.on()` : toutes les versions de
`WebServer` / `ESP8266WebServer` ne l'acceptent pas. On passe par un pointeur
statique `_self` (une seule instance par type de serveur).

### I8 — L'appelant doit tester `enCours()`

Tout `ESP.restart()` conditionné par un minuteur (redémarrage horaire, watchdog
d'inactivité) doit tester `ota.enCours()` d'abord, sinon il coupe l'écriture en
plein milieu.

### I9 — `suspendu()` testé dans la boucle de CHAQUE projet

Pendant un transfert (et jusqu'au redémarrage) l'application ne doit plus rien
faire : pas d'envoi Domoticz/Telegram/MQTT (synchrone → bloque le cœur au milieu
d'une écriture flash), pas de persistance, pas de mesure coûteuse.

```cpp
server.handleClient();
ota.loop();                    // AVANT le test : fait avancer le transfert
if (ota.suspendu()) return;
```

Un projet qui oublie ce test peut faire échouer ses propres OTA de façon
intermittente — le symptôme est un `end:` en échec sans cause apparente.

Les callbacks `Update.onStart/onEnd/onError` restent disponibles pour les
besoins spécifiques (couper un bus série, libérer des tampons). `LinkysTIC` s'en
sert pour arrêter la TIC et le mDNS.

### I10 — Chien de garde abonné pendant TOUT le transfert (garde N1)

`Parsing.cpp` (`_uploadReadByte`) fait
`while (!client.available() && client.connected()) delay(2);` **sans aucun
timeout**. Un client qui envoie les en-têtes puis se tait en laissant la socket
ouverte fige `handleClient()` **à vie** : le serveur web meurt, `enCours()`
reste armé, et aucun watchdog ne réagit si la tâche n'est pas abonnée. Panne
totale, non auto-réparatrice, **déclenchable sans authentification**.

`chienDeGarde(ajouter, rearmer, retirer)` : `ajouter` à `UPLOAD_FILE_START`,
`rearmer` à chaque morceau, `retirer` sur **tous** les chemins de sortie
(`liberer()`).

### I11 — Fichier vide traité explicitement (garde F6)

`up.totalSize == 0` → `_abandonner()` + message « fichier vide (0 octet) ».
Sans cela, `Update.end()` lit un tampon de saut jamais alloué et répond
« Flash Read Failed » — message trompeur pour un cas banal.

### I12 — `ABORTED` après `END` ne doit pas invalider une image valide (garde N2)

`Parsing.cpp` teste `!client.connected()` **APRÈS** avoir livré
`UPLOAD_FILE_END`. Un client qui ferme juste après le dernier octet déclenche
donc END (l'image est déjà marquée démarrable) **PUIS** ABORTED. Sans le test
`dernier == OTA_OK`, on appellerait `Update.end()` sur une image valide et on
annoncerait « interrompu » alors que la mise à jour a réussi : l'utilisateur
relance un transfert sur une carte déjà à jour, et aucun redémarrage n'est
programmé.

### I13 — Garde temporelle (F1)

`dernierOctetMs` est repoussé à chaque morceau ; `transfertMort()` expose le
silence. En LAN un client légitime envoie en continu (trou < 1 ms), le défaut de
30 s laisse une marge énorme.

---

## 2 bis. Invariants SPÉCIFIQUES à l'asynchrone

Chacun est tracé à une ligne d'`ESPAsyncWebServer` 3.7.3 — pas une supposition.

### I14 — `suspendu()` est OBLIGATOIRE (A5)

En synchrone, un oubli fait échouer l'OTA. En asynchrone, les callbacks
d'upload tournent dans la **tâche TCP** : sur ESP32 c'est le cœur 0, alors que
`loop()` est sur le cœur 1. Un `Update.write()` concurrent d'un
`SPIFFS.write()` / `LittleFS.write()` / écriture NVS **corrompt le système de
fichiers**. Le pire n'est plus une mise à jour ratée, c'est un disque illisible.

### I15 — Un fichier vide ne délivre JAMAIS `final` (A2)

`WebRequest.cpp:596` : `if (_itemSize) { handleUpload(..., true); }`. Un fichier
de 0 octet ne produit **aucun** appel : ni début, ni fin. `Update` n'est donc
jamais armé, et `_final()` répond 500.

⚠️ Si un transfert précédent avait laissé `Update` armé, `_final()` doit
l'abandonner : c'est le rôle de `_arme`, positionné après un `Update.begin()`
réussi et remis à `false` sur tous les chemins de sortie. Sans lui, l'envoi
suivant serait bloqué.

### I16 — `onDisconnect()` remplace `UPLOAD_FILE_ABORTED` (A3)

Il se déclenche **aussi après un transfert réussi** (fermeture normale de la
socket). La garde N2 (`dernier == OTA_OK` → ne rien invalider) est donc encore
plus indispensable qu'en synchrone.

### I17 — Forcer `AUTH_BASIC` (A7)

`ESPAsyncWebServer.h:317` : `requestAuthentication(realm, isDigest = true)`.
Le défaut est **Digest**, que `curl -F` ne négocie pas → 401 en boucle. Le
synchrone, lui, envoie du Basic. L'adaptateur force
`AsyncAuthType::AUTH_BASIC` pour que le parc continue de fonctionner.

`authenticate()` en revanche fonctionne tel quel : `_authMethod` vaut
`AUTH_NONE`, donc le chemin Basic est emprunté.

### I18 — Un seul upload à la fois, et c'est VÉRIFIÉ (A9)

Deux fichiers multipart simultanés écriraient dans le même `Update` → image
corrompue. Le premier arrivé gagne ; le second reçoit `503` et **ses morceaux
sont ignorés** via `_reqRefusee` / `_reqAuthKo` (pointeurs de requête, pas des
booléens : sinon la réponse d'une requête consommerait l'état d'une autre).

### I19 — Pas de `collectHeaders()` (A6)

Tous les en-têtes sont accessibles en permanence en asynchrone. L'appel
n'existe pas. **Ne pas en ajouter** après `begin()` : ce serait sans effet et
trompeur.

### I20 — A8 : ESP8266 compilé mais NON validé

`Update.begin()` / `Update.write()` sont appelés depuis la tâche TCP.
Accepté sur ESP32. Sur ESP8266, `UpdaterClass` n'a pas été conçu pour ce
contexte : **faire un transfert réel avant tout déploiement**, ne pas supposer.

### I21 — Les deux adaptateurs exposent la MÊME API

C'est ce qui rend la migration d'un projet à un changement d'en-tête et de type.
Toute méthode publique ajoutée d'un côté **doit** l'être de l'autre, avec la
même sémantique. Surface à maintenir en parité :

```
begin(server, route, titre, gardeMultipart, routeUpload)
identifiants(user, pass)        chienDeGarde(aj, rearm, ret)
delaiSansOctet(ms)              avantUpload(f)
tailleInconnue(bool)            liberer()
loop()                          enCours()   rebootDu()   etat()
suspendu()                      transfertMort()
delaiRedemarrage(ms)            titre(t)
surDebut(f)  surProgression(f)  surFin(f)
```

Seules différences admises, toutes justifiées par le transport :

| Point | Synchrone | Asynchrone |
|---|---|---|
| `gardeMultipart` | protège du crash distant (I3) | filtre défensif sans effet (A1) |
| `collectHeaders()` dans `begin()` | oui, `Content-Type` + `Origin` | non, sans objet (I19) |
| `ESP.restart()` dans le handler final | interdit (I5) | interdit (I5) |
| Rôle de `suspendu()` | conseillé | **obligatoire** (I14) |

Toute autre divergence est un défaut, pas une adaptation.


---

## 3. Modifier la librairie

1. Toute correction **doit** être justifiée par un constat mesuré (reproduction
   du défaut) — pas par une préférence de style.
2. Mettre à jour `__ESP_OTA_H_VERSION__` (dans `src/ESP_OTA.h`, le tronc),
   `library.json` et `library.properties` **ensemble** (les trois portent la
   version).
3. Ajouter une entrée dans `CHANGELOG.md` : quoi, pourquoi, comment vérifié.
4. Vérifier la compilation sur **les deux** plateformes, et sur **les trois**
   chemins d'inclusion :
   ```bash
   # synchrone        : #include <ESP_OTA_Sync.h>
   # rétro-compat 1.x : #include <ESP_OTA.h>   (tronc + Sync)
   # asynchrone       : #include <ESP_OTA_Async.h>
   pio run -e esp12e      # ESP8266
   pio run -e esp32dev    # ESP32
   ```
   ⚠️ Le projet de test asynchrone doit contenir, **après** ses `#include` :
   ```cpp
   #if defined(ESP32)
     #ifdef WEBSERVER_H
       #error "WebServer.h tire dans un projet asynchrone : decoupe cassee"
     #endif
   #endif
   ```
   C'est la seule preuve automatique que la découpe tient.
5. Vérifier le comportement réel :
   - `curl -F firmware=@x.bin http://<ip>/update` → une seule requête, `200`
   - POST non multipart → `401`/`500`, **pas de reboot**
   - fichier vide → `500`, pas de `200`
   - **asynchrone** : envoi immédiatement suivi d'un second envoi (le premier
     doit réussir, le second aussi — preuve que `Update` est bien désarmé, I15)
   - **asynchrone** : coupure du client en plein transfert, puis nouvel envoi
     qui doit réussir (preuve de I16)

---

## 4. Ce que la librairie ne fait PAS

- Pas de mise à jour « pull » (l'appareil ne va pas chercher son firmware) →
  voir `AutoUpdate_ESP`.
- Pas de vérification de signature d'image.
- Pas de file d'attente : un upload à la fois (en asynchrone, le second est
  refusé en `503`, voir I18).
- Pas de compatibilité `PsychicHttp`, `ESP8266WebServerSecure`, ni des serveurs
  TLS. Seuls `WebServer`, `ESP8266WebServer` et `AsyncWebServer` sont couverts.
- L'adaptateur asynchrone **ne déclare pas** `ESPAsyncWebServer` : c'est
  l'appelant qui l'ajoute à ses `lib_deps`. La librairie reste sans dépendance
  imposée.

---

## 5. Style

Français, commentaires explicatifs qui disent **pourquoi** (pas seulement quoi),
accentuation ASCII dans le code, tableaux dans les en-têtes pour les
avertissements. Voir `EspNow_UniVar` et `Guard_ESP` pour le ton.
`Guard_ESP_web.h` (adaptateurs `GE_WebGuard` / `GE_AsyncWebGuard` sur un cœur
commun) est le précédent maison de cette découpe : s'en inspirer.
