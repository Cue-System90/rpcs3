# Cloud-Vorlage für RPCS3 (Claude Code)

Diese Vorlage richtet Claude-Code-Cloudsitzungen für den Fork `Cue-System90/rpcs3` ein.
Gebaut wird im offiziellen RPCS3-CI-Docker-Image. Die Toolchain entspricht damit der Upstream-CI
(clang-22/gcc-13, Qt 6.12, LLVM 22, Vulkan, SDL3, ffmpeg).

## Bestandteile

| Datei | Zweck |
|---|---|
| `.claude/cloud/setup.sh` | Setup-Skript der Cloud-Umgebung: lädt das CI-Image, wird ca. 7 Tage gecacht |
| `scripts/claude-cloud/build.sh` | holt die Submodule und baut bzw. testet im Container (`--help`) |
| `CLAUDE.md` | Arbeitsanweisungen für Claude in diesem Repo |

## Einmalig: Umgebung anlegen

1. [claude.ai/code](https://claude.ai/code) öffnen, unter dem Eingabefeld die Umgebungsauswahl öffnen und **New environment** wählen.
2. Ausfüllen:
   - **Name**: `RPCS3` (falls das Feld angeboten wird)
   - **Network access**: `Trusted`. Das reicht, denn GitHub, Docker Hub und die Ubuntu-Paketquellen stehen auf der Standardliste.
   - **Environment variables** (erlaubt längere Befehle, z. B. Builds):
     ```
     BASH_DEFAULT_TIMEOUT_MS=600000
     BASH_MAX_TIMEOUT_MS=3600000
     ```
   - **Setup script**: Inhalt von `.claude/cloud/setup.sh` einfügen oder per **Choose file** laden
   - **Who can use it**: nur du
3. **Add environment** klicken.

## GitHub-Zugriff

Damit Claude Branches in den Fork pushen kann, braucht claude.ai Zugriff auf `Cue-System90/rpcs3`:
entweder die Claude-GitHub-App für das Repo installieren oder einmal in einem lokalen `claude`-Terminal `/web-setup` ausführen.

## Sitzung starten

- claude.ai/code oder Claude-Desktop-App: neue Cloudsitzung mit Repo `Cue-System90/rpcs3` und Umgebung `RPCS3`.
- Lokale Arbeitskopie: `C:\Users\brenne\Desktop\Projekte\rpcs3` (Remotes `origin` = Fork, `upstream` = RPCS3).
- Beispiel-Prompts:
  - „Baue `rpcs3_emu` und erkläre mir, wie `cellSaveData` AutoSave behandelt.“
  - „Lege einen Branch an, behebe <Problem> und prüfe es mit `--tests`.“
  - „Prüfe, ob der aktuelle Stand auch mit GCC kompiliert.“

## Grenzen

- Keine GPU und kein Bildschirm: Spiele lassen sich in der Cloud nicht starten. Geprüft wird per Build und Unit-Tests.
- Laut Anthropic ca. 4 vCPUs, 16 GB RAM und 30 GB Platte (kann sich ändern). Ein Vollbuild dauert entsprechend lange,
  deshalb startet Claude ihn im Hintergrund und liest das Log.
- Jede Sitzung beginnt mit einem frischen Klon. Die Submodule (ohne LLVM) werden pro Sitzung neu geladen,
  der ccache gilt nur innerhalb der Sitzung.

## Wartung

- **Neues CI-Image upstream** (z. B. `rpcs3-ci-jammy:2.3` in `.github/workflows/rpcs3.yml`): `build.sh` erkennt die Version
  selbst und lädt sie pro Sitzung nach. Damit sie wieder gecacht wird, `RPCS3_CI_IMAGE` in `setup.sh` anpassen und das
  geänderte Skript in der Umgebung speichern.
- **Fork aktualisieren**: auf GitHub **Sync fork**, oder lokal `git fetch upstream` und danach `git merge upstream/master`.
- `CLAUDE.md` steht upstream in `.gitignore` und wurde mit `git add -f` eingecheckt; Änderungen daran werden normal erfasst.
