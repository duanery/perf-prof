#!/bin/bash
# Download latest python-build-standalone
#
# Package selection:
#   - Version: first argument or PYTHON_VERSION env var, default 3.12
#   - Architecture: second argument or auto-detected via uname -m (x86_64, aarch64)
#   - Variant: install_only_stripped — minimal runtime without debug symbols,
#     pre-installed layout (bin/ lib/ include/), ready to use directly
#   - Stability: alpha/beta/rc versions are filtered out, highest patch version selected
#
# Source: https://github.com/astral-sh/python-build-standalone
# Asset naming: cpython-{ver}+{tag}-{arch}-unknown-linux-gnu-install_only_stripped.tar.gz
#
# Usage:
#   ./download-python-standalone.sh [PYTHON_VERSION] [ARCH]
#
# Examples:
#   ./download-python-standalone.sh          # download latest stable (3.12.x) for current arch
#   ./download-python-standalone.sh 3.13     # download latest 3.13.x for current arch
#   ./download-python-standalone.sh 3.12 aarch64  # download 3.12.x for aarch64

set -euo pipefail

PYTHON_VERSION="${1:-${PYTHON_VERSION:-3.12}}"
ARCH="${2:-$(uname -m)}"
VARIANT="${ARCH}-unknown-linux-gnu-install_only_stripped"

echo "Fetching latest release info..."
RELEASE_JSON=$(curl -sfL https://raw.githubusercontent.com/astral-sh/python-build-standalone/latest-release/latest-release.json)
TAG=$(echo "$RELEASE_JSON" | python3 -c "import json,sys; print(json.load(sys.stdin)['tag'])")
PREFIX=$(echo "$RELEASE_JSON" | python3 -c "import json,sys; print(json.load(sys.stdin)['asset_url_prefix'])")

echo "Latest release tag: $TAG"

echo "Finding cpython-${PYTHON_VERSION}.x asset..."
ASSET_NAME=$(curl -sf "https://api.github.com/repos/astral-sh/python-build-standalone/releases/tags/${TAG}" \
  | python3 -c "
import json, sys, re
data = json.load(sys.stdin)
pattern = re.compile(r'^cpython-${PYTHON_VERSION}\.\d+(\+|\-).*${VARIANT}\.tar\.gz\$')
matches = [a['name'] for a in data['assets'] if pattern.match(a['name'])]
stable = [m for m in matches if not re.search(r'(a|b|rc)\d+', m)]
if stable:
    matches = stable
if matches:
    matches.sort(key=lambda x: [int(n) for n in re.search(r'cpython-(\d+)\.(\d+)\.(\d+)', x).groups()], reverse=True)
    print(matches[0])
else:
    sys.exit(1)
")

if [ -z "$ASSET_NAME" ]; then
    echo "ERROR: No matching asset found for cpython-${PYTHON_VERSION}.x ${VARIANT}" 1>&2
    exit 1
fi

if [ -f "$ASSET_NAME" ]; then
    echo "Already exists: $ASSET_NAME"
else
    echo "Downloading: $ASSET_NAME"
    curl -fL --progress-bar -O "${PREFIX}/${ASSET_NAME}"
fi

echo ""
echo "Downloaded: $ASSET_NAME"
echo "Extract with: tar xzf $ASSET_NAME"
