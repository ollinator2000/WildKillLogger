# Konfigurationsreferenz

Datei: `config.json`

| Feld | Typ | Default | Bedeutung |
|---|---|---|---|
| `kill_radius` | float | `3000.0` | Maximaler Abstand zwischen Dino und Spieler fuer Kill-Zuordnung |
| `position_update_ms` | int | `2000` | Intervall der Positionsupdates |
| `player_fresh_seconds` | int | `15` | Maximales Alter der letzten Spielerposition |
| `write_debug_log` | bool | `true` | Aktiviert/Deaktiviert Debug-Log |
| `write_only_high_confidence` | bool | `true` | Nur eindeutige Kills in CSV |
| `debug_log_non_dino_destroy` | bool | `false` | Loggt verworfene Nicht-Dino-Destroy-Events |
| `debug_log_skipped_kills` | bool | `true` | Loggt low/unknown-Skips |
| `kill_csv_filename` | string | `wild_kills.csv` | Name der Kill-CSV |
| `debug_log_filename` | string | `wildkilllogger_debug.log` | Name der Debug-Log-Datei |

CSV-Header:

```text
timestamp_utc,dino_blueprint,dino_x,dino_y,dino_z,killer_eos,killer_name,nearest_distance
```
