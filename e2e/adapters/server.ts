// The deno.land std `serve` the edge entry points call at module load. There is
// no HTTP server in the harness: `serve` records the handler instead, so a test
// can import a real `functions/<name>/index.ts` and call the exact handler the
// platform would, with a real Request (see e2e/helpers/edge.ts).
export type ServedHandler = (req: Request) => Promise<Response>;
export const servedHandlers: ServedHandler[] = [];
export const serve = (handler: ServedHandler) => { servedHandlers.push(handler); };
