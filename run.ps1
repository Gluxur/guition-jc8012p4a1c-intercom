# Compile / flashe / surveille le FIRMWARE DE PRODUCTION (visiophone).
#   .\run.ps1                 -> build seul
#   .\run.ps1 -Flash          -> build + flash + moniteur
#   .\run.ps1 -Monitor        -> moniteur seul (carte deja flashee)
#   .\run.ps1 -Clean          -> fullclean puis build
#   .\run.ps1 -Port COM12     -> autre port que COM7
#
# Pendant le moniteur : Ctrl+] pour sortir.
#
# Pendant : .\test_apps\run.ps1 -Flash met le firmware de TEST sur la carte.
# Ce script est le chemin de retour vers l'application.
#
# Les scripts export.ps1 d'ESP-IDF ne fonctionnent pas sur cette machine
# (agencement de l'extension VS Code : pas de python_env sous C:\Espressif),
# d'ou l'environnement monte a la main ci-dessous. Meme recette que
# test_apps\run.ps1.
#
# ATTENTION : ne JAMAIS ajouter `set-target` ici. La commande REINITIALISE
# sdkconfig (137+ options reglees a la main perdues : optim PERF, CPU 360 MHz,
# broches SDIO d'esp_hosted, LV_USE_LOTTIE...). La cible est deja fixee, et
# -Clean / fullclean ne touche pas au sdkconfig.

param(
  [switch]$Flash,
  [switch]$Monitor,
  [switch]$Clean,
  [string]$Port = "COM7"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Versions alignees sur C:\esp\v6.0.1\esp-idf\tools\tools.json (migration 5.4.4
# -> 6.0.1 du 2026-07-30) : toolchain esp-15.2.0_20251204, cmake 4.0.3.
$env:IDF_PATH = "C:\esp\v6.0.1\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python\v6.0.1\venv"
# OBLIGATOIRE ici (contrairement au firmware de test, qui ne tire pas le
# Wi-Fi) : le Kconfig d'esp_hosted lit $(ESP_IDF_VERSION), vide sinon, et
# kconfgen echoue a l'analyse.
$env:ESP_IDF_VERSION = "6.0"
$env:IDF_TARGET = "esp32p4"
# Sans ca, le build echoue en fin de course sur "ESP_ROM_ELF_DIR environment
# variable is not defined / Error while generating esp_rom gdbinit". Sans effet
# sur le binaire, mais ca casse la commande. 20241011 est la version demandee
# par tools.json de 6.0.1 (inchangee depuis 5.4.4).
$env:ESP_ROM_ELF_DIR = "C:\Espressif\tools\esp-rom-elfs\20241011"
$env:PATH = "C:\Espressif\tools\python\v6.0.1\venv\Scripts;" +
            "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;" +
            "C:\Espressif\tools\cmake\4.0.3\bin;" +
            "C:\Espressif\tools\ninja\1.12.1;" +
            "C:\Espressif\tools\ccache\4.12.1;" + $env:PATH

Set-Location $here
$idf = "$env:IDF_PATH\tools\idf.py"

if ($Monitor) {
  python $idf -p $Port monitor
  exit $LASTEXITCODE
}

if ($Clean) {
  # `idf.py fullclean` plutot qu'un Remove-Item : VS Code garde
  # build\.cache\clangd ouvert, une suppression brute echoue partiellement.
  # L'echec n'est pas fatal, le build qui suit regenere ce qu'il faut.
  python $idf fullclean
  if ($LASTEXITCODE -ne 0) {
    Write-Host "fullclean incomplet (VS Code tient des fichiers ?), on continue" -ForegroundColor DarkYellow
  }
}

if ($Flash) {
  python $idf -p $Port flash monitor
} else {
  python $idf build
}
exit $LASTEXITCODE
