# AGENTS — ESP_OTA

Librairie header-only de mise à jour OTA par HTTP, pour serveur web
**synchrone** ESP8266 / ESP32. Version 1.0.0.

---

## 1. Ce que fait la librairie

| Élément | Rôle |
|---|---|
| `src/ESP_OTA.h` | tout : page, upload, état, crochets |
| `examples/Basique/` | exemple minimal |

Template sur le type de serveur : `ESP_OTA<WebServer>` (ESP32) ou
`ESP_OTA<ESP8266WebServer>` (ESP8266).

---

## 2. Invariants — NE PAS RÉGRESSER

### I1 — `Update.begin()` à `UPLOAD_FILE_START`

C'est **la** raison d'être de la librairie. Le déplacer dans un handler
`GET /ota/start` ramènerait le protocole en deux étapes et le faux `200 OK`.

### I2 — Le code HTTP reflète l'état réel

`200` seulement si `Update.end()` a réussi **et** `taille > 0`. Sinon `500`.
ElegantOTA répondait `200` quand zéro octet avait été écrit : la carte reboutait
sur l'ancien firmware en annonçant un succès.

### I3 — Tester le `Content-Type` AVANT `server.upload()`

Un POST non multipart arrive par le chemin RAW de `_parseRequest`, où
`_currentUpload` n'existe pas. `server.upload()` déréférence un pointeur nul →
panique → reboot à distance **sans authentification**.

⚠️ Ce test exige que `Content-Type` soit dans la liste `collectHeaders()`.
`begin()` s'en charge quand `gardeMultipart` est vrai.

### I4 — Authentifier AVANT d'écrire en flash

Le contrôle doit être à `UPLOAD_FILE_START`. Placé dans le handler final, il
arriverait **après** l'écriture des ~1 Mo de firmware.

### I5 — Redémarrage différé

Jamais `ESP.restart()` dans le handler final : la réponse HTTP serait coupée et
le navigateur afficherait une erreur alors que l'OTA a réussi. Le redémarrage
est piloté par `loop()` après `ESP_OTA_DELAI_REBOOT_MS`.

### I6 — Aucune allocation dynamique pour la page

Page envoyée en **chunked** (`setContentLength(CONTENT_LENGTH_UNKNOWN)` +
`sendContent`). Pas de tampon de la taille de la page : une première version
bufferisait 900 o en pile, et la mesure a montré que ce tampon était compté
dans la RAM statique (−960 o après passage au flux).

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

## 3. Modifier la librairie

1. Toute correction **doit** être justifiée par un constat mesuré (reproduction
   du défaut) — pas par une préférence de style.
2. Mettre à jour `__ESP_OTA_H_VERSION__`, `library.json` et
   `library.properties` **ensemble** (les trois portent la version).
3. Ajouter une entrée dans `CHANGELOG.md` : quoi, pourquoi, comment vérifié.
4. Vérifier la compilation sur **les deux** plateformes :
   ```bash
   pio run -e esp12e      # ESP8266
   pio run -e esp32dev    # ESP32
   ```
5. Vérifier le comportement réel :
   - `curl -F firmware=@x.bin http://<ip>/update` → une seule requête, `200`
   - POST non multipart → `401`/`500`, **pas de reboot**
   - fichier vide → `500`, pas de `200`

---

## 4. Ce que la librairie ne fait PAS

- Pas de mise à jour « pull » (l'appareil ne va pas chercher son firmware) →
  voir `AutoUpdate_ESP`.
- Pas de vérification de signature d'image.
- Pas de file d'attente : un upload à la fois.
- Pas de compatibilité `AsyncWebServer` (le parc est synchrone).

---

## 5. Style

Français, commentaires explicatifs qui disent **pourquoi** (pas seulement quoi),
accentuation ASCII dans le code, tableaux dans les en-têtes pour les
avertissements. Voir `EspNow_UniVar` et `Guard_ESP` pour le ton.
