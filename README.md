# WildKillLogger (ARK: Survival Ascended)

`WildKillLogger` ist ein ARK Server API Plugin fuer ARK: Survival Ascended Dedicated Server.
Das Plugin schreibt Wild-Dino-Kills als CSV fuer Auswertung, Leaderboards oder Discord-Bots.

## Status

Dieses Repository ist jetzt als GitHub-Community-Repo vorbereitet.

Aktuell sind im Projekt enthalten:
- `WildKillLogger.dll` (kompilierte Plugin-Datei)
- `PluginInfo.json`
- `config.json` (laufende Konfiguration)
- `WildKillLogger_Config_Dokumentation.pdf` (technische Doku)

Hinweis:
Die PDF nennt zusaetzlich `WildKillLogger_Main_config_documented.cpp` und `config.json.example`.
Diese beiden Dateien sind im aktuellen Stand nicht enthalten.

## Was das Plugin tut

- registriert Spieler beim Join
- aktualisiert Spielerpositionen in einem Hintergrund-Thread
- lauscht auf `AActor.Destroyed()`
- filtert auf Dino-Character-Blueprints
- ordnet Dino-Destroy-Ereignisse heuristisch einem Spieler zu (Radius + Freshness)
- schreibt nur zuverlaessige (`high confidence`) Kills in die CSV (Standard)
- schreibt detaillierte Diagnosen in ein Debug-Log

## Ausgabe-Dateien (Runtime)

- `wild_kills.csv`
- `wildkilllogger_debug.log`

Diese Dateien werden zur Laufzeit im Plugin-Ordner erzeugt und sind **nicht** fuer Git bestimmt.

## Installation auf ASA-Server

1. `WildKillLogger.dll` in `ArkApi/Plugins/WildKillLogger/` kopieren.
2. `config.json` im selben Ordner belassen oder anpassen.
3. Server neu starten.
4. Pruefen, ob `wild_kills.csv` und `wildkilllogger_debug.log` entstehen.

## Konfiguration

Siehe `config.json` und `docs/CONFIG_REFERENCE.md`.

Standardwerte:

```json
{
  "kill_radius": 3000.0,
  "position_update_ms": 2000,
  "player_fresh_seconds": 15,
  "write_debug_log": true,
  "write_only_high_confidence": true,
  "debug_log_non_dino_destroy": false,
  "debug_log_skipped_kills": true,
  "kill_csv_filename": "wild_kills.csv",
  "debug_log_filename": "wildkilllogger_debug.log"
}
```

## Entwicklung / Community

Wenn du am Plugin weiterarbeiten willst:

1. Issue anlegen (Bug/Feature/Refactor)
2. Branch von `main` erstellen
3. Aenderung + Tests/Validierung
4. Pull Request mit Repro-Schritten und erwarteter Wirkung

Details: `CONTRIBUTING.md`

## Lizenz

MIT, siehe `LICENSE`.

## Credits

Projektbasis und Funktionsbeschreibung stammen aus `WildKillLogger_Config_Dokumentation.pdf`.
