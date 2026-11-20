# Configure, compile et lance les tests host.
#   .\run.ps1              -> build + run
#   .\run.ps1 -Clean       -> repart de zéro
#   .\run.ps1 -Filter Foo* -> ne lance qu'un sous-ensemble de tests
#
# CMake et Ninja sont pris dans l'installation ESP-IDF : ils y sont déjà et ne
# sont pas sur le PATH global de la machine. Le COMPILATEUR, lui, doit être
# installé à part (cf. README.md) — celui d'ESP-IDF est un cross-compilateur
# RISC-V, il ne produit pas d'exécutable PC.

param(
  [switch]$Clean,
  # Regex ctest (pas un glob) : "G711.*" et non "*".
  [string]$Filter = ""
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $here "build"

$cmake = Get-ChildItem "C:\Espressif\tools\cmake\*\bin\cmake.exe" -ErrorAction SilentlyContinue |
         Sort-Object FullName -Descending | Select-Object -First 1
$ctest = Get-ChildItem "C:\Espressif\tools\cmake\*\bin\ctest.exe" -ErrorAction SilentlyContinue |
         Sort-Object FullName -Descending | Select-Object -First 1
$ninjaDir = Get-ChildItem "C:\Espressif\tools\ninja\*\ninja.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1

if (-not $cmake) { throw "cmake introuvable sous C:\Espressif\tools\cmake" }
if ($ninjaDir) { $env:PATH = "$($ninjaDir.Directory.FullName);$env:PATH" }

# MinGW DOIT vivre à un chemin court. GCC construit son chemin d'include interne
# sans normaliser les "..", ce qui ajoute ~60 caractères, et il n'est pas
# long-path aware : depuis le dossier winget (nom de paquet à rallonge) le
# préfixe atteint 245 chars et tout header dont le nom dépasse 14 caractères
# devient introuvable — "bits/error_constants.h: No such file or directory",
# alors que bits/c++config.h du même dossier se résout. Ni jonction ni subst
# n'aident : GCC résout jusqu'au chemin réel.
if (Test-Path "D:\mingw64\bin\g++.exe") { $env:PATH = "D:\mingw64\bin;$env:PATH" }

$compiler = @("g++", "clang++", "cl") | ForEach-Object { Get-Command $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
if (-not $compiler) {
  throw "Aucun compilateur C++ host sur le PATH (g++, clang++ ou cl). Voir test\README.md."
}
Write-Host "Compilateur : $($compiler.Source)" -ForegroundColor DarkGray

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

& $cmake.FullName -S $here -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE -ne 0) { throw "configure a échoué" }

& $cmake.FullName --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "build a échoué" }

$ctestArgs = @("--test-dir", $buildDir, "--output-on-failure")
if ($Filter) { $ctestArgs += @("-R", $Filter) }

& $ctest.FullName @ctestArgs
exit $LASTEXITCODE
