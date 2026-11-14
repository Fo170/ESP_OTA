# Changelog — ESP_OTA

Format : [SemVer](https://semver.org/lang/fr/). Chaque entrée dit **quoi**,
**pourquoi**, et **comment c'est vérifié**.

---

## [1.0.0] — 14/11/2026

Première version publiée. Remplace ElegantOTA sur les serveurs web synchrones
du parc.

### Ajouté

- `ESP_OTA<Serveur>::begin()` — enregistre `GET <route>` (page) et
  `POST <route>` (upload). Fonctionne avec `WebServer` (ESP32) et
  `ESP8266WebServer` (ESP8266) : même API, template.
- `Update.begin()` appelé à **`UPLOAD_FILE_START`** — un seul POST multipart
  suffit, plus de `/ota/start` préalable.
- **Code HTTP honnête** : `500` si rien n'a été écrit, `200` seulement si
  `Update.end()` a réussi **et** que `taille > 0`.
- **Garde anti-crash** : le `Content-Type` est testé avant `server.upload()`.
  Un POST non multipart partait par le chemin RAW où `_currentUpload` est nul →
  panique → reboot à distance sans authentification (reproduit le 25/10/2026).
- **Redémarrage différé** via `loop()` — la réponse HTTP part avant le reboot.
- **Authentification optionnelle** (`identifiants()`) testée à
  `UPLOAD_FILE_START`, donc **avant** toute écriture en flash.
- **Crochets** `surDebut` / `surProgression` / `surFin` — remplacent
  `onStart` / `onProgress` / `onEnd` d'ElegantOTA.
- **État exposé** (`OtaEtat`) : taille, durée, tentatives, succès, échecs,
  dernier résultat, message — pour la télémétrie et `/sante`.
- Page HTML construite dans un tampon fixe (`snprintf`), **aucune String**.

### Retiré par rapport à ElegantOTA

- Protocole en deux étapes.
- Faux `200 OK` quand zéro octet avait été écrit.
- `ESPAsyncWebServer` + `ESPAsyncTCP` dans l'arbre de dépendances.

### Mesuré

Sur `ESP8266-Thermometre` (ElegantOTA 3.1.7 → ESP_OTA 1.0.0) :

| | Avant | Après | Gain |
|---|---|---|---|
| Flash | 461 299 o | 451 895 o | **−9 404 o** |
| RAM statique | 49 496 o | 49 992 o | +496 o |
| Bibliothèques | ElegantOTA + ESPAsyncWebServer + ESPAsyncTCP | aucune | 3 de moins |

La RAM augmente de ~500 o : c'est le prix de `OtaEtat` (taille, durée,
tentatives, succès, échecs, message), des trois crochets et de la page en flux.
Une version minimale écrite à la main ne coûtait que ~160 o — mais sans état,
sans crochets, sans authentification et sans garde anti-crash.

La page est envoyée en **chunked** : elle n'est jamais construite en RAM. Une
première version bufferisait 900 o en pile ; la mesure a montré que ce tampon
était compté dans la RAM statique (−960 o après passage au flux).

### Vérifié

- `pio run` SUCCESS sur `esp12e` (ESP8266, via `ESP8266-Thermometre`) et
  `esp32dev` (ESP32, projet de test).
- `curl -F firmware=@firmware.bin http://<ip>/update` → une seule requête.
- POST non multipart → `401`/`500`, **plus de reboot**.

### Migration du parc — 8 projets, 10 binaires

ElegantOTA **et** les OTA écrites à la main remplacées par ESP_OTA :

