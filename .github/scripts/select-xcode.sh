#!/usr/bin/env bash
# Selects the newest released Xcode installed on the macOS image (betas and
# release candidates carry a suffix in their name and are skipped).
set -euo pipefail
xcode=$(ls -d /Applications/Xcode_*.app \
        | grep -E '/Xcode_[0-9]+(\.[0-9]+)*\.app$' \
        | sort -V | tail -1)
[ -n "$xcode" ] || { echo "::error::no released Xcode found"; exit 1; }
sudo xcode-select -s "$xcode"
echo "macOS $(sw_vers -productVersion) | $(xcodebuild -version | head -1) | $(clang++ --version | head -1)"
