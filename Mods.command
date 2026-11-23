#!/bin/bash
# Double-click in Finder (or run from a terminal) to choose mods and build.
cd "$(dirname "$0")" && exec python3 tools/mods.py
