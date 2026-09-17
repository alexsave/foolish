// The engine exports decodeReplay reads, and only those.
//
// decode.ts loads the engine lazily, so the replay screen's chunk (not every
// page) carries the wasm. A dynamic import keeps EVERY export of its target
// alive in a bundle, whatever the importer uses - pointed at engine.ts itself,
// it shipped the server's unmasked state readers (deserializeGameState,
// runPackedAction, materializeKernelGame, ...) to the browser. Pointed here, the
// bundle keeps these four. e2e/security_client_boundary.test.ts pins it.
export { ensureEngineAsync, kernelReplayDecode, __LOG_TYPE_FROM_INT, __cardFromWire } from '@sdk/ts/wasm/engine.ts';
