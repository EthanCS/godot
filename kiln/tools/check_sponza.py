#!/usr/bin/env python3
"""Compatibility entry point for the current Surfel GI capture validator."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).with_name("check_surfel.py")),run_name="__main__")
