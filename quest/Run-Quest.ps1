# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Installs PrimedGun v2 on the connected Quest, starts the game and collects its logs.

.DESCRIPTION
Installs the APK Build-Quest.ps1 made, optionally places a disc image, starts the
VR activity directly (skipping the launcher panel) and captures one session's
diagnostics in quest/.runs/<stamp>/: the logcat (UTF-8), the game's own log file,
and last_error.txt when the session could not start.

Android storage rules this works within:
 * Nothing is created under Android/data/<package> through adb before the app has
   run: those folders would belong to the shell user and the app could not write
   its settings, saves or log there. The app is started once first.
 * Files adb places stay owned by the shell user; they are made readable and
   writable for the app.
 * The disc is pushed to a staging folder, then moved, so a broken push never
   leaves a half-written disc.iso where the game looks.

.PARAMETER Disc
A Metroid Prime (USA) revision 0 disc image to place as the game's disc.iso.

.PARAMETER Settings
A port_settings.ini to place in the user folder (for test runs).

.PARAMETER BootWorld
Boots straight into a world, as MP_BOOT_WORLD (39F2DE28:B2701146 is the Landing Site).

.EXAMPLE
quest\Run-Quest.ps1 -Disc "D:\Nintendo GameCube\Games\Metroid Prime (USA).nkit.iso" -Seconds 120
#>
[CmdletBinding()]
param(
    [string]$Apk = '',
    [string]$Disc = '',
    [string]$Settings = '',
    # A world (and area) to boot straight into, as MP_BOOT_WORLD: 39F2DE28:B2701146 is the Landing Site.
    [string]$BootWorld = '',
    [ValidateSet('release', 'debug')] [string]$Configuration = 'release',
    [int]$Seconds = 60,
    [switch]$NoLaunch,
    [switch]$SkipInstall
)
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
$adb = Join-Path $sdkRoot 'platform-tools\adb.exe'
if (-not (Test-Path $adb)) { throw "adb not found at $adb" }

$package = 'org.primedgun.v2'
$activity = "$package/.PrimedGunVrActivity"
$userDir = "/sdcard/Android/data/$package/files"
$stageDir = '/sdcard/Download/PrimedGunQuestStaging'

function Invoke-Adb { param([string[]]$Arguments) & $adb @Arguments; if ($LASTEXITCODE -ne 0) { throw "adb $($Arguments -join ' ') failed ($LASTEXITCODE)" } }
# adb shell output carries a trailing CR, so trim before comparing.
function Test-DevicePath { param([string]$Path, [string]$Kind = 'd') ((& $adb shell "test -$Kind '$Path' && echo yes") | Out-String).Trim() -eq 'yes' }

$devices = (& $adb devices) -match "device$"
if (-not $devices) { throw 'No device in "device" state; check the Quest is connected and USB debugging is allowed.' }

if (-not $SkipInstall) {
    if (-not $Apk) {
        $Apk = Get-ChildItem -Path (Join-Path $root "app\build\outputs\apk\$Configuration") -Filter '*.apk' -ErrorAction SilentlyContinue |
            Select-Object -First 1 | ForEach-Object FullName
    }
    if (-not $Apk -or -not (Test-Path $Apk)) { throw 'No APK found; run Build-Quest.ps1 first or pass -Apk' }
    Write-Host "Installing $Apk"
    Invoke-Adb @('install', '-r', '-g', $Apk)
}

if (-not (Test-DevicePath $userDir)) {
    Write-Host 'First launch so the app creates its own folders'
    Invoke-Adb @('shell', 'am', 'start', '-W', '-n', $activity)
    for ($i = 0; $i -lt 30 -and -not (Test-DevicePath $userDir); ++$i) { Start-Sleep -Seconds 1 }
    & $adb shell am force-stop $package
    if (-not (Test-DevicePath $userDir)) { throw "The app did not create $userDir" }
}

function Push-UserFile([string]$LocalPath, [string]$Name) {
    if (-not (Test-Path $LocalPath)) { throw "$LocalPath not found" }
    Invoke-Adb @('shell', 'mkdir', '-p', $stageDir)
    Write-Host "Pushing $LocalPath as $Name"
    Invoke-Adb @('push', $LocalPath, "$stageDir/$Name")
    Invoke-Adb @('shell', "mv '$stageDir/$Name' '$userDir/$Name' && chmod 0666 '$userDir/$Name'")
}
if ($Disc) { Push-UserFile $Disc 'disc.iso' }
if ($Settings) { Push-UserFile $Settings 'port_settings.ini' }

if ($NoLaunch) { return }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = Join-Path $root ".runs\$stamp"
New-Item -ItemType Directory -Force $runDir | Out-Null

& $adb logcat -c
Write-Host "Launching $activity for $Seconds s"
$startArgs = @('shell', 'am', 'start', '-n', $activity)
if ($BootWorld) { $startArgs += @('--es', 'MP_BOOT_WORLD', $BootWorld) }
Invoke-Adb $startArgs
Start-Sleep -Seconds $Seconds
$logcat = Join-Path $runDir 'logcat.txt'
# Windows PowerShell's '>' writes UTF-16; keep the capture greppable.
& $adb logcat -d -v time | Out-File -FilePath $logcat -Encoding utf8
Write-Host "logcat: $logcat"

# The game runs in its own process; the package's main process is the launcher panel.
$running = ((& $adb shell pidof "${package}:game") | Out-String).Trim()
if ($running) { Write-Host "Game process still running (pid $running)" } else { Write-Warning 'Game process is not running' }

foreach ($name in @('metroid_prime_port.log', 'last_error.txt')) {
    if (Test-DevicePath "$userDir/$name" 'f') {
        & $adb pull "$userDir/$name" (Join-Path $runDir $name) | Out-Null
        Write-Host "pulled $name"
    }
}

Write-Host '--- game log (metroidprime / aurora / stdout / SDL / PrimedGunVr / crashes) ---'
Select-String -Path $logcat -Pattern '[VDIWEF]/(metroidprime|aurora|stdout|SDL|PrimedGunVr|AndroidRuntime|DEBUG|libc) *\(' |
    Select-Object -Last 150 | ForEach-Object { $_.Line }
