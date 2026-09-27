// The bots-module exports the /m/ link preview reads, and only those.
//
// src/app/m/[payload]/layout.tsx imports the kernel lazily. A dynamic import
// keeps EVERY export of its target alive in a bundle, whatever the importer
// uses - pointed at bots.ts itself, it kept the server bot drive (wasmBotDrive)
// and, through it, the unmasked state export. Pointed here, the bundle keeps
// these three. e2e/security_client_boundary.test.ts pins it.
export { ensureBotsAsync, kernelMsgDecode, kernelB32Decode } from '@sdk/ts/wasm/bots.ts';
