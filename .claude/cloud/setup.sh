#!/bin/bash
# Setup-Skript für die Claude-Code-Cloud-Umgebung "RPCS3".
# In claude.ai/code -> Umgebung -> "Setup script" einfügen oder per "Choose file" laden.
#
# Läuft einmalig als root auf Ubuntu 24.04, bevor Claude Code startet. Das Ergebnis
# (Dateisystem inkl. Docker-Image) wird ca. 7 Tage zwischengespeichert. Das Skript muss
# mit Exit-Code 0 enden und sollte in ca. 5 Minuten fertig sein, sonst wird nicht gecacht.
#
# RPCS3_CI_IMAGE an .github/workflows/rpcs3.yml (Linux_Build, x86_64) angleichen, wenn
# upstream das Image erneuert. scripts/claude-cloud/build.sh liest die Version selbst aus
# der Workflow-Datei und lädt ein neueres Image notfalls pro Sitzung nach.

RPCS3_CI_IMAGE="rpcs3/rpcs3-ci-jammy:2.2"

log() { echo "[rpcs3-setup] $*"; }

# Host-Werkzeuge (Formatierung, Shell-Lint) parallel zum Image-Download
(
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq && apt-get install -y -qq --no-install-recommends clang-format shellcheck
) >/tmp/rpcs3-setup-apt.log 2>&1 &
apt_pid=$!

# Docker-Daemon sicherstellen
started_dockerd=0
if ! docker info >/dev/null 2>&1; then
	log "starte dockerd ..."
	nohup dockerd >/var/log/dockerd.log 2>&1 &
	dockerd_pid=$!
	started_dockerd=1
	for _ in $(seq 1 60); do
		docker info >/dev/null 2>&1 && break
		sleep 1
	done
fi

# Offizielles RPCS3-CI-Image laden (Qt 6.12, LLVM 22, clang-22/gcc-13, Vulkan, SDL3, ffmpeg)
if docker info >/dev/null 2>&1; then
	log "lade $RPCS3_CI_IMAGE (ca. 1 GB) ..."
	if docker pull --quiet "$RPCS3_CI_IMAGE"; then
		log "Image bereit"
	else
		log "WARNUNG: docker pull fehlgeschlagen, build.sh versucht es erneut"
	fi
else
	log "WARNUNG: Docker nicht erreichbar, build.sh versucht es erneut"
fi

wait "$apt_pid" || log "WARNUNG: apt-Installation fehlgeschlagen (siehe /tmp/rpcs3-setup-apt.log)"

# Selbst gestarteten Daemon sauber beenden, damit der Cache-Snapshot konsistent ist
if [ "$started_dockerd" = 1 ]; then
	kill "$dockerd_pid" 2>/dev/null && wait "$dockerd_pid" 2>/dev/null
fi

log "fertig"
exit 0
