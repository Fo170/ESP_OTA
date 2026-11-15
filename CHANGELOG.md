# Changelog — ESP_OTA

Format : [SemVer](https://semver.org/lang/fr/). Chaque entrée dit **quoi**,
**pourquoi**, et **comment c'est vérifié**.

---

## [1.1.0] — 15/11/2026

### Ajouté

- **Compatibilité `AsyncWebServer`.** La librairie ne fonctionnait qu'avec un
  serveur synchrone. Elle couvre désormais les deux, sans que le chemin
  synchrone change d'une ligne.
- **Découpe en trois fichiers**, une responsabilité chacun :

  | Fichier | Rôle | En-tête de serveur inclus |
  |---|---|---|
  | `ESP_OTA.h` | **tronc commun** : version, réglages, `OtaEtat`, accès `Update` | **aucun** |
  | `ESP_OTA_Sync.h` | adaptateur `WebServer` / `ESP8266WebServer` | oui |
  | `ESP_OTA_Async.h` | adaptateur `AsyncWebServer` | oui (`ESPAsyncWebServer.h`) |

  Le tronc ne contenant aucun en-tête de serveur, un projet synchrone ne tire
  pas `ESPAsyncWebServer` et un projet asynchrone ne tire pas `WebServer.h`.

- `examples/Basique_Async/` — exemple minimal pour `AsyncWebServer`.

### Compatibilité ascendante 1.x

`#include <ESP_OTA.h>` **reste équivalent** : le tronc inclut `ESP_OTA_Sync.h`
à la fin, sauf si `ESP_OTA_ASYNC_H` est défini (`ESP_OTA_Async.h` le définit
avant d'inclure le tronc) ou si l'appelant définit `ESP_OTA_TRONC_SEUL`.
**Aucun des 10 binaires du parc n'a besoin d'être modifié** — vérifié par
compilation d'un `main.cpp` 1.x inchangé.

### API asynchrone — identique à la synchrone

`ESP_OTA_Async<Serveur>` expose exactement les mêmes méthodes :
`begin`, `identifiants`, `chienDeGarde`, `delaiSansOctet`, `transfertMort`,
`avantUpload`, `tailleInconnue`, `liberer`, `loop`, `enCours`, `rebootDu`,
`etat`, `suspendu`, `delaiRedemarrage`, `titre`, `surDebut`, `surProgression`,
`surFin`. La migration d'un projet est un changement d'en-tête et de type.

### Neuf divergences réelles, vérifiées dans le source d'ESPAsyncWebServer 3.7.3

Aucune n'est une supposition : chacune est tracée à une ligne du framework.

| # | Constat | Ligne du framework | Traitement |
|---|---|---|---|
| A1 | `handleUpload` n'est appelé que si `_isMultipart` — le chemin RAW et son pointeur nul n'existent pas | `WebRequest.cpp:172` | Garde I3 conservée comme filtre défensif, mais le **crash distant est impossible par construction** |
| A2 | Un fichier de 0 octet ne délivre **jamais** `final` | `WebRequest.cpp:596` (`if (_itemSize)`) | `_arme` mémorise `Update.begin()` ; `_final()` abandonne si aucun signal de fin n'est venu |
| A3 | Pas d'`UPLOAD_FILE_ABORTED` | — | `request->onDisconnect()` — qui se déclenche **aussi après un succès** → garde N2 obligatoire |
| A4 | Pas de `sendContent()` chunked | `RESPONSE_STREAM_BUFFER_SIZE = 1460` | `beginResponseStream()` : 1460 o de tas, libérés après envoi. **I6 n'est pas tenu, il est mesuré** (voir plus bas) |
| A5 | Callbacks dans la tâche TCP, pas dans `loop()` | — | `suspendu()` devient **obligatoire**, pas conseillé |
| A6 | Pas de `collectHeaders()` | — | Un piège de moins que le synchrone |
| A7 | `requestAuthentication()` est **DIGEST** par défaut | `ESPAsyncWebServer.h:317` (`isDigest = true`) | Force `AsyncAuthType::AUTH_BASIC`, sinon `curl -F` reçoit 401 en boucle |
| A8 | `Update.begin()` dans la tâche TCP | — | Accepté sur ESP32 ; sur ESP8266 **compilé mais non validé par la mesure** |
| A9 | Deux uploads simultanés écriraient dans le même `Update` | — | Le second est refusé (503) et ses morceaux ignorés via `_reqConflit` |

### Mesuré

**RAM statique de l'instance** (`nm --print-size`, identique sur les deux
plateformes) :

| Classe | Taille |
|---|---|
| `ESP_OTA<Serveur>` (synchrone) | **148 o** |
| `ESP_OTA_Async<Serveur>` (asynchrone) | **160 o** |

Les +12 o sont `_reqRefusee` + `_reqAuthKo` + `_codeRefus` + `_arme` : le prix
du suivi par requête qu'impose la concurrence (A9).

**Code généré** (ESP8266, où les symboles sont lisibles) :

| Fonction | Synchrone | Asynchrone | Δ |
|---|---|---|---|
| `_page()` | 243 o | 254 o | +11 o |
| `_morceau()` | 747 o | 780 o | +33 o |

**Page HTML — I6 ne peut pas être tenu en asynchrone** :

| | Synchrone | Asynchrone |
|---|---|---|
| Envoi | `sendContent()` chunked | `beginResponseStream()` |
| RAM **statique** pour la page | 0 | **0** |
| Pic de **tas** | 0 | **1460 o** (`RESPONSE_STREAM_BUFFER_SIZE`) + ~48 o d'objet, libérés après envoi |
| Tampon `char[256]` des messages | oui | oui (identique) |

La page mesure ~980 o : elle tient dans le `reserve(1460)`, donc **aucune
réallocation** du `String` interne. La RAM statique reste nulle dans les deux
cas — c'est le pic de tas qui augmente, et il est chiffré plutôt que nié.

**Chemin synchrone : aucune régression.** Avant et après la découpe, mêmes
chiffres au bit près :

| Cible | RAM | Flash |
|---|---|---|
| `esp32dev` | 27 172 o | 480 900 o |
| `esp12e` | 29 472 o | 311 171 o |

### Vérifié

Trois projets de test, deux plateformes, compilation sans aucun avertissement
provenant de la librairie (`-Wall`) :

| Projet de test | En-tête inclus | `esp32dev` | `esp12e` |
|---|---|---|---|
| synchrone | `<ESP_OTA_Sync.h>` | SUCCESS | SUCCESS |
| rétro-compat 1.x | `<ESP_OTA.h>` | SUCCESS | SUCCESS |
| asynchrone | `<ESP_OTA_Async.h>` | SUCCESS | SUCCESS |

- Preuve de la découpe : le projet asynchrone contient
  `#ifdef WEBSERVER_H #error` après ses `#include` → il compile, donc
  `WebServer.h` **n'a pas** été tiré.
- Preuve de la compatibilité : le projet « rétro-compat » est du code 1.x
  inchangé (`#include <ESP_OTA.h>` + `ESP_OTA<WebServer> ota;`) → compile
  sans modification.

### Non vérifié — à mesurer avant tout déploiement

- **A8 (ESP8266)** : `Update.begin()` / `Update.write()` depuis la tâche TCP
  asynchrone. Compile, mais `UpdaterClass` n'a pas été conçu pour ce contexte.
  À valider par un transfert réel sur une carte avant usage en production.
- **A5** : la corruption du système de fichiers en cas d'oubli de
  `suspendu()` est un raisonnement, pas une reproduction.

---

## [1.0.1] — 14/11/2026

### Corrigé

- **Page `/update` tronquée** (`_page()`, `char b[160]`). Le tampon sert aux
  **deux** `snprintf` ; le second produit **231 octets** mesurés pour la route
  par défaut. Au-delà de 159 caractères la fin était perdue :
  - la commande `curl -F firmware=@firmware.bin http://<ip>/update` affichée à
    l'utilisateur s'arrêtait au milieu du nom de fichier — précisément l'aide
    qu'on lit quand un envoi échoue ;
  - les balises `</div></div></body></html>` disparaissaient.

  Invisible à l'œil : le navigateur referme le HTML tout seul, donc la page
  *paraissait* correcte. Détecté par le compilateur, qui le signalait depuis la
  1.0.0 sur les 10 projets du parc :
  ```
  ESP_OTA.h:375: warning: directive output truncated writing 187 bytes
                         into a region of size 160 [-Wformat-truncation=]
  ```
  Tampon porté à **256 o**, qui couvre les deux messages (110 o et 231 o).

### Vérifié

- `pio run -e d1_mini -e d1_mini_safe` sur `WeMos Mini (Lolin) - LinkysTIC` :
  build OK, plus aucun `-Wformat-truncation` provenant de `ESP_OTA.h`.

### Non corrigé (limite du coeur, hors librairie)

- `Parsing-impl.h:336` du coeur ESP8266 : `_uploadReadByte()` boucle
  `while (!client.available() && client.connected()) yield();` **sans timeout**.
  Un client qui envoie ses en-têtes puis se tait fige `handleClient()` à vie.
  `chienDeGarde()` ne peut pas aider sur ESP8266 (`CM_WatchDogAdd/Reset` de
  Crash_ESP_Monitor sont des no-ops `#if defined(ESP32)`). Seul remède :
  coupure de courant.

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
