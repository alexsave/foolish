#!/bin/bash
# WARNING: the historical sync below is disabled: it clobbers src/common with
# the server versions, whose relative imports end in ".ts" and which have
# drifted from the hand-maintained client copies (e.g. PersonalGame support in
# get_next_player_index). Reconcile the copies before re-enabling.
# cp supabase/functions/_shared/{common_utils.ts,constants.ts,types.ts} src/common/

# Replay codec: supabase/functions/_shared/replay is canonical; src/replay is
# generated. Only the genuinely shared pieces are copied to the client —
# codec.ts (kernel, self-contained), core.ts (the public-state replayer both
# directions run) and decode.ts (replay screen). encode.ts is server-only and
# is intentionally NOT copied. The sed rewrites Deno-style ".ts" import
# specifiers for the bundler.
fix_imports() {
  sed -e 's|from "\.\./types\.ts"|from "../common/types"|' \
      -e 's|from "\.\./constants\.ts"|from "../common/constants"|' \
      -e 's|from "\.\./common_utils\.ts"|from "../common/common_utils"|' \
      -e 's|from "\./codec\.ts"|from "./codec"|' \
      -e 's|from "\./core\.ts"|from "./core"|' \
      "$1"
}
cp supabase/functions/_shared/replay/codec.ts src/replay/codec.ts
fix_imports supabase/functions/_shared/replay/core.ts > src/replay/core.ts
fix_imports supabase/functions/_shared/replay/decode.ts > src/replay/decode.ts
