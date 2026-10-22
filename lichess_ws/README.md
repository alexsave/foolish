# lichess_ws

The lichess backend rebuilt in C on c_base, one component at a time, in the order of the gain each brings (SCOPE.md section 2), each raced against the real component on the same machine (DESIGN.md).

The first component is lila-ws, lichess's WebSocket gateway: `server/` is a drop-in for it on the sockets lichess serves at volume (site, api, lobby, round), speaking the browser's protocol and lila's Redis protocol byte for byte (SCOPE.md section 3).

This is a clean-room implementation of the protocol from reading lila-ws and lila (AGPL-3.0); none of their source is copied or translated here, and if this code is ever offered to lichess it would be released under AGPL-3.0.
