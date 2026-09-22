// ws_phoenix.ts - a WebSocket server and the Phoenix channel envelope, by hand.
//
// The fake Supabase (e2e/fake_supabase.mts) needs one thing Node does not ship
// and this repo has no dependency for: a WebSocket SERVER. Realtime is the only
// way another player's move reaches a browser, so a backend that cannot push is
// not a backend. This is RFC 6455 for the one case that matters - a browser
// client, text frames, no extensions, no compression - and the Phoenix envelope
// @supabase/realtime-js speaks on top of it.
//
// THE WIRE, as realtime-js 2.x writes and reads it (its lib/serializer.js, vsn
// 2.0.0): every message is a JSON array
//
//     [join_ref, ref, topic, event, payload]
//
// A join is `phx_join` and must be answered on the same ref with `phx_reply`
// and {status, response}; a channel carrying postgres_changes bindings must get
// its filters echoed back with ids or realtime-js errors the channel
// ("mismatch between server and client bindings"). The socket's own keepalive
// is `heartbeat` on the topic "phoenix" and must be answered or the client
// tears the socket down after one interval. A broadcast the server originates
// is that same array with a null join_ref and ref, event "broadcast", and a
// payload of {type, event, payload} - the shape RealtimeChannel hands a
// .on('broadcast', { event }, cb) callback.
//
// Nothing here knows anything about the game: it moves JSON between sockets and
// a hub. The backend decides what to send.

import { createHash, randomUUID } from 'node:crypto';
import type { IncomingMessage, Server } from 'node:http';
import type { Socket } from 'node:net';
import type { Duplex } from 'node:stream';

/** RFC 6455 section 1.3. */
const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

/** A channel's name without realtime-js's namespace ("realtime:gu-x" -> "gu-x"). */
export const bareTopic = (topic: string): string => (topic.startsWith('realtime:') ? topic.slice('realtime:'.length) : topic);

/** One Phoenix message, in the order the array carries it. */
export interface PhxMessage {
    joinRef: string | null;
    ref: string | null;
    topic: string;
    event: string;
    payload: Record<string, unknown>;
}

/** One connected client, and what it has joined. */
export class PhxSocket {
    readonly id = randomUUID();
    /**
     * Topics this socket has joined, by their BARE name, to the name the wire
     * uses. realtime-js namespaces every channel ("realtime:gu-<game>-<user>"),
     * so a backend that pushed to the name it was asked for would push to
     * nobody at all; the bare name is what a caller knows, the wire name is
     * what must go back out.
     */
    readonly joined = new Map<string, string>();
    /** The access token the client last presented (a join payload's, or an access_token push). */
    token: string | null = null;
    closed = false;

    constructor(private readonly sock: Duplex, readonly url: URL) {}

    /** One Phoenix frame out. */
    send(msg: [string | null, string | null, string, string, unknown]): void {
        if (this.closed) return;
        this.sock.write(frame(JSON.stringify(msg)));
    }

    /** A reply to `msg`, on its own ref. */
    reply(msg: PhxMessage, status: 'ok' | 'error', response: unknown = {}): void {
        this.send([msg.joinRef, msg.ref, msg.topic, 'phx_reply', { status, response }]);
    }

    close(): void {
        if (this.closed) return;
        this.closed = true;
        try { this.sock.end(Buffer.from([0x88, 0x00])); } catch { /* already gone */ }
    }
}

export interface PhxHandlers {
    /**
     * A socket wants to join `topic`. Return null to admit it, or a reason
     * object to refuse it (Realtime's own refusal shape is {reason}).
     * Called before the reply, so a handler may record who joined what.
     */
    onJoin?: (sock: PhxSocket, msg: PhxMessage) => { reason: string } | null;
    /** Anything that is not a join, a leave or a heartbeat. */
    onMessage?: (sock: PhxSocket, msg: PhxMessage) => void;
    onClose?: (sock: PhxSocket) => void;
    /** Every frame in and out, for a trace. */
    log?: (direction: 'in' | 'out', topic: string, event: string) => void;
}

/** The sockets, and what may be pushed to them. */
export class PhxHub {
    readonly sockets = new Set<PhxSocket>();

    constructor(private readonly handlers: PhxHandlers) {}

    /** Every socket that has joined `topic` (by its bare name). */
    subscribers(topic: string): PhxSocket[] {
        const bare = bareTopic(topic);
        return [...this.sockets].filter((s) => !s.closed && s.joined.has(bare));
    }

