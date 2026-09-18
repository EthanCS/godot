# Cornell box (kajiya parity workload) runner.
#   .\run.ps1 -- --frames=256 --output=captures\default
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot   # kiln/
$repo = Split-Path -Parent $root           # repo root with bin/

$godot = Join-Path $repo 'bin\godot.windows.editor.x86_64.mono.console.exe'
if (-not (Test-Path $godot)) { $godot = Join-Path $repo 'bin\godot.windows.editor.x86_64.mono.exe' }

& $godot --path (Join-Path $root 'cornell') --rendering-driver vulkan --rendering-method kiln_deferred @args
exit $LASTEXITCODE
