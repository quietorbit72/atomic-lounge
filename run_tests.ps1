# SPDX-License-Identifier: AGPL-3.0-only
# Atomic Lounge: engine, ORBIT core, converter and page tests on Windows (the VST3 has its own: plugin\build.ps1 -Test).
#   powershell -ExecutionPolicy Bypass -File run_tests.ps1
# C compiler: $env:CC, else gcc on PATH or MSYS2 UCRT64, else clang from LLVM. Python 3 and node on PATH.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$cc = @($env:CC, (Get-Command gcc -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
        'C:\msys64\ucrt64\bin\gcc.exe', 'C:\Program Files\LLVM\bin\clang.exe') |
      Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $cc) { throw 'no C compiler (set CC to gcc or clang)' }
if ($cc -like '*msys64*') { $env:PATH = "$(Split-Path $cc);$env:PATH" }
Write-Host "C compiler: $cc"
New-Item -ItemType Directory -Force build | Out-Null
$dsp = Get-ChildItem dsp -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$flags = '-O2', '-std=c11', '-Wall', '-Wno-unused-parameter', '-Idsp/include'
$lm = if ($cc -like '*clang*') { @('-D_CRT_SECURE_NO_WARNINGS') } else { @('-lm') }
$targets = [ordered]@{
    'build/mfe_render.exe'   = @('tools/render/mfe_render.c')
    'build/test_dsp.exe'     = @('tests/dsp/test_dsp.c')
    'build/test_dsp_small.exe' = @('tests/dsp/test_dsp.c')   # with the small-memory effect sizes
    'build/test_seq.exe'     = @('tests/dsp/test_seq.c')
    'build/test_orbit.exe'   = @('tests/orbit/test_orbit.c')   # ORBIT core (dsp/orbit)
    'build/test_orbit_input.exe' = @('tests/orbit/test_orbit_input.c')   # ORBIT live input, parked puck
}
foreach ($out in $targets.Keys) {
    $argv = [System.Collections.Generic.List[string]]::new()
    $flags | ForEach-Object { $argv.Add($_) }
    if ($out -match 'orbit') { $argv.Add('-Idsp/orbit') }
    if ($out -match 'small') { '-DMFE_DELAY_LEN=12288', '-DMFE_OILCAN_RING=4096', '-DMFE_OILCAN_LITE=1' | ForEach-Object { $argv.Add($_) } }
    $argv.Add('-o'); $argv.Add($out)
    $targets[$out] + $dsp + $lm | ForEach-Object { $argv.Add($_) }
    & $cc $argv.ToArray()
    if ($LASTEXITCODE) { throw "compile failed: $out" }
}
& .\build\test_dsp.exe; if ($LASTEXITCODE) { throw 'engine tests failed' }
& .\build\test_dsp_small.exe; if ($LASTEXITCODE) { throw 'engine tests (small effect sizes) failed' }
& .\build\test_seq.exe; if ($LASTEXITCODE) { throw 'sequencer tests failed' }
& .\build\test_orbit.exe; if ($LASTEXITCODE) { throw 'ORBIT core tests failed' }
& .\build\test_orbit_input.exe; if ($LASTEXITCODE) { throw 'ORBIT input tests failed' }
& python -I tests/fixtures/make_fixtures.py | Out-Null
& python -I tests/run_py_tests.py; if ($LASTEXITCODE) { throw 'converter tests failed' }
& python -I tests/test_massive_mods.py; if ($LASTEXITCODE) { throw 'modulation decoder tests failed' }
& python -I tools/build_editor.py -o build/converter.html | Out-Null; if ($LASTEXITCODE) { throw 'editor build failed' }
if ((Get-FileHash build/converter.html).Hash -ne (Get-FileHash editor/converter.html).Hash) { throw 'editor/converter.html is stale: run python -I tools/build_editor.py' }
& node tests/test_editor.mjs; if ($LASTEXITCODE) { throw 'converter page tests failed' }
& node tests/test_panel.mjs; if ($LASTEXITCODE) { throw 'panel tests failed' }
Write-Host 'ALL TESTS PASSED'
