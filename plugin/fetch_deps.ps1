# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk VST3: fetch the third-party code the plugin builds against, at pinned versions, checked by
# commit / SHA-256, into third_party/ (nothing of it is in this repository):
#   * JUCE 8.0.15 (AGPLv3)            git tag 8.0.15, commit checked; plugin/patches/*.patch applied
#   * Microsoft WebView2 SDK          NuGet Microsoft.Web.WebView2 1.0.4314.35 (BSD-3-Clause): the .nupkg's
#                                     SHA-256 and the extracted header / static loader SHA-256 are checked
#   * Steinberg VST3 SDK (optional)   -Validator: tag v3.8.1_build_84 (MIT) + the submodules the validator
#                                     needs; builds validator.exe (Steinberg's plugin test host)
# Downloads go to third_party/_downloads/<name>/ first. Safe to run again: whatever is present and correct
# is kept, so after the first run it needs no network.
#   powershell -ExecutionPolicy Bypass -File plugin\fetch_deps.ps1              # JUCE + WebView2 SDK
#   powershell -ExecutionPolicy Bypass -File plugin\fetch_deps.ps1 -Validator   # + VST3 SDK validator
param([switch]$Validator)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path $here -Parent
$tp = Join-Path $repo 'third_party'
$dl = Join-Path $tp '_downloads'
New-Item -ItemType Directory -Force $dl | Out-Null

$JUCE_TAG = '8.0.15'
$JUCE_COMMIT = '91ad83ae34a81e0833b1a2b0866f54846370ae53'
$WV2_VERSION = '1.0.4314.35'
$WV2_NUPKG_SHA256 = '90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4'
$WV2_FILES = [ordered]@{
    'include/WebView2.h'                   = @('build/native/include/WebView2.h', '4e47763725f507235e8a12f35b9f41d7e348586153414c39fc1e9108875c181f')
    'include/WebView2EnvironmentOptions.h' = @('build/native/include/WebView2EnvironmentOptions.h', '25a2de86c15fdc2fbb0e5d9cb01f17cadb8accc7ed4a5a41e875a5e7b866c991')
    'x64/WebView2LoaderStatic.lib'         = @('build/native/x64/WebView2LoaderStatic.lib', '6d03a1fcc7cd3d34f221d77e7630f436c64fdbdc6ce77ec714dce7bcdb37731f')
    'LICENSE.txt'                          = @('LICENSE.txt', '0af8f1b807512aae39c2ac1aa4d0cae65cabecb6fd554b8439a5162a0d6eca55')
}
$VST3_TAG = 'v3.8.1_build_84'
$VST3_COMMIT = '3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96'
$VST3_SUBMODULES = [ordered]@{   # the validator needs these four; doc, tutorials and vstgui4 are not fetched
    'base'           = 'fcf9da0bd27a16f7f03773a3a39822f28f5c8477'
    'cmake'          = '054c9143cbb8d47fc4694e473f2ee3b4d951a8f5'
    'pluginterfaces' = '4f547e8e102b47de4a8b8aaf343c73b700786372'
    'public.sdk'     = '586dc5e6c8012c3e4b01c79389375cbe96bdb1da'
}