    /**
     * A broadcast to everyone on `topic`, in the shape a
     * .on('broadcast', { event }, cb) binding is handed. Returns how many
     * sockets it reached - 0 means nobody was listening, which is a real
     * Realtime outcome (there is no catch-up) and worth seeing in a trace.
     */
    broadcast(topic: string, event: string, payload: unknown): number {
        const subs = this.subscribers(topic);
        for (const s of subs) {
            s.send([null, null, s.joined.get(bareTopic(topic))!, 'broadcast', { type: 'broadcast', event, payload }]);
            this.handlers.log?.('out', topic, `broadcast:${event}`);
        }
        return subs.length;
    }

    /** A postgres_changes push, as realtime sends one (the payload's `ids` pick the binding). */
    postgresChanges(topic: string, ids: number[], data: unknown): number {
        const subs = this.subscribers(topic);
        for (const s of subs) {
            s.send([null, null, s.joined.get(bareTopic(topic))!, 'postgres_changes', { ids, data }]);
            this.handlers.log?.('out', topic, 'postgres_changes');
        }
        return subs.length;
    }

    closeAll(): void {
        for (const s of this.sockets) s.close();
        this.sockets.clear();
    }
}

/**
 * Serves the WebSocket half of a Supabase stack at `path` on `server`.
 * Returns the hub, which is how the backend pushes.
 */
export function attachPhoenix(server: Server, path: string, handlers: PhxHandlers = {}): PhxHub {
    const hub = new PhxHub(handlers);

    server.on('upgrade', (req: IncomingMessage, sock: Duplex, head: Buffer) => {
        const url = new URL(req.url ?? '/', 'http://127.0.0.1');
        if (url.pathname !== path) { sock.destroy(); return; }
        const key = req.headers['sec-websocket-key'];
        if (typeof key !== 'string') { sock.destroy(); return; }

        // realtime-js offers ["phoenix", "base64url.bearer.phx.<token>"] whenever
        // it holds a token; a server must pick one it was offered, and "phoenix"
        // is the one that means "no opinion about the token".
        const offered = String(req.headers['sec-websocket-protocol'] ?? '').split(',').map((s) => s.trim()).filter(Boolean);
        const accept = createHash('sha1').update(key + WS_GUID).digest('base64');
        const lines = [
            'HTTP/1.1 101 Switching Protocols',
            'Upgrade: websocket',
            'Connection: Upgrade',
            `Sec-WebSocket-Accept: ${accept}`,
        ];
        if (offered.includes('phoenix')) lines.push('Sec-WebSocket-Protocol: phoenix');
        sock.write(lines.join('\r\n') + '\r\n\r\n');
        // A push is one small frame and the trace times it: Nagle would hold it
        // back for the next write that never comes. `upgrade` hands a net.Socket,
        // which the http types widen to Duplex.
        (sock as Socket).setNoDelay?.(true);

        const client = new PhxSocket(sock, url);
        hub.sockets.add(client);

        let buf: Buffer<ArrayBufferLike> = head && head.length > 0 ? Buffer.from(head) : Buffer.alloc(0);
        // A message split across frames: opcode of the first, bytes so far.
        let partial: { opcode: number; chunks: Buffer<ArrayBufferLike>[] } | null = null;

        const finish = () => {
            if (client.closed) return;
            client.closed = true;
            hub.sockets.delete(client);
            handlers.onClose?.(client);
        };

        sock.on('data', (chunk: Buffer) => {
            buf = buf.length === 0 ? chunk : Buffer.concat([buf, chunk]);
            for (;;) {
                const read = unframe(buf);
                if (!read) break;
                buf = read.rest;
                if (read.opcode === 0x8) { client.close(); finish(); return; }
                if (read.opcode === 0x9) { sock.write(frame(read.payload.toString('utf8'), 0xa)); continue; }
                if (read.opcode === 0xa) continue;
                if (read.opcode === 0x0 && partial) partial.chunks.push(read.payload);
                else if (!read.fin) { partial = { opcode: read.opcode, chunks: [read.payload] }; continue; }
                if (!read.fin) continue;
                const body = partial ? Buffer.concat([...partial.chunks, ...(read.opcode === 0x0 ? [] : [read.payload])]) : read.payload;
                partial = null;
                deliver(client, hub, handlers, body.toString('utf8'));
            }
        });
        sock.on('error', finish);
        sock.on('close', finish);
    });

    return hub;
}

