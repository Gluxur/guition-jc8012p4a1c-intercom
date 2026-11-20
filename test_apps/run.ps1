# Compile / flashe / lance le firmware de test sur la carte.
#   .\run.ps1              -> build seul
#   .\run.ps1 -Flash       -> build + flash + moniteur (menu Unity interactif)
#   .\run.ps1 -Port COM12  -> autre port que COM7
#
# ⚠️ Flasher ce firmware REMPLACE l'application sur la carte. Reflasher le
# projet de la racine pour revenir au visiophone.
#
# Les scripts export.ps1 d'ESP-IDF ne fonctionnent pas sur cette machine
# (agencement de l'extension VS Code : pas de python_env sous C:\Espressif),
# d'ou l'environnement monte a la main ci-dessous.
#
# ATTENTION : ne JAMAIS ajouter `set-target` ici — la commande reinitialise
# sdkconfig. Cf. le commentaire equivalent dans ..\run.ps1.

param(
  [switch]$Flash,
  [string]$Port = "COM7"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Versions alignees sur C:\esp\v6.0.1\esp-idf\tools\tools.json (migration 5.4.4
# -> 6.0.1 du 2026-07-30) : toolchain esp-15.2.0_20251204, cmake 4.0.3.
$env:IDF_PATH = "C:\esp\v6.0.1\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python\v6.0.1\venv"
# esp_hosted lit $(ESP_IDF_VERSION) dans son Kconfig ; vide, la generation de
# config casse. Inoffensif ici (pas de Wi-Fi), garde par parite avec le build
# du firmware principal.
$env:ESP_IDF_VERSION = "6.0"
$env:IDF_TARGET = "esp32p4"
# Sans ca, le build echoue en fin de course sur "ESP_ROM_ELF_DIR environment
# variable is not defined / Error while generating esp_rom gdbinit". Sans effet
# sur le binaire, mais ca casse la commande.
$env:ESP_ROM_ELF_DIR = "C:\Espressif\tools\esp-rom-elfs\20241011"
$env:PATH = "C:\Espressif\tools\python\v6.0.1\venv\Scripts;" +
            "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;" +
            "C:\Espressif\tools\cmake\4.0.3\bin;" +
            "C:\Espressif\tools\ninja\1.12.1;" +
            "C:\Espressif\tools\ccache\4.12.1;" + $env:PATH

Set-Location $here

if ($Flash) {
  python "$env:IDF_PATH\tools\idf.py" -p $Port flash monitor
} else {
  python "$env:IDF_PATH\tools\idf.py" build
}
exit $LASTEXITCODE
