#!/usr/bin/env python3
"""Export a self-contained local macOS app using the custom release template.
No signing, notarization or publication. Requires prior template_release build.
"""
import argparse,plistlib,shutil,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();app=a.output.resolve()/'KilnIsland.app';mac=app/'Contents/MacOS';resources=app/'Contents/Resources';mac.mkdir(parents=True,exist_ok=True);resources.mkdir(parents=True,exist_ok=True)
engine=root/'bin/godot.macos.editor.arm64';template=root/'bin/godot.macos.template_release.arm64'
subprocess.run([str(engine),'--headless','--path',str(root/'kiln/demo'),'--editor','--import','--quit'],check=True)
subprocess.run([str(engine),'--headless','--path',str(root/'kiln/demo'),'--export-pack','macOS Kiln Pack',str(resources/'KilnIsland.pck')],check=True)
shutil.copy2(template,mac/'KilnIsland');shutil.copy2(root/'LICENSE.txt',resources/'GODOT-LICENSE.txt')
(app/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':'KilnIsland','CFBundleIdentifier':'org.kiln.island','CFBundleName':'KilnIsland','CFBundlePackageType':'APPL','CFBundleVersion':'0.1','NSHighResolutionCapable':True}))
print(app)
