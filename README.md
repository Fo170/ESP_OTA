# ESP_OTA

Mise à jour du firmware par HTTP **en une seule étape**, pour les serveurs web
**synchrones** (`WebServer` / `ESP8266WebServer`) **et asynchrones**
(`AsyncWebServer`) ESP8266 et ESP32. Remplace ElegantOTA.

- **Version** : 1.1.0
- **Plateformes** : `espressif8266`, `espressif32`
- **Header-only**
- **Dépendances** : aucune pour le synchrone (uniquement `Update.h` du cœur
  Arduino). L'asynchrone exige `ESPAsyncWebServer`, mais **c'est l'appelant qui
  le déclare** — la librairie ne l'impose jamais.

---

## Architecture — trois fichiers

| Fichier | Rôle | En-tête de serveur inclus |
|---|---|---|
| `ESP_OTA.h` | **tronc commun** : version, réglages, `OtaEtat`, accès `Update` | **aucun** |
| `ESP_OTA_Sync.h` | adaptateur `WebServer` / `ESP8266WebServer` | oui |
| `ESP_OTA_Async.h` | adaptateur `AsyncWebServer` | oui (`ESPAsyncWebServer.h`) |

```cpp
#include <ESP_OTA_Sync.h>    // serveur synchrone
#include <ESP_OTA_Async.h>   // serveur asynchrone
```

Le tronc ne contenant **aucun** en-tête de serveur, un projet synchrone ne tire
pas `ESPAsyncWebServer` et un projet asynchrone ne tire pas `WebServer.h`.

### Compatibilité ascendante 1.x

`#include <ESP_OTA.h>` reste équivalent : le tronc inclut `ESP_OTA_Sync.h` à la
fin, sauf si `ESP_OTA_ASYNC_H` est défini (`ESP_OTA_Async.h` le fait) ou si
l'appelant définit `ESP_OTA_TRONC_SEUL`. **Aucun projet existant n'a besoin
d'être modifié.**

---

## Pourquoi

### 1. ElegantOTA impose un protocole en deux étapes

```
GET  /ota/start    → Update.begin()
POST /ota/upload   → Update.write() / Update.end()
```

Un POST direct — ce que fait `curl -F` — laisse `Update` **non initialisé**.
L'échec est alors **totalement silencieux** :

| Étape | Ce qui se passe |
|---|---|
| `Update.begin()` | jamais appelé → `_size == 0` |
| `Update.write()` | renvoie 0, le retour est ignoré par la librairie |
| `Update.end()` | sort sur `_size == 0` **sans** positionner `_error` |
| handler final | `hasError()` faux → **200 OK** + redémarrage |

La carte reboute sur l'**ancien** firmware en annonçant un succès. Constaté en
production le 27/10/2026 : « OK » en 2,1 s, puis version inchangée.

**ESP_OTA** appelle `Update.begin()` à `UPLOAD_FILE_START`. Un seul POST
multipart suffit, et le code HTTP reflète l'état réel.

### 2. Crash distant sans authentification (synchrone)

Tout POST **non multipart** emprunte le chemin RAW de `_parseRequest`, où
`_currentUpload` n'existe pas. `server.upload()` déréférence alors un pointeur
nul → **panique** → la carte reboute. N'importe qui sur le LAN peut la rebooter
en boucle, sans mot de passe (reproduit le 25/10/2026).

**ESP_OTA** teste le `Content-Type` **avant** `server.upload()`.
En asynchrone ce défaut **n'existe pas** : `handleUpload` n'est appelé que pour
un corps multipart. La garde y est conservée comme filtre défensif.

### 3. Dépendances inutiles