function Sha256([string]$f) { (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash.ToLowerInvariant() }
function GitRun { & git.exe @args; if ($LASTEXITCODE) { throw "git $($args -join ' ') failed" } }

# ------------------------------------------------------------------------------------------- JUCE ---
$juce = Join-Path $tp 'JUCE'
if (-not (Test-Path (Join-Path $juce 'CMakeLists.txt'))) {
    Write-Host "JUCE ${JUCE_TAG}: cloning"
    GitRun -c advice.detachedHead=false -c core.longpaths=true clone --quiet --depth 1 --branch $JUCE_TAG https://github.com/juce-framework/JUCE $juce
}
$head = (& git -C $juce rev-parse HEAD).Trim()
if ($head -ne $JUCE_COMMIT) { throw "third_party/JUCE is at $head, expected $JUCE_COMMIT (tag $JUCE_TAG)" }
Write-Host "JUCE $JUCE_TAG at $head (checked)"

# patches: each is a one-file unified diff; its '-' lines are replaced by its '+' lines (line-ending
# agnostic, so it works whatever git's autocrlf did). Already patched = left alone.
foreach ($patch in Get-ChildItem (Join-Path $here 'patches') -Filter '*.patch' | Sort-Object Name) {
    $lines = Get-Content -LiteralPath $patch.FullName -Encoding UTF8
    $target = ($lines | Where-Object { $_ -like '+++ b/*' } | Select-Object -First 1).Substring(6)
    $old = @(); $new = @()
    foreach ($l in $lines) {
        if ($l -like '---*' -or $l -like '+++*') { continue }
        if ($l.StartsWith('-')) { $old += $l.Substring(1).TrimEnd("`r") }
        elseif ($l.StartsWith('+')) { $new += $l.Substring(1).TrimEnd("`r") }
    }
    $file = Join-Path $juce $target
    $text = [IO.File]::ReadAllText($file)
    $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $o = $old -join $nl; $n = $new -join $nl
    $count = ([regex]::Matches($text, [regex]::Escape($o))).Count
    if ($text.Contains($n) -and $count -eq 0) { Write-Host "patch $($patch.Name): already applied"; continue }
    if ($count -ne 1) { throw "patch $($patch.Name): expected the original text once in $target, found $count" }
    [IO.File]::WriteAllText($file, $text.Replace($o, $n), (New-Object Text.UTF8Encoding($false)))
    Write-Host "patch $($patch.Name): applied to $target"
}

# ------------------------------------------------------------------------------------ WebView2 SDK ---
$wv2 = Join-Path $tp 'webview2'
$ok = $true
foreach ($k in $WV2_FILES.Keys) {
    $f = Join-Path $wv2 $k
    if (-not (Test-Path $f) -or (Sha256 $f) -ne $WV2_FILES[$k][1]) { $ok = $false }
}
if ($ok) { Write-Host "WebView2 SDK $WV2_VERSION present (checked)" }
else {
    $d = Join-Path $dl "webview2-$WV2_VERSION"
    New-Item -ItemType Directory -Force $d | Out-Null
    $pkg = Join-Path $d "microsoft.web.webview2.$WV2_VERSION.nupkg"
    if (-not (Test-Path $pkg) -or (Sha256 $pkg) -ne $WV2_NUPKG_SHA256) {
        $url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$WV2_VERSION/microsoft.web.webview2.$WV2_VERSION.nupkg"
        Write-Host "WebView2 SDK ${WV2_VERSION}: downloading $url"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $pkg
    }
    $h = Sha256 $pkg
    if ($h -ne $WV2_NUPKG_SHA256) { throw "WebView2 .nupkg SHA-256 $h, expected $WV2_NUPKG_SHA256" }
    $x = Join-Path $d 'pkg'
    if (Test-Path $x) { Remove-Item -Recurse -Force $x }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($pkg, $x)
    foreach ($k in $WV2_FILES.Keys) {
        $src = Join-Path $x $WV2_FILES[$k][0]
        if ((Sha256 $src) -ne $WV2_FILES[$k][1]) { throw "WebView2 SDK: $($WV2_FILES[$k][0]) does not match its pinned SHA-256" }
        $dst = Join-Path $wv2 $k
        New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
        Copy-Item -Force $src $dst
    }
    Set-Content -Encoding ASCII (Join-Path $wv2 'VERSION.txt') "Microsoft.Web.WebView2 $WV2_VERSION (nuget.org), .nupkg SHA-256 $WV2_NUPKG_SHA256"
    Write-Host "WebView2 SDK $WV2_VERSION installed in third_party/webview2 (checked)"
}

# ---------------------------------------------------------------------- VST3 SDK validator (optional) ---
if ($Validator) {
    $v = Join-Path $dl "vst3sdk-$VST3_TAG"
    $src = Join-Path $v 'src'
    if (-not (Test-Path (Join-Path $src 'CMakeLists.txt'))) {
        New-Item -ItemType Directory -Force $v | Out-Null
        Write-Host "VST3 SDK ${VST3_TAG}: cloning"
        GitRun -c advice.detachedHead=false -c core.longpaths=true clone --quiet --depth 1 --branch $VST3_TAG https://github.com/steinbergmedia/vst3sdk $src
        GitRun -C $src submodule update --init --depth 1 @($VST3_SUBMODULES.Keys)
    }
    $head = (& git -C $src rev-parse HEAD).Trim()
    if ($head -ne $VST3_COMMIT) { throw "VST3 SDK at $head, expected $VST3_COMMIT ($VST3_TAG)" }
    foreach ($m in $VST3_SUBMODULES.Keys) {
        $c = (& git -C (Join-Path $src $m) rev-parse HEAD).Trim()
        if ($c -ne $VST3_SUBMODULES[$m]) { throw "VST3 SDK submodule $m at $c, expected $($VST3_SUBMODULES[$m])" }
    }
    Write-Host "VST3 SDK $VST3_TAG at $head, submodules checked"
    $exe = Join-Path $v 'build\bin\validator.exe'
    if (-not (Test-Path $exe)) {
        $bt = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
        $cmake = "$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        $ninja = "$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
        cmd /c "`"$bt\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
        }
        # examples, VSTGUI, plug-in links into Program Files and the auto-run validator are all off
        & $cmake -S $src -B (Join-Path $v 'build') -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release `
            -DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_CREATE_PLUGIN_LINK=OFF `
            -DSMTG_RUN_VST_VALIDATOR=OFF -DSMTG_PLUGIN_TARGET_USER_PROGRAM_FILES_COMMON=OFF
        if ($LASTEXITCODE) { throw 'VST3 SDK configure failed' }
        & $cmake --build (Join-Path $v 'build') --target validator --parallel
        if ($LASTEXITCODE) { throw 'validator build failed' }
    }
    Write-Host "validator: $exe"
}
