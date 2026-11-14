# ESP_OTA

Mise à jour du firmware par HTTP **en une seule étape**, pour les serveurs web
**synchrones** ESP8266 et ESP32. Remplace ElegantOTA.

- **Version** : 1.0.0
- **Plateformes** : `espressif8266`, `espressif32`
- **Header-only**, aucune allocation dynamique pour la page
- **Dépendances** : aucune (uniquement `Update.h` du cœur Arduino)

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

### 2. Crash distant sans authentification

Tout POST **non multipart** emprunte le chemin RAW de `_parseRequest`, où
`_currentUpload` n'existe pas. `server.upload()` déréférence alors un pointeur
nul → **panique** → la carte reboute. N'importe qui sur le LAN peut la rebooter
en boucle, sans mot de passe (reproduit le 25/10/2026).

**ESP_OTA** teste le `Content-Type` **avant** `server.upload()`.

### 3. Dépendances inutiles

ElegantOTA tire `ESPAsyncWebServer` (~98 Ko d'archive) et `ESPAsyncTCP`
(~16 Ko) alors que le parc utilise des serveurs **synchrones**. Mesuré sur
`ESP8266-Thermometre` : **−14 436 o de flash, −160 o de RAM** après retrait.

### 4. Redémarrage trop précoce

Redémarrer dans le handler final coupe la réponse : le navigateur affiche une
erreur alors que l'OTA a réussi. **ESP_OTA** diffère le redémarrage via
`loop()`.

---

## Utilisation

```cpp
#include <ESP_OTA.h>

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
}
```

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

## ⚠️ Points d'attention

### `collectHeaders`

Si `gardeMultipart` est vrai (défaut), `begin()` appelle
`server.collectHeaders({"Content-Type"}, 1)`. Ce doit être le **dernier**
`collectHeaders()` du projet : il remplace la liste collectée.
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

`suspendu()` couvre les **deux** fenêtres : le transfert et l'attente du
redémarrage.

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

## Structure

```
ESP_OTA/
├── src/ESP_OTA.h                    la librairie (header-only)
├── examples/Basique/Basique.ino     exemple minimal
├── library.json / library.properties
├── README.md / CHANGELOG.md / AGENTS.md
└── LICENSE                          GPL-3.0-only
```