function deliver(sock: PhxSocket, hub: PhxHub, handlers: PhxHandlers, text: string): void {
    let parsed: unknown;
    try { parsed = JSON.parse(text); } catch { return; }
    if (!Array.isArray(parsed) || parsed.length < 5) return;
    const msg: PhxMessage = {
        joinRef: parsed[0] ?? null, ref: parsed[1] ?? null,
        topic: String(parsed[2] ?? ''), event: String(parsed[3] ?? ''),
        payload: (parsed[4] ?? {}) as Record<string, unknown>,
    };
    handlers.log?.('in', msg.topic, msg.event);

    if (msg.topic === 'phoenix' && msg.event === 'heartbeat') { sock.reply(msg, 'ok'); return; }

    switch (msg.event) {
        case 'phx_join': {
            const token = msg.payload.access_token;
            if (typeof token === 'string') sock.token = token;
            const refused = handlers.onJoin?.(sock, msg) ?? null;
            if (refused) { sock.reply(msg, 'error', refused); return; }
            sock.joined.set(bareTopic(msg.topic), msg.topic);
            sock.reply(msg, 'ok', joinResponse(msg));
            return;
        }
        case 'phx_leave':
            sock.joined.delete(bareTopic(msg.topic));
            sock.reply(msg, 'ok');
            sock.send([msg.joinRef, null, msg.topic, 'phx_close', {}]);
            return;
        case 'access_token': {
            const token = msg.payload.access_token;
            if (typeof token === 'string') sock.token = token;
            if (msg.ref) sock.reply(msg, 'ok');
            return;
        }
        default:
            handlers.onMessage?.(sock, msg);
            if (msg.ref) sock.reply(msg, 'ok');
    }
    void hub;
}

/**
 * What a join is answered WITH. A channel with postgres_changes bindings gets
 * its own filters back, each with an id: realtime-js compares them field for
 * field and errors the channel if they do not match, so the only right answer
 * is the request's own list. A broadcast-only channel gets {}.
 */
function joinResponse(msg: PhxMessage): Record<string, unknown> {
    const config = msg.payload.config as { postgres_changes?: unknown[] } | undefined;
    const wanted = config?.postgres_changes;
    if (!Array.isArray(wanted) || wanted.length === 0) return {};
    return { postgres_changes: wanted.map((f, i) => ({ ...(f as object), id: i + 1 })) };
}

// ---- RFC 6455 framing -------------------------------------------------------

/** One server frame: text by default, unmasked, as a browser requires. */
function frame(text: string, opcode = 0x1): Buffer {
    const body = Buffer.from(text, 'utf8');
    const n = body.length;
    let header: Buffer;
    if (n < 126) {
        header = Buffer.from([0x80 | opcode, n]);
    } else if (n < 65536) {
        header = Buffer.alloc(4);
        header[0] = 0x80 | opcode;
        header[1] = 126;
        header.writeUInt16BE(n, 2);
    } else {
        header = Buffer.alloc(10);
        header[0] = 0x80 | opcode;
        header[1] = 127;
        header.writeBigUInt64BE(BigInt(n), 2);
    }
    return Buffer.concat([header, body]);
}

interface Unframed { fin: boolean; opcode: number; payload: Buffer<ArrayBufferLike>; rest: Buffer<ArrayBufferLike> }

/** One client frame off the front of `buf`, or null while it is short. */
function unframe(buf: Buffer<ArrayBufferLike>): Unframed | null {
    if (buf.length < 2) return null;
    const fin = (buf[0] & 0x80) !== 0;
    const opcode = buf[0] & 0x0f;
    const masked = (buf[1] & 0x80) !== 0;
    let len = buf[1] & 0x7f;
    let at = 2;
    if (len === 126) {
        if (buf.length < at + 2) return null;
        len = buf.readUInt16BE(at);
        at += 2;
    } else if (len === 127) {
        if (buf.length < at + 8) return null;
        len = Number(buf.readBigUInt64BE(at));
        at += 8;
    }
    let mask: Buffer | null = null;
    if (masked) {
        if (buf.length < at + 4) return null;
        mask = buf.subarray(at, at + 4);
        at += 4;
    }
    if (buf.length < at + len) return null;
    const payload = Buffer.from(buf.subarray(at, at + len));
    if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
    return { fin, opcode, payload, rest: buf.subarray(at + len) };
}
