# Konfigurationsreferenz

Datei: `config.json`

| Feld | Typ | Default | Bedeutung |
|---|---|---|---|
| `kill_radius` | float | `3000.0` | Maximaler Abstand zwischen Dino und Spieler fuer Kill-Zuordnung |
| `position_update_ms` | int | `2000` | Intervall der Positionsupdates |
| `player_fresh_seconds` | int | `15` | Maximales Alter der letzten Spielerposition (relevant bei `enable_position_thread=true`) |
| `stale_player_seconds` | int | `86400` | Verwirft sehr alte Spieler-Snapshots aus der internen Liste |
| `enable_position_thread` | bool | `false` | Aktiviert Legacy-Positions-Thread (unter Wine eher deaktiviert lassen) |
| `write_debug_log` | bool | `false` | Aktiviert/Deaktiviert Debug-Log (Default fuer Live-Betrieb) |
| `write_only_high_confidence` | bool | `true` | Nur eindeutige Kills in CSV |
| `debug_log_non_dino_destroy` | bool | `false` | Loggt verworfene Nicht-Dino-Destroy-Events |
| `debug_log_skipped_kills` | bool | `true` | Loggt low/unknown-Skips |
| `debug_log_sample_rate` | int | `1` | Sampling fuer haeufige Debug-Zeilen (`1`=jede, `10`=jede 10.) |
| `write_rejected_kills` | bool | `false` | Schreibt verworfene Kill-Kandidaten (low/unknown) in eine separate CSV |
| `kill_csv_filename` | string | `wild_kills.csv` | Name der Kill-CSV |
| `rejected_csv_filename` | string | `rejected_kills.csv` | Name der CSV fuer verworfene Kill-Kandidaten |
| `debug_log_filename` | string | `wildkilllogger_debug.log` | Name der Debug-Log-Datei |

CSV-Header:

```text
timestamp_utc,dino_blueprint,dino_x,dino_y,dino_z,killer_eos,killer_name,nearest_distance
```
