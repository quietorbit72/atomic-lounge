# SPDX-License-Identifier: AGPL-3.0-only
# Atomic Lounge (formerly Vintage Chonk) VST3 (Windows x64): configure + build with the Visual Studio 2022 Build Tools' own CMake
# and Ninja, Release. Output: plugin\dist\Atomic Lounge.vst3 (a VST3 bundle folder). Nothing is installed.
# Runs plugin\fetch_deps.ps1 first (JUCE + WebView2 SDK at pinned, checked versions; the JUCE patch).
#   powershell -ExecutionPolicy Bypass -File plugin\build.ps1            # plugin
#   powershell -ExecutionPolicy Bypass -File plugin\build.ps1 -Test      # plugin + host test, then run it
#   powershell -ExecutionPolicy Bypass -File plugin\build.ps1 -Clean     # fresh configure
#   powershell -ExecutionPolicy Bypass -File plugin\build.ps1 -Dev -Test # development build: plugin\build-dev ->
#                                                                         plugin\dist-dev (the release dist is untouched)
# clang-cl (for the engine's C): $env:MFE_CLANG_CL, else C:\Program Files\LLVM\bin, else an Android NDK's
# (under $env:MFE_NDK_DIR, default D:\Android\sdk\ndk).
param([switch]$Test, [switch]$Clean, [switch]$NoScreenshot, [switch]$Dev)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path $here -Parent
$name = 'Atomic Lounge'     # PRODUCT_NAME (formerly Vintage Chonk; the plugin IDs are unchanged)
$bt = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
$cmake = "$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$vcvars = "$bt\VC\Auxiliary\Build\vcvars64.bat"

& "$here\fetch_deps.ps1"
foreach ($f in @($cmake, $ninja, $vcvars, "$repo\third_party\JUCE\CMakeLists.txt",
                 "$repo\third_party\webview2\x64\WebView2LoaderStatic.lib", "$repo\third_party\webview2\include\WebView2.h")) {
    if (-not (Test-Path $f)) { throw "missing: $f" }
}

# the MSVC environment (cl, link, Windows SDK 10.0.26100) from vcvars64.bat
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'vcvars64.bat did not set up cl.exe' }

# factory bank for the host programs: regenerated from the panel's factory.js when node is here
if (Get-Command node -ErrorAction SilentlyContinue) { node "$here\tools\gen_factory.mjs"; if ($LASTEXITCODE) { throw 'gen_factory failed' } }

# The engine's C (dsp/) uses GCC/Clang vector extensions (the Chroma spring tank), which MSVC's C
# compiler does not accept; dsp/*.c is compiled by clang-cl (MSVC ABI and runtime, linked by MSVC's link).
# C++ (JUCE, the plugin) stays on MSVC cl.
$ndk = if ($env:MFE_NDK_DIR) { $env:MFE_NDK_DIR } else { 'D:\Android\sdk\ndk' }
$cands = @()
if ($env:MFE_CLANG_CL) { $cands += $env:MFE_CLANG_CL }
if (Test-Path $ndk) {
    $cands += @(Get-ChildItem $ndk -Recurse -Filter 'clang-cl.exe' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
}
$cands += 'C:\Program Files\LLVM\bin\clang-cl.exe'
$clangcl = $cands | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $clangcl) { throw 'clang-cl.exe not found: install LLVM (C:\Program Files\LLVM) or set MFE_CLANG_CL' }
Write-Host "C compiler (engine): $clangcl"
$clangclFwd = $clangcl -replace '\\', '/'

$build = if ($Dev) { "$here\build-dev" } else { "$here\build" }
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
# JUCE writes the Windows version resource only on the first configure; drop stale ones so name/version follow CMakeLists.txt
Get-ChildItem $build -Recurse -Filter '*_resources.rc' -ErrorAction SilentlyContinue | Remove-Item -Force
& $cmake -S $here -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DMFE_CLANG_CL=$clangclFwd"
if ($LASTEXITCODE) { throw 'configure failed' }
& $cmake --build $build --target VintageChonk_VST3 VintageChonk_Standalone --parallel
if ($LASTEXITCODE) { throw 'build failed' }

$bundle = Get-ChildItem "$build\VintageChonk_artefacts" -Recurse -Directory -Filter "$name.vst3" | Select-Object -First 1
if (-not $bundle) { throw "no $name.vst3 in the build" }
$dist = if ($Dev) { "$here\dist-dev" } else { "$here\dist" }
New-Item -ItemType Directory -Force $dist | Out-Null
if (Test-Path "$dist\$name.vst3") { Remove-Item -Recurse -Force "$dist\$name.vst3" }
Copy-Item -Recurse $bundle.FullName "$dist\$name.vst3"
$dll = Get-ChildItem "$dist\$name.vst3" -Recurse -Filter '*.vst3' -File | Select-Object -First 1
$standalone = Get-ChildItem "$build\VintageChonk_artefacts" -Recurse -File -Filter "$name.exe" | Select-Object -First 1
if ($standalone) { Copy-Item -Force $standalone.FullName "$dist\$name.exe" }
Write-Host ("built: {0} ({1:N0} bytes)" -f $dll.FullName, $dll.Length)

if ($Test) {
    & $cmake --build $build --target VintageChonkHostTest --parallel
    if ($LASTEXITCODE) { throw 'host test build failed' }
    $exe = Get-ChildItem "$build\VintageChonkHostTest_artefacts" -Recurse -Filter 'VintageChonkHostTest.exe' | Select-Object -First 1
    $targs = @("$dist\$name.vst3", "$build\test_out")
    if ($NoScreenshot) { $targs += '--no-screenshot' }
    & $exe.FullName @targs
    if ($LASTEXITCODE) { throw "host test failed ($LASTEXITCODE)" }
}

Write-Host ''
Write-Host 'Not installed. To install for every DAW on this PC, run in an ADMIN PowerShell:'
Write-Host "  Copy-Item -Recurse -Force `"$dist\$name.vst3`" `"C:\Program Files\Common Files\VST3\`""
