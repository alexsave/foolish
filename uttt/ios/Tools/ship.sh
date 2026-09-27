#!/bin/bash
# Ship UTTT to TestFlight - see shared/tools/ship/ship.sh for options and traps.
exec "$(dirname "$0")/../../../shared/tools/ship/ship.sh" "$(dirname "$0")/ship.env" "$@"
