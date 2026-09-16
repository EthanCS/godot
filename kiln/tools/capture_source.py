#!/usr/bin/env python3
"""Build an isolated copy of the original 4.6.2 C# study, never the source tree.
Copies only allowlisted rendering code and island assets. UI/localization and
project admission are adapted; GI, material shaders and geometry remain original.
Output must be outside source. No secrets, caches, game state or Core dependency.
"""
import argparse,hashlib,json,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--godot',type=Path,default=Path('/Applications/Godot_mono.app/Contents/MacOS/Godot'));p.add_argument('--build-only',action='store_true');a=p.parse_args()
src=(a.source/'Game/OC.Client').resolve();dst=a.output.resolve();assert not dst.is_relative_to(a.source.resolve());dst.mkdir(parents=True,exist_ok=True)
patterns=['scripts/studies/IslandMaterialStudy*.cs','rendering/kiln_gi/*.cs','rendering/kiln_gi/*.tres','rendering/kiln_gi/shaders/*.glsl','rendering/xegtao/*.cs','rendering/shaders/main_island_surface.gdshader','rendering/shaders/surface.gdshaderinc','rendering/shaders/material_response.gdshaderinc','rendering/shaders/direct_light.gdshaderinc','rendering/sky/*','assets/materials/main_island/*.tres','assets/models/environments/main_island/*.glb','scenes/studies/main_island_direct_light.tscn']
manifest={}
for pattern in patterns:
 for f in src.glob(pattern):
  if not f.is_file() or f.suffix in ['.uid','.import']:continue
  target=dst/f.relative_to(src);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,target);manifest[str(f.relative_to(src))]=hashlib.sha256(f.read_bytes()).hexdigest()
(dst/'source-manifest.json').write_text(json.dumps({'commit':subprocess.check_output(['git','-C',str(a.source),'rev-parse','HEAD'],text=True).strip(),'files':manifest},indent=2))
(dst/'OC.Client.csproj').write_text('<Project Sdk="Godot.NET.Sdk/4.6.2"><PropertyGroup><TargetFramework>net10.0</TargetFramework><EnableDynamicLoading>true</EnableDynamicLoading><RootNamespace>OC.Client</RootNamespace></PropertyGroup></Project>')
(dst/'project.godot').write_text('''config_version=5
[application]
config/name="Original Kiln reference"
config/features=PackedStringArray("4.6", "C#", "Forward Plus")
run/main_scene="res://scenes/studies/main_island_direct_light.tscn"
[display]
window/size/viewport_width=1920
window/size/viewport_height=1080
[dotnet]
project/assembly_name="OC.Client"
[rendering]
renderer/rendering_method="forward_plus"
rendering_device/driver="metal"
''')
(dst/'ReferenceDependencies.cs').write_text('''using Godot;
namespace OC.Core.Framework { public static class GameIds { public const string LevelAmbientGroup="level_ambient"; } public static class LocaleService { public static string T(string key, params object[] args)=>key; } }
namespace OC.Client {
public static class QualitySettings { public sealed class Graphics { public int Ao=3; } public static Graphics Current=new(); }
public static class ProjectRendering {
 public static bool IsGiMaterial(ShaderMaterial m)=>m.Shader?.ResourcePath=="res://rendering/shaders/main_island_surface.gdshader";
 public static void DisableNative(Godot.Environment e) { if(e==null)return; e.SdfgiEnabled=e.SsilEnabled=e.SsaoEnabled=e.SsrEnabled=false;e.AmbientLightEnergy=0;e.ReflectedLightSource=Godot.Environment.ReflectionSource.Disabled; }
}}
''')
# Dedicated capture driver omits unrelated original interactive/zero-first-frame
# assertions. Record this adaptation; do not label it a full source test pass.
study=dst/'scripts/studies/IslandMaterialStudy.cs';s=study.read_text();s=s.replace('RefreshStatus();\n        if (OS.GetCmdlineUserArgs()', 'RefreshStatus();\n        _ = CaptureReferenceOnly(); return;\n        if (OS.GetCmdlineUserArgs()',1);study.write_text(s)
(dst/'scripts/studies/IslandMaterialStudy.Reference.cs').write_text('''using System.IO; using System.Threading.Tasks; using Godot;
namespace OC.Client; public partial class IslandMaterialStudy {
async Task CaptureReferenceOnly() {
_capturing=true; _hud.Visible=false; GetWindow().Size=new(1920,1080); GetViewport().Msaa3D=Viewport.Msaa.Disabled; GetViewport().UseTaa=false;
try { for(int i=0;i<5;i++) { SetView(i); _giEnabled=true; _giDebug=0; await GiSettle(280); await GiShot($"reference_{i}_lit"); _giEnabled=false; await GiSettle(12); await GiShot($"reference_{i}_direct"); _giEnabled=true; _onlyIndirect=true; await GiSettle(280); await GiShot($"reference_{i}_indirect"); _onlyIndirect=false; SetDisplay(5); await GiSettle(2); await GiShot($"reference_{i}_albedo"); SetDisplay(0); } GD.Print("[KILN_REFERENCE] five views captured; no AA; exposure 1; 256 samples; source sky model"); GetTree().Quit(); }
catch(System.Exception e) { GD.PushError(e.ToString()); GetTree().Quit(1); }
}}
''')
# Canonical /private/tmp avoids incorrect ScriptPath attributes from /tmp symlinks.
subprocess.run(['dotnet','build',str(dst/'OC.Client.csproj'),'-t:Rebuild'],check=True,cwd=dst)
subprocess.run([str(a.godot),'--headless','--path',str(dst),'--editor','--import','--quit'],check=True,cwd=dst)
if not a.build_only:subprocess.run([str(a.godot),'--path',str(dst)],check=True,cwd=dst)
