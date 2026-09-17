#!/usr/bin/env python3
"""Fetch and build the pinned, separately licensed NRD SDK outside tracked source."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
COMMIT = "792eff196afdd350fd9c3f862119017ccb438a0e"
parser = argparse.ArgumentParser()
parser.add_argument("--sdk", type=Path, default=ROOT / "bin/build_deps/NRD")
args = parser.parse_args()
sdk = args.sdk.resolve()


def run(*command):
    subprocess.run(list(map(str, command)), check=True)


if not sdk.exists():
    run("git", "clone", "--branch", "v4.17.3", "--depth", "1", "https://github.com/NVIDIA-RTX/NRD.git", sdk)
head = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
if head != COMMIT:
    raise SystemExit(f"Expected NRD v4.17.3 at {COMMIT}, found {head}. Select the pinned revision explicitly.")
build = sdk / "_Build417"
options = ["-G", "Visual Studio 17 2022", "-A", "x64", "-DNRD_STATIC_LIBRARY=ON",
           "-DNRD_EMBEDS_DXIL_SHADERS=OFF", "-DNRD_EMBEDS_DXBC_SHADERS=OFF",
           "-DNRD_NORMAL_ENCODING=2", "-DNRD_ROUGHNESS_ENCODING=1",
           "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", "-DNRD_SUPPORTS_CHECKERBOARD=OFF"]
run("cmake", "-S", sdk, "-B", build, *options)
# Use the compiler version fetched by this release, not an unrelated Vulkan SDK DXC.
run("cmake", "-S", sdk, "-B", build,
    f"-DSHADERMAKE_DXC_VK_PATH={(build / '_deps/dxc-src/bin/x64/dxc.exe').as_posix()}")
run("cmake", "--build", build, "--config", "Release", "-j", "16")
(sdk / "kiln-build.json").write_text(json.dumps({"version": "4.17.3", "commit": COMMIT,
    "normal_encoding": 2, "roughness_encoding": 1, "backend": "SPIR-V/Vulkan",
    "license": "NVIDIA RTX SDKs LICENSE", "license_file": str(sdk / "LICENSE.txt")}, indent=2))
print("NRD built. Its separate NVIDIA license applies; no SDK source was added to the engine repository.")
