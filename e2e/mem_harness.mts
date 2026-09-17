// Ad-hoc harness (not part of the suite): plays full bot-vs-bot games through
// the production path - tables of bots on the kernel's bot cycle, as the
// server's loop drives them (e2e/helpers/bot_table.ts) - and reports process RSS
// growth, to verify the endgame solvers' wasm heap footprint.
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/mem_harness.mts

const rssMB = () => Math.round(process.memoryUsage().rss / 1048576);

// Capture every wasm memory as modules instantiate so we can report the real
// (virtual) footprint the edge runtime bills against, not just touched RSS.
const memories: WebAssembly.Memory[] = [];
const RealInstance = WebAssembly.Instance;
(WebAssembly as any).Instance = function (mod: WebAssembly.Module, imports?: WebAssembly.Imports) {
    const inst = new RealInstance(mod, imports);
    const m = (inst.exports as any).memory;
    if (m instanceof WebAssembly.Memory) memories.push(m);
    return inst;
} as any;
(WebAssembly as any).Instance.prototype = RealInstance.prototype;
const wasmMB = () => memories.map(m => Math.round(m.buffer.byteLength / 1048576)).join('+');

// Imported after the hook, so the table's bots.wasm instance is captured.
const { playBotTable, seedBytes } = await import('./helpers/bot_table.ts');

console.log('start rss:', rssMB(), 'MB');
const matchups: string[][] = [['cordite', 'octogen'], ['blackpowder', 'firecracker'], ['octogen', 'blackpowder']];
for (const [i, brains] of matchups.entries()) {
    const g = playBotTable(brains, seedBytes(brains.length, i), { gameId: `mem-${brains.join('-')}` });
    console.log(`${brains.join(' vs ')}: done=${g.fool >= 0} moves=${g.actions} rss=${rssMB()}MB wasm=${wasmMB()}MB`);
}
console.log('final rss:', rssMB(), 'MB');
