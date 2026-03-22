# Contributing

Danke fuer deinen Beitrag zu WildKillLogger.

## Workflow

1. Fork erstellen
2. Branch anlegen (`feature/...`, `fix/...`, `chore/...`)
3. Aenderungen committen (kleine, nachvollziehbare Commits)
4. Pull Request gegen `main`

## Pull Request Checkliste

- [ ] Problem und Loesung klar beschrieben
- [ ] Breaking Changes genannt
- [ ] Konfig-Aenderungen dokumentiert
- [ ] Relevante Log-Ausgaben oder Testnachweise hinzugefuegt

## Coding Richtlinien

- keine sensiblen Daten in Commits
- Runtime-Artefakte (`*.log`, `*.csv`) nicht committen
- Rueckwaertskompatibilitaet fuer bestehende `config.json` beachten

## Was aktuell noch fehlt

Die PDF-Dokumentation verweist auf eine dokumentierte C++-Referenzdatei.
Falls du den Source-Code hast, eroeffne bitte ein PR und lege ihn unter `src/` ab.
