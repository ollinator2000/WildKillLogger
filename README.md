# WildKillLogger (ARK: Survival Ascended / ASA Plugin)

`WildKillLogger` ist ein ARK Server API Plugin fuer ARK: Survival Ascended Dedicated Server.
Das Plugin erkennt PvE-Wild-Dino-Kills heuristisch und schreibt sie als CSV fuer Auswertung, Leaderboards oder Discord-Bots.

## Ueberblick

Da ASA aktuell keine stabilen Damage-/Kill-Hooks in allen Setups bietet, nutzt das Plugin eine robuste Naeherungslogik auf Basis von:
- `AActor.Destroyed()` Events
- bekannten Spielerpositionen
- raeumlicher Naehe + zeitlicher Frische

Ziel ist eine saubere, verwertbare Kill-Datenbasis statt moeglichst vieler unsicherer Treffer.

## Features

- zuverlaessige PvE-Kill-Erkennung (heuristisch)
- keine unsicheren Memory-/Offset-Hooks
- saubere CSV-Ausgabe fuer Weiterverarbeitung
- ausfuehrliches Debug-Logging fuer Diagnose
- Konfiguration ohne Rebuild ueber `config.json`
- performantes, thread-basiertes Spielertracking

## Funktionsweise

### Player Tracking
Beim Join werden Spieler registriert (u. a. EOS-ID, Name, Position).

### Hintergrund-Thread
Aktualisiert regelmaessig die zuletzt bekannte Spielerposition.

### Dino Detection
Hook ueber `AActor.Destroyed()`, anschliessend Dino-Blueprint-Filter.

### Kill Attribution (Confidence)
- genau 1 plausibler Spieler im Radius -> `high`
- mehrere Spieler -> `low`
- kein passender Spieler -> `unknown`

Standardmaessig werden nur `high`-Faelle in die CSV geschrieben.

## Ausgabe-Dateien (Runtime)

Pfad (Server): `ArkApi/Plugins/WildKillLogger/`

- `wild_kills.csv`
- `wildkilllogger_debug.log`
- `config.json`

Die Laufzeitdateien `*.csv` und `*.log` sind absichtlich in `.gitignore`.

## Installation

1. `WildKillLogger.dll` nach `ArkApi/Plugins/WildKillLogger/` kopieren.
2. `config.json` lokal aus `config.json.example` erzeugen oder vom Plugin erzeugen lassen.
3. Server neu starten.
4. Pruefen, ob `wild_kills.csv` und `wildkilllogger_debug.log` erzeugt werden.

## Konfiguration

Details: `docs/CONFIG_REFERENCE.md`

Standardwerte:

```json
{
  "kill_radius": 3000.0,
  "position_update_ms": 2000,
  "player_fresh_seconds": 15,
  "stale_player_seconds": 86400,
  "enable_position_thread": false,
  "write_debug_log": true,
  "write_only_high_confidence": true,
  "debug_log_non_dino_destroy": false,
  "debug_log_skipped_kills": true,
  "debug_log_sample_rate": 1,
  "kill_csv_filename": "wild_kills.csv",
  "debug_log_filename": "wildkilllogger_debug.log"
}
```

Hinweis fuer Linux/Wine:
- `enable_position_thread: false` ist der stabile Sicherheitsmodus.
- `enable_position_thread: true` nutzt Legacy-Tracking mit hoeherer Genauigkeit, kann aber in bestimmten Wine-Setups instabil sein.
- `player_fresh_seconds` wird nur bei aktiviertem Positionsthread fuer die Freshness-Pruefung verwendet.
- `debug_log_sample_rate` reduziert Hot-Path-Loglast (`1`=voll, `10`=jede 10. Debug-Zeile).

## Repository-Status

Dieses Repository ist als Community-Repo vorbereitet.

Aktuell enthalten:
- `WildKillLogger.dll` (kompilierte Plugin-Datei)
- `PluginInfo.json`
- `config.json.example` (Vorlage; `config.json` bleibt lokal und wird nicht versioniert)
- `WildKillLogger_Config_Dokumentation.pdf`

Hinweis:
Die PDF verweist zusaetzlich auf `WildKillLogger_Main_config_documented.cpp`.
Die dokumentierte Source-Datei liegt jetzt unter `src/WildKillLogger_Main_config_documented.cpp`.

## Entwicklung / Community

1. Issue erstellen (Bug/Feature/Refactor)
2. Branch von `main` erstellen
3. Aenderung inklusive Validierung
4. Pull Request mit Repro-Schritten und erwarteter Wirkung

Details: `CONTRIBUTING.md`

## Lizenz

MIT, siehe `LICENSE`.

## Credits

Technische Basis und Funktionsbeschreibung stammen aus `WildKillLogger_Config_Dokumentation.pdf`.
