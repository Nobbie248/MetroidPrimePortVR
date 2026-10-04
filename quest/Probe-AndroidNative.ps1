# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Cross-compiles the port's native library for the Quest (arm64-v8a) with CMake alone.

.DESCRIPTION
The Gradle build in this folder runs the same CMake project, but it takes minutes to
reach the first native error and hides it in a long log. This probe configures and
builds one target straight from the NDK toolchain file, which is the quickest way to
see whether the native side compiles and links on this Windows host.

The arguments mirror app/build.gradle.kts. Keep the two lists in step.

.PARAMETER OpenXR
Builds the OpenXR (Vulkan) VR path. Without it the build is the plain Android port.

.PARAMETER NodStub
Links the non-functional nod stub instead of building nod with Rust. The result
cannot mount a disc; it isolates the Rust/Corrosion part of the build.

.EXAMPLE
quest\Probe-AndroidNative.ps1 -NodStub
quest\Probe-AndroidNative.ps1 -OpenXR
#>
param(
    [string]$BuildDir = "build/android-probe",
    [switch]$OpenXR,
    [switch]$NodStub,
    [switch]$Fresh,
    [string]$Target = "metroid_prime_port",
    [int]$Jobs = 12
)

$ErrorActionPreference = "Stop"

$ndkVersion = "29.0.14206865"
$minSdk = 29

$repo = Split-Path -Parent $PSScriptRoot

$sdk = $env:ANDROID_HOME
if (-not $sdk) { $sdk = $env:ANDROID_SDK_ROOT }
if (-not $sdk) { $sdk = Join-Path $env:LOCALAPPDATA "Android\Sdk" }
if (-not (Test-Path $sdk)) { throw "Android SDK not found (set ANDROID_HOME)." }

$ndk = Join-Path $sdk "ndk\$ndkVersion"
if (-not (Test-Path $ndk)) { throw "NDK $ndkVersion not found under $sdk\ndk." }
$toolchain = Join-Path $ndk "build\cmake\android.toolchain.cmake"

# The SDK's CMake folders carry a Ninja; system CMake does not.
$ninja = Get-ChildItem (Join-Path $sdk "cmake") -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName "bin\ninja.exe" } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if (-not $ninja) { throw "No ninja.exe under $sdk\cmake\*\bin." }

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { throw "cmake is not on PATH." }

if (-not $NodStub) {
    if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
        throw "cargo is not on PATH; nod is built with Rust (or pass -NodStub)."
    }
    $targets = & rustup target list --installed 2>$null
    if ($targets -notcontains "aarch64-linux-android") {
        throw "Rust target aarch64-linux-android is missing: rustup target add aarch64-linux-android"
    }
}

$configure = @(
    "-S", $repo,
    "-B", (Join-Path $repo $BuildDir),
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    "-DANDROID_ABI=arm64-v8a",
    "-DANDROID_PLATFORM=android-$minSdk",
    "-DANDROID_STL=c++_static",
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
    "-DMP_BUILD_TESTS=OFF",
    # OpenSSL's Android build needs make and a Unix PATH; Archipelago wss://
    # and the Remastered NSP import are left out of the Quest build.
    "-DMP_ALLOW_NO_TLS=ON",
    "-DAURORA_SDL3_PROVIDER=vendor",
    "-DAURORA_SDL3_LINKAGE=static",
    "-DAURORA_DAWN_PROVIDER=auto",
    "-DAURORA_DAWN_LINKAGE=static",
    "-DAURORA_NOD_PROVIDER=vendor",
    "-DAURORA_NOD_LINKAGE=static",
    "-DMP_ENABLE_OPENXR=$(if ($OpenXR) { 'ON' } else { 'OFF' })"
)
if ($NodStub) {
    $configure += "-DMP_ANDROID_NOD_STUB=ON"
} else {
    $configure += "-DMP_ANDROID_NOD_STUB=OFF"
    $configure += "-DRust_CARGO_TARGET=aarch64-linux-android"
}
if ($Fresh) { $configure = @("--fresh") + $configure }

Write-Host "cmake $($configure -join ' ')"
& $cmake @configure
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

& $cmake --build (Join-Path $repo $BuildDir) --target $Target -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$library = Get-ChildItem (Join-Path $repo $BuildDir) -Recurse -Filter "lib$Target.so" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($library) {
    Write-Host ("built {0} ({1:N1} MB)" -f $library.FullName, ($library.Length / 1MB))
}
