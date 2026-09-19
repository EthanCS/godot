param([switch]$ForwardPlus, [switch]$NoGI, [switch]$MultiLight, [switch]$DryFloor, [switch]$Still, [ValidateRange(0.02, 1.0)][float]$Roughness = 0.18, [ValidateRange(0, 28)][int]$DebugView = 0)
$engine = Join-Path $PSScriptRoot '../../bin/godot.windows.editor.x86_64.mono.console.exe'
$method = if ($ForwardPlus) { 'forward_plus' } else { 'kiln_deferred' }
$demoArgs = @('--path', $PSScriptRoot, '--rendering-driver', 'vulkan', '--rendering-method', $method, '--')
$demoArgs += '--roughness=' + $Roughness.ToString([System.Globalization.CultureInfo]::InvariantCulture)
if ($MultiLight) { $demoArgs += '--multi-light' }
if ($NoGI) { $demoArgs += '--no-gi' }
if ($DryFloor) { $demoArgs += '--dry-floor' }
if ($Still) { $demoArgs += '--still' }
if ($DebugView -ne 0) { $demoArgs += "--debug-view=$DebugView" }
& $engine @demoArgs
