param([switch]$ForwardPlus, [switch]$NoGI, [switch]$MultiLight)
$engine = Join-Path $PSScriptRoot '../../bin/godot.windows.editor.x86_64.mono.console.exe'
$method = if ($ForwardPlus) { 'forward_plus' } else { 'kiln_deferred' }
$demoArgs = @('--path', $PSScriptRoot, '--rendering-driver', 'vulkan', '--rendering-method', $method, '--')
if ($MultiLight) { $demoArgs += '--multi-light' }
if ($NoGI) { $demoArgs += '--no-gi' }
& $engine @demoArgs
