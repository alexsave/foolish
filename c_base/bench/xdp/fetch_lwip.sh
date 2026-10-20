#!/bin/bash
# fetch_lwip.sh : download the pinned lwIP release (BSD licence) next to build.sh and check its sha256.
set -euo pipefail
D=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
curl -fsSL -o "$D/lwip-2_2_1.tar.gz" https://github.com/lwip-tcpip/lwip/archive/refs/tags/STABLE-2_2_1_RELEASE.tar.gz
echo "ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539  $D/lwip-2_2_1.tar.gz" | shasum -a 256 -c -