ElegantOTA tire `ESPAsyncWebServer` (~98 Ko d'archive) et `ESPAsyncTCP`
(~16 Ko) alors que le parc utilise des serveurs **synchrones**. Mesuré sur
`ESP8266-Thermometre` : **−14 436 o de flash, −160 o de RAM** après retrait.

### 4. Redémarrage trop précoce

Redémarrer dans le handler final coupe la réponse : le navigateur affiche une
erreur alors que l'OTA a réussi. **ESP_OTA** diffère le redémarrage via
`loop()`.

---

## Utilisation — serveur SYNCHRONE

```cpp
#include <ESP_OTA_Sync.h>

ESP8266WebServer server(80);      // ou WebServer sur ESP32
ESP_OTA<ESP8266WebServer> ota;

void setup() {
  // ... vos routes ...
  ota.identifiants("admin", "motdepasse");   // optionnel
  ota.begin(server);                         // /update (GET page, POST upload)
  server.begin();
}

void loop() {
  server.handleClient();
  ota.loop();                                // redémarrage différé
  if (ota.suspendu()) return;
}
```

## Utilisation — serveur ASYNCHRONE

```cpp
#include <ESP_OTA_Async.h>

AsyncWebServer server(80);
ESP_OTA_Async<AsyncWebServer> ota;

void setup() {
  // ... vos routes ...
  ota.identifiants("admin", "motdepasse");   // optionnel
  ota.begin(server);                         // /update (GET page, POST upload)
  server.begin();                            // ⚠️ l'AsyncWebServer démarre seul
}

void loop() {
  ota.loop();
  if (ota.suspendu()) return;                // ⚠️ OBLIGATOIRE ici, voir plus bas
}
```

L'API est **strictement la même** dans les deux cas. Migrer, c'est changer
l'en-tête et le type.

Côté client :

```bash
curl -F firmware=@.pio/build/esp12e/firmware.bin http://192.168.1.105/update
#  200 OK - redemarrage en cours
#  500 ECHEC - rien n'a ete ecrit        ← plus de faux succès
```

---

## API

### Configuration

| Méthode | Effet |
|---|---|
| `begin(server, route="/update", titre=nullptr, gardeMultipart=true)` | enregistre les routes |
| `identifiants(user, pass)` | protège page et upload par HTTP Basic |
| `delaiRedemarrage(ms)` | délai entre la réponse et le reboot (défaut 1500 ms) |
| `titre(texte)` | titre de la page |

### État

| Méthode | Effet |
|---|---|
| `suspendu()` | **l'application doit s'abstenir de toute activité** (transfert en cours OU redémarrage imminent) |
| `enCours()` | un upload est en cours — **à tester avant tout `ESP.restart()`** |
| `rebootDu()` | un redémarrage est programmé |
| `transfertMort()` | plus aucun octet depuis `delaiSansOctet()` |
| `etat()` | `OtaEtat` : taille, durée, tentatives, succès, échecs, dernier résultat, message |

### Crochets

| Méthode | Effet |
|---|---|
| `surDebut(f)` | `Update.begin()` a réussi |
| `surProgression(f)` | `f(octetsEcrits)` à chaque paquet |
| `surFin(f)` | `f(bool ok)` en fin de transfert |

---

## ⚠️ Points d'attention — synchrone

### `collectHeaders`

Si `gardeMultipart` est vrai (défaut), `begin()` appelle
`server.collectHeaders({"Content-Type","Origin"}, 2)`. Ce doit être le
**dernier** `collectHeaders()` du projet : il remplace la liste collectée.
`Authorization` reste en position 0, donc `authenticate()` continue de
fonctionner.

Passer `false` si le projet collecte d'autres en-têtes et veut garder sa liste —
mais la garde anti-crash est alors désactivée.

### Gel de l'application — `suspendu()`

Pendant une OTA, **rien d'autre ne doit tourner** : une écriture flash
entrecoupée par une requête HTTP synchrone (Domoticz, Telegram) fait échouer la
mise à jour, et sur ESP8266 le tas est déjà serré.

```cpp
void loop() {
  server.handleClient();
  ota.loop();                       // ⚠️ AVANT le test : fait avancer le transfert
  if (ota.suspendu()) return;       // rien d'autre pendant une OTA

  ... envois Domoticz, ESP-NOW, persistance, mesures ...
}
```

### Redémarrage horaire / watchdog

Un upload dure plusieurs secondes. Tout `ESP.restart()` conditionné par un
minuteur doit tester `ota.enCours()` d'abord, sinon il coupe l'écriture en
plein milieu.

```cpp
if (t_fct >= PERIODE_REBOOT_S && !ota.enCours()) ESP.restart();
```

### Guard_ESP

La route d'upload ne doit **pas** être filtrée par un garde de durée : un
upload dépasse largement `dureeMaxMs`, et le watchdog de concurrence libérerait
le slot pendant l'écriture.

---

## ⚠️ Points d'attention — asynchrone

### `suspendu()` passe de « conseillé » à **OBLIGATOIRE**

En synchrone, tout tourne dans `loop()` : un oubli de `suspendu()` fait échouer
l'OTA. En asynchrone, les callbacks d'upload tournent dans la **tâche TCP** —
sur ESP32 c'est le **cœur 0**, alors que `loop()` est sur le cœur 1. Un
`Update.write()` concurrent d'un `SPIFFS.write()` / `LittleFS.write()` /
écriture NVS **corrompt le système de fichiers**.

Le pire n'est plus une mise à jour ratée : c'est un disque illisible après
redémarrage.

### Pas de `collectHeaders()`

Tous les en-têtes sont accessibles en permanence. L'appel n'existe pas et n'est
pas nécessaire — un piège de moins que le synchrone. **Ne pas en ajouter.**

### L'authentification force `AUTH_BASIC`

`requestAuthentication()` est **Digest par défaut** dans `ESPAsyncWebServer`
(`isDigest = true`). `curl -F` ne négocie pas Digest : sans forçage, il
recevrait 401 en boucle. La librairie force donc `AsyncAuthType::AUTH_BASIC`
pour rester compatible avec le parc.

### Un seul upload à la fois, et c'est vérifié

Deux fichiers multipart simultanés écriraient dans le même `Update` → image
corrompue. Le premier arrivé gagne ; le second reçoit `503` et ses morceaux
sont ignorés.

### Page HTML : 1460 o de tas, pas 0

La page n'est pas envoyée en chunked mais via `beginResponseStream()`, qui
réserve `RESPONSE_STREAM_BUFFER_SIZE = 1460 o` sur le tas (libérés après
envoi). La **RAM statique** reste à zéro, le **pic de tas** augmente. La page
mesure ~980 o : elle tient dans la réserve, sans réallocation.

### ESP8266 : compile, mais à valider par la mesure

`Update.begin()` / `Update.write()` sont appelés depuis la tâche TCP
asynchrone. Accepté sur ESP32. Sur ESP8266, `UpdaterClass` n'a pas été conçu
pour ce contexte : **faire un transfert réel avant tout déploiement**.

---

## Structure

```
ESP_OTA/
├── src/ESP_OTA.h                    tronc commun (aucun serveur web)
├── src/ESP_OTA_Sync.h               adaptateur synchrone
├── src/ESP_OTA_Async.h              adaptateur asynchrone (opt-in)
├── examples/Basique/Basique.ino             exemple synchrone
├── examples/Basique_Async/Basique_Async.ino exemple asynchrone
├── library.json / library.properties
├── README.md / CHANGELOG.md / AGENTS.md
└── LICENSE                          GPL-3.0-only
```
