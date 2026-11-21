# Provisionne la configuration reseau DIRECTEMENT en NVS, sans passer par la
# page Configuration de l'ecran tactile.
#
#   .\tools\provision_nvs.ps1 -Ssid "MonWifi" -Pass "secret" -HostAddr "192.168.1.10"
#   .\tools\provision_nvs.ps1 -Ssid ... -Pass ... -HostAddr ... -SpkVol 60 -Backlight 80
#   .\tools\provision_nvs.ps1 -Ssid ... -DryRun          # genere l'image, ne flashe pas
#
# Pourquoi ce script existe : editer app_config.h ne sert a RIEN sur une carte
# deja provisionnee. `settings::init()` copie les defauts compiles PUIS les
# ecrase champ par champ avec ce que contient la NVS (settings.cpp:85-102), et
# `idf.py flash` n'ecrit que bootloader / table / application — la partition NVS
# survit intacte.
#
# ⚠️ Ce script REMPLACE toute la partition NVS (0x9000, 24 Ko). Tout ce qui n'est
# pas passe en parametre est perdu et repart sur les defauts compiles :
#   - namespace net_cfg    : ssid, pass, host
#   - namespace audio_cfg  : spk_vol (defaut 50), mic_gain (83), backlight (100)
#   - namespace esphome_api: la cle de chiffrement que Home Assistant a envoyee.
#     Effacee, la carte redevient "non provisionnee" : HA echoue alors avec
#     "invalid encryption key" et demande une reconfiguration de l'appareil
#     (supprimer puis re-adopter l'appareil ESPHome dans HA).
# Un champ omis ici n'est pas ecrit dans l'image, et `settings` retombe alors sur
# cfg::defaults.

param(
  [Parameter(Mandatory = $true)][string]$Ssid,
  [Parameter(Mandatory = $true)][string]$Pass,
  [string]$HostAddr = "",
  [int]$SpkVol    = -1,
  [int]$MicGain   = -1,
  [int]$Backlight = -1,
  [string]$Port = "COM7",
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Doivent suivre partitions.csv. Verifiables a tout moment :
#   python $IDF_PATH\components\partition_table\gen_esp32part.py build\partition_table\partition-table.bin
$nvsOffset = "0x9000"
$nvsSize   = "0x6000"   # 24 Ko

$env:IDF_PATH = "C:\esp\v6.0.1\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$python = "C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe"
$gen = "$env:IDF_PATH\components\nvs_flash\nvs_partition_generator\nvs_partition_gen.py"

if (-not (Test-Path $python)) { throw "python du venv IDF introuvable : $python" }
if (-not (Test-Path $gen))    { throw "nvs_partition_gen.py introuvable : $gen" }

# Le CSV porte le mot de passe Wi-Fi en clair : il vit dans le dossier temporaire
# et est efface en sortie, jamais dans le depot.
$work = Join-Path $env:TEMP "nvs_provision_$PID"
New-Item -ItemType Directory -Force $work | Out-Null
$csv = Join-Path $work "net_cfg.csv"
$bin = Join-Path $work "nvs.bin"

try {
  $lines = @("key,type,encoding,value", "net_cfg,namespace,,")
  $lines += "ssid,data,string,$Ssid"
  $lines += "pass,data,string,$Pass"
  if ($HostAddr) { $lines += "host,data,string,$HostAddr" }

  if ($SpkVol -ge 0 -or $MicGain -ge 0 -or $Backlight -ge 0) {
    $lines += "audio_cfg,namespace,,"
    if ($SpkVol    -ge 0) { $lines += "spk_vol,data,u8,$SpkVol" }
    if ($MicGain   -ge 0) { $lines += "mic_gain,data,u8,$MicGain" }
    if ($Backlight -ge 0) { $lines += "backlight,data,u8,$Backlight" }
  }

  Set-Content -Path $csv -Value $lines -Encoding ASCII
  Write-Host "Champs ecrits : $(($lines | Where-Object { $_ -notmatch 'namespace|^key,' }).Count)" -ForegroundColor DarkGray

  & $python $gen generate $csv $bin $nvsSize
  if ($LASTEXITCODE -ne 0) { throw "generation de l'image NVS echouee" }

  if ($DryRun) {
    $keep = Join-Path $repo "nvs.bin"
    Copy-Item $bin $keep -Force
    Write-Host "DryRun : image ecrite dans $keep, rien n'a ete flashe." -ForegroundColor Yellow
    Write-Host "Flasher manuellement : python -m esptool --chip esp32p4 -p $Port write_flash $nvsOffset nvs.bin"
    return
  }

  Write-Host "Ecriture de la partition NVS a $nvsOffset sur $Port..." -ForegroundColor Cyan
  & $python -m esptool --chip esp32p4 -p $Port write_flash $nvsOffset $bin
  if ($LASTEXITCODE -ne 0) { throw "flash de la NVS echoue (port occupe par un moniteur ?)" }

  Write-Host "OK. La carte redemarre sur la nouvelle configuration." -ForegroundColor Green
}
finally {
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