| Projet | Flash avant | Flash après | Δ | RAM avant | RAM après | Δ |
|---|---|---|---|---|---|---|
| `WeMos Mini (Lolin) - LinkysTIC` | 539 915 | **485 275** | **−54 640** | 51 852 | 52 044 | +192 |
| `LinkysTIC` (noyau de secours) | 413 831 | **359 263** | **−54 568** | 35 108 | 35 308 | +200 |
| `Station_Meteo` | 456 747 | **447 579** | −9 168 | 46 144 | 46 656 | +512 |
| `Station_Meteo` (secours) | 365 643 | **356 227** | −9 416 | 34 536 | 35 064 | +528 |
| `ESP8266-Thermometre` | 461 299 | **452 175** | −9 124 | 49 496 | 50 000 | +504 |
| `PZEM004T` | 613 389 | **604 829** | −8 560 | 48 700 | 49 052 | +352 |
| `ESP8266_Surveillance_Batterie` | 475 951 | **467 223** | −8 728 | 43 348 | 43 652 | +304 |
| `ESP8266-Radiateur` | ≈ 380 900 | **378 735** | ≈ −2 165 | 39 368 | 39 788 | +420 |
| `ESP32_Ordonnanceur` | 1 180 793 | **1 180 221** | −572 | 101 416 | 101 504 | +88 |
| `WeMosMini_BME280_Garage` | 453 321 | 458 477 | **+5 156** | 43 484 | 44 168 | +684 |
| **Total** | | | **≈ −148 Ko** | | | **+3,7 Ko** |

**LinkysTIC gagne 54 Ko** parce qu'il utilisait ElegantOTA **2.2.9**, qui liait
tout `ESPAsyncWebServer`. Les autres, en 3.x/4.x, n'en liaient qu'une partie.

**BME280 Garage coûte 5 Ko de plus** : son `Net/Ota.h` (193 lignes) était plus
maigre que la librairie (550 lignes, authentification + gardes N1/F1/F6 + état).
C'est le prix de l'uniformité — et il gagne les gardes qu'il n'avait pas.

Le gain en RAM est **négatif** : la librairie est plus riche que l'ancien code.

### Gel de l'application pendant l'OTA — `suspendu()`

Ajouté en réponse à un besoin exprimé : *« quand un update OTA était enclenché,
ça mettait en pause toutes les autres fonctions »*. Chaque projet le faisait
à sa façon (ou pas du tout) :

| Projet | Mécanisme avant | Après |
|---|---|---|
| `LinkysTIC` | `Update.onStart/onEnd/onError` (coupe la TIC, libère les tampons, stoppe mDNS) | **conservé** — callbacks `Updater` indépendants d'ElegantOTA |
| `Station_Meteo` | `Update.isRunning()` | **conservé** |
| `ESP32_Ordonnanceur` | `ota_en_cours` → gel de la persistance SMB | `ota.enCours()` |
| `WeMosMini_BME280_Garage` | `otaEnCours() \|\| otaRedemarrageDu()` | `ota.suspendu()` |
| `ESP8266-Thermometre` | **rien** | `if (ota.suspendu()) return;` |
| `ESP8266-Radiateur` | **rien** | `if (ota.suspendu()) return;` |
| `ESP8266_Surveillance_Batterie` | **rien** | `if (ota.suspendu()) return;` |
| `PZEM004T` | **rien** | `if (ota.suspendu()) return;` |

`suspendu()` couvre les **deux** fenêtres : le transfert ET l'attente du
redémarrage (1,5 s après la réponse HTTP). Pourquoi c'est indispensable :

1. Une écriture flash entrecoupée par une requête HTTP synchrone (Domoticz,
   Telegram) fait **échouer** l'OTA : le cœur bloque sur le réseau pendant que
   l'Updater attend le morceau suivant.
2. Sur ESP8266 le tas est déjà serré : une String de réponse construite pendant
   l'upload provoque un abort OOM au pire moment.
3. Le redémarrage est imminent : envoyer une mesure puis couper la carte laisse
   le récepteur avec une valeur orpheline.

### Autres ajouts liés à la migration de l'Ordonnanceur

- `begin(..., routeUpload)` — URL de POST distincte de la page (l'Ordonnanceur
  garde `/update` pour la page et `/ota/upload` pour l'image, déjà documentés).
- **Garde N2** : `UPLOAD_FILE_END` suivi d'`UPLOAD_FILE_ABORTED` (le framework
  teste `!client.connected()` APRÈS avoir livré END). Sans elle on appelait
  `Update.end()` sur une image **déjà valide** et on annonçait « interrompu »
  alors que la mise à jour avait réussi.
