"use strict";
// cdom shim: C calls one of these functions per DOM operation, and the shim posts events back.
// Fixed per app. Wasm passes i32, so ids and numbers that may pass 2^31 are made unsigned (>>> 0).
// Written to be read: the page ships it minified (terser), so names and lines cost nothing there.
var cdom = (() => {
  const EVENTS = ["click", "dblclick", "input", "change", "keydown", "keyup", "focusin",
    "focusout", "submit", "pointerdown", "pointerup", "pointermove", "resize", "visibilitychange",
    "mousedown", "mousemove", "mouseup", "touchstart", "touchmove", "touchend", "touchcancel"];
  const RESIZE = 12;            // EVENTS index of the window's resize
  const VISIBILITY = 13;        // and of visibilitychange
  const TRANSIENT = 0x40000000; // string ids from here up are one-off strings (cd_tmp)
  const DETACHED = 0xffffffff;  // "no insertion parent" for at()
  const SMALL = 64;             // string batches up to this many bytes are decoded char by char
  const SVG = "http://www.w3.org/2000/svg";
  const doc = document;
  const textDecoder = new TextDecoder();
  const textEncoder = new TextEncoder();
  const nodes = [];         // node by id
  const interned = [];      // interned strings by id
  const transient = [];     // one-off strings by id - TRANSIENT
  const templates = [];     // template root by template id
  const stack = [];         // the open elements
  const delegated = [];     // event types with a listener on the mount
  const templateSlots = []; // slot text nodes by template id
  let slots = [];           // slot text nodes of the template being built
  const windowListeners = []; // the document's (or window's) listener per event type, while listened
  let listenMask = new Uint32Array(1024); // listened event types per node id (a bit per type)
  let exports;              // the wasm module's exports
  let memory;
  let buffer;               // memory.buffer when the views below were made
  let u8;
  let u32;
  let i32;
  let stringBytes;          // where C writes a batch of string bytes
  let stringMeta;           // where C writes (id, bytes, chars) per string in the batch
  let inbox;                // where the event record goes
  let depth = 0;            // how many elements are open
  let last = null;          // the node made or walked to last
  let root = null;          // the outermost element closed last
  let base = DETACHED;      // the parent new top-level nodes go into
  let before = 0;           // the node they go before (0: at the end)

  // Typed views over wasm memory, made again after it grows.
  function views() {
    buffer = memory.buffer;
    u8 = new Uint8Array(buffer);
    u32 = new Uint32Array(buffer);
    i32 = new Int32Array(buffer);
  }
  // A view of length bytes of C memory at ptr, good until the memory grows.
  function bytes(ptr, length) {
    return new Uint8Array(memory.buffer, ptr, length);
  }
  // A copy of them, which outlives the call and any growth.
  function copyBytes(ptr, length) {
    return bytes(ptr, length).slice();
  }
  // Puts node into parent before node next (null: at the end).
  function place(parent, node, next) {
    parent.insertBefore(node, next);
  }
  // A string by id, interned or one-off.
  function stringOf(id) {
    return id < TRANSIENT ? interned[id] : transient[id - TRANSIENT];
  }
  // Keeps string value under id.
  function keep(id, value) {
    if (id < TRANSIENT) {
      interned[id] = value;
    } else {
      transient[id - TRANSIENT] = value;
    }
  }

  // Decodes a batch of count strings (byteCount bytes in all): one TextDecoder call for the
  // batch, or a char loop when it is small.
  function decode(count, byteCount) {
    let meta = stringMeta >> 2;
    if (byteCount > SMALL) {
      const all = textDecoder.decode(u8.subarray(stringBytes, stringBytes + byteCount));
      let offset = 0;
      for (let i = 0; i < count; i++, meta += 3) {
        const chars = u32[meta + 2];
        keep(u32[meta], all.substring(offset, offset + chars));
        offset += chars;
      }
      return;
    }
    let at = stringBytes;
    for (let i = 0; i < count; i++, meta += 3) {
      const bytes = u32[meta + 1];
      let value;
      if (bytes === u32[meta + 2]) { // ASCII: one char per byte
        value = "";
        for (let k = 0; k < bytes; k++) {
          value += String.fromCharCode(u8[at + k]);
        }
      } else {
        value = textDecoder.decode(u8.subarray(at, at + bytes));
      }
      keep(u32[meta], value);
      at += bytes;
    }
  }

  // Puts a new top-level node where at() said, or into the open element.
  function placeNew(node) {
    if (depth) {
      stack[depth - 1].appendChild(node);
    } else if (base !== DETACHED) {
      place(nodes[base], node, before ? nodes[before] : null);
    }
  }

  // Where an event happened: a touch event's first changed touch (the finger that pressed, moved or
  // lifted), any other event's own clientX and clientY.
  function pointOf(event) {
    return event.changedTouches ? event.changedTouches[0] : event;
  }
  // 1 when the event is aimed at a field the user types into.
  function editing(target) {
    if (!target || !target.tagName) {
      return 0;
    }
    const tag = target.tagName;
    return +(target.isContentEditable || tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT");
  }
  // Writes the event record into C memory and calls C; C flushes its answer on the way out, and
  // may have asked for the event's default action not to be taken.
  function post(type, id, event, path, pathDepth) {
    if (buffer !== memory.buffer) {
      views();
    }
    const record = inbox >> 2;
    const point = pointOf(event);
    const key = event.key;
    i32[record] = type;
    i32[record + 1] = id;
    i32[record + 2] = point.clientX | 0;
    i32[record + 3] = point.clientY | 0;
    i32[record + 4] = event.button | 0;
    i32[record + 5] = event.keyCode | 0;
    i32[record + 6] = event.shiftKey | (event.ctrlKey << 1) | (event.altKey << 2) | (event.metaKey << 3)
      | (editing(event.target) << 4);
    i32[record + 7] = path;
    i32[record + 8] = pathDepth;
    i32[record + 9] = typeof key === "string" && key.length === 1 ? key.charCodeAt(0) : 0;
    i32[record + 10] = 0;
    exports.cd_event();
    if (buffer !== memory.buffer) {
      views();
    }
    if (i32[record + 10] && event.preventDefault) {
      event.preventDefault();
    }
  }

  // One listener on the mount per event type: the event goes to the nearest listening ancestor of
  // its target, with the path from that node down (8 bits of child index per level).
  function delegate(type) {
    delegated[type] = 1;
    const bit = 1 << type;
    const mount = nodes[0];
    mount.addEventListener(EVENTS[type], (event) => {
      let path = 0;
      let pathDepth = 0;
      for (let node = event.target; node && node !== mount; node = node.parentNode) {
        if (node.$i !== undefined && listenMask[node.$i] & bit) {
          post(type, node.$i, event, path, pathDepth);
          return;
        }
        let index = 0;
        for (let sibling = node.previousElementSibling; sibling; sibling = sibling.previousElementSibling) {
          index++;
        }
        path = (path << 8) | (index & 255);
        pathDepth++;
      }
    });
  }

  // C sends a batch of new strings.
  function strings(count, byteCount) {
    if (buffer !== memory.buffer) {
      views();
    }
    decode(count, byteCount);
  }
  // Where new top-level nodes go: into parent, before node beforeId (0: at the end).
  function at(parent, beforeId) {
    base = parent >>> 0;
    before = beforeId;
  }
  // An opened element or clone is a child of the open element, if any.
  function push(element) {
    if (depth) {
      stack[depth - 1].appendChild(element);
    }
    stack[depth++] = element;
    last = element;
  }
  // Opens an element; C says which tags are SVG.
  function open(tag) {
    push(doc.createElement(stringOf(tag)));
  }
  function open_svg(tag) {
    push(doc.createElementNS(SVG, stringOf(tag)));
  }
  // Closes the open element; the outermost one is placed.
  function close() {
    const element = stack[--depth];
    if (!depth) {
      root = element;
      placeNew(element);
    }
  }
  function text(id) {
    last = doc.createTextNode(stringOf(id));
    placeNew(last);
  }
  function text_num(number) {
    last = doc.createTextNode(number >>> 0);
    placeNew(last);
  }
  function attr(name, value) {
    last.setAttribute(stringOf(name), stringOf(value));
  }
  function setClass(value) {
    last.className = stringOf(value);
  }
  // Names the last node, or the open element, with a node id.
  function name(id) {
    nodes[id] = last;
  }
  function name_open(id) {
    nodes[id] = stack[depth - 1];
  }
  // Names the last node and listens for event type on it (through the mount's listener).
  function listen(id, type) {
    nodes[id] = last;
    last.$i = id;
    if (id >= listenMask.length) {
      const grown = new Uint32Array(id * 2);
      grown.set(listenMask);
      listenMask = grown;
    }
    listenMask[id] |= 1 << type;
    if (!delegated[type]) {
      delegate(type);
    }
  }
  function value(id) {
    last.nodeValue = stringOf(id);
  }
  function value_num(number) {
    last.nodeValue = number >>> 0;
  }
  // The element just built becomes template templateId, with the slots marked in it.
  function tpl_save(templateId) {
    templates[templateId] = root;
    templateSlots[templateId] = slots;
    slots = [];
    root.remove();
  }
  function clone(templateId) {
    push(templates[templateId].cloneNode(true));
  }
  function slot(index) {
    slots[index] = last;
  }
  function tpl_text(templateId, index, id) {
    templateSlots[templateId][index].nodeValue = stringOf(id);
  }
  function tpl_text_num(templateId, index, number) {
    templateSlots[templateId][index].nodeValue = number >>> 0;
  }
  // A walk: count steps of 2 bits (1 firstChild, 2 nextSibling, 3 parentNode) from a named node,
  // or from the last node when from is -1.
  function walk(from, steps, count) {
    let node = from === -1 ? last : nodes[from];
    for (; count; count--, steps >>>= 2) {
      const step = steps & 3;
      if (step === 1) {
        node = node.firstChild;
      } else if (step === 2) {
        node = node.nextSibling;
      } else {
        node = node.parentNode;
      }
    }
    last = node;
  }
  function walk_value(from, steps, count, id) {
    walk(from, steps, count);
    last.nodeValue = stringOf(id);
  }
  function walk_value_num(from, steps, count, number) {
    walk(from, steps, count);
    last.nodeValue = number >>> 0;
  }
  function set_text(id, textId) {
    nodes[id].nodeValue = stringOf(textId);
  }
  function set_text_num(id, number) {
    nodes[id].nodeValue = number >>> 0;
  }
  function set_attr(id, name, value) {
    nodes[id].setAttribute(stringOf(name), stringOf(value));
  }
  function set_class(id, value) {
    nodes[id].className = stringOf(value);
  }
  // One style property.
  function style(id, property, value) {
    nodes[id].style.setProperty(stringOf(property), stringOf(value));
  }
  function style_num(id, property, number, unit) {
    nodes[id].style.setProperty(stringOf(property), number + stringOf(unit));
  }
  // Pixels in C memory (channels bytes each: RGBA, or RGB and opaque), at (x, y) in an image of
  // fullWidth x fullHeight, to an image file: copied now, encoded off the call, the file's bytes
  // back into C (cd_encoded) with token.
  function encode(pixelsPtr, width, height, channels, fullWidth, fullHeight, x, y, mime, token) {
    const canvas = doc.createElement("canvas");
    canvas.width = fullWidth;
    canvas.height = fullHeight;
    let pixels = new Uint8ClampedArray(memory.buffer, pixelsPtr, width * height * channels);
    if (channels === 3) {
      const rgba = new Uint8ClampedArray(width * height * 4);
      for (let from = 0, to = 0; to < rgba.length; from += 3, to += 4) {
        rgba[to] = pixels[from];
        rgba[to + 1] = pixels[from + 1];
        rgba[to + 2] = pixels[from + 2];
        rgba[to + 3] = 255;
      }
      pixels = rgba;
    }
    canvas.getContext("2d").putImageData(new ImageData(pixels, width, height), x, y);
    canvas.toBlob(async (blob) => deliver(exports.cd_encoded, token, await blob.arrayBuffer()), stringOf(mime), 0.92);
  }
  // An image file's bytes in C memory as a url(blob:) on a style property, now.
  function image_bytes(id, property, bytesPtr, length, mime) {
    const blob = new Blob([copyBytes(bytesPtr, length)], { type: stringOf(mime) });
    nodes[id].style.setProperty(stringOf(property), `url(${URL.createObjectURL(blob)})`);
  }
  function remove(id) {
    nodes[id].remove();
    nodes[id] = undefined;
    listenMask[id] = 0;
  }
  function free(id) {
    nodes[id] = undefined;
    listenMask[id] = 0;
  }
  // Moves a node before node beforeId (0: to the end of its parent).
  function move(id, beforeId) {
    const node = nodes[id];
    place(node.parentNode, node, beforeId ? nodes[beforeId] : null);
  }
  function clear(id) {
    nodes[id].textContent = "";
  }
  // Takes a node out of the document, remembering where it was, and puts it back.
  function detach(id) {
    const node = nodes[id];
    node.$p = node.parentNode;
    node.$s = node.nextSibling;
    node.remove();
  }
  function attach(id) {
    const node = nodes[id];
    place(node.$p, node, node.$s);
    node.$p = null;
    node.$s = null;
  }

  // Window events go through the event record like any other: resize and visibilitychange as the
  // viewport (x, y its size, key 1 when hidden), every other type as the event itself.
  function viewportRecord() {
    return { clientX: innerWidth, clientY: innerHeight, keyCode: +doc.hidden };
  }
  function windowTarget(type) {
    return type === RESIZE ? window : doc;
  }
  // Listens for event type on the whole page, once however often it is asked; not passive, so C
  // can stop a touchmove scrolling the page.
  function win(type) {
    if (windowListeners[type]) {
      return;
    }
    const viewportEvent = type === RESIZE || type === VISIBILITY;
    const listener = (event) => post(type, 0, viewportEvent ? viewportRecord() : event, 0, 0);
    windowListeners[type] = listener;
    windowTarget(type).addEventListener(EVENTS[type], listener, { passive: false });
  }
  function unwin(type) {
    const listener = windowListeners[type];
    if (!listener) {
      return;
    }
    windowTarget(type).removeEventListener(EVENTS[type], listener);
    windowListeners[type] = undefined;
  }
  // The node an event at (x, y) would go to: the nearest listening ancestor-or-self of the topmost
  // element there that takes pointer events; 0 for none.
  function hit(x, y) {
    const mount = nodes[0];
    for (let node = doc.elementFromPoint(x, y); node && node !== mount; node = node.parentNode) {
      if (node.$i !== undefined && listenMask[node.$i]) {
        return node.$i;
      }
    }
    return 0;
  }
  // The viewport into C memory: width, height, hidden, device pixel ratio times 100.
  function viewport(recordPtr) {
    if (buffer !== memory.buffer) {
      views();
    }
    i32.set([innerWidth, innerHeight, +doc.hidden, Math.round(devicePixelRatio * 100)], recordPtr >> 2);
  }
  // A node's box on screen into C memory as four doubles: left, top, width, height.
  function rect(id, outPtr) {
    const box = nodes[id].getBoundingClientRect();
    new Float64Array(memory.buffer, outPtr, 4).set([box.left, box.top, box.width, box.height]);
  }
  function timer(ms, token) {
    setTimeout(() => exports.cd_timer(token), ms);
  }
  function raf(token) {
    requestAnimationFrame((time) => exports.cd_raf(token, time));
  }
  function now() {
    return performance.now();
  }

  // UTF-8 bytes in C memory as a string.
  function utf8(ptr, length) {
    return textDecoder.decode(bytes(ptr, length));
  }
  // Bytes into C memory, in room C makes with cd_bytes_buf, then answer(arg, ptr, length).
  function deliver(answer, arg, data) {
    const incoming = new Uint8Array(data);
    const ptr = exports.cd_bytes_buf(incoming.length);
    bytes(ptr, incoming.length).set(incoming);
    answer(arg, ptr, incoming.length);
  }

  // The network (src/cdom_net.c).
  const sockets = [];     // the open sockets by id
  const freeSockets = []; // ids whose sockets closed, handed out again first
  // Opens a WebSocket to the url in C memory; its events come back by socket id. The id is free
  // once the socket closed (before C hears it), so the table holds only the sockets open at once.
  function ws_open(urlPtr, urlLen) {
    const id = freeSockets.length ? freeSockets.pop() : sockets.length;
    const socket = new WebSocket(utf8(urlPtr, urlLen));
    sockets[id] = socket;
    socket.binaryType = "arraybuffer";
    socket.onopen = () => exports.cd_net_open(id);
    socket.onmessage = (event) => {
      const data = typeof event.data === "string" ? textEncoder.encode(event.data) : event.data;
      deliver(exports.cd_net_msg, id, data);
    };
    socket.onclose = (event) => {
      sockets[id] = undefined;
      freeSockets.push(id);
      exports.cd_net_close(id, event.code);
    };
    return id;
  }
  // send copies the bytes it is given, so a view of C memory is enough.
  function ws_send(id, bytesPtr, length) {
    sockets[id].send(bytes(bytesPtr, length));
  }
  function ws_close(id) {
    sockets[id].close();
  }
  function ws_buffered(id) {
    return sockets[id].bufferedAmount;
  }
  // Request headers from one block of "Name: value\n" lines.
  function headerList(headersPtr, headersLen) {
    const list = [];
    for (const line of utf8(headersPtr, headersLen).split("\n")) {
      const colon = line.indexOf(":");
      if (colon > 0) {
        list.push([line.slice(0, colon), line.slice(colon + 1).trim()]);
      }
    }
    return list;
  }
  // One request; the status and the body come back with token: status 0 when it never arrived,
  // 1 when deadlineMs (0: none) passed first and the request was aborted.
  function http(token, methodPtr, methodLen, urlPtr, urlLen, headersPtr, headersLen, bodyPtr, bodyLen, deadlineMs) {
    const controller = new AbortController();
    const init = {
      method: utf8(methodPtr, methodLen),
      headers: headerList(headersPtr, headersLen),
      body: bodyLen ? copyBytes(bodyPtr, bodyLen) : undefined,
      signal: controller.signal,
    };
    const timer = deadlineMs ? setTimeout(() => controller.abort(), deadlineMs) : 0;
    let status = 0;
    fetch(utf8(urlPtr, urlLen), init)
      .then((response) => {
        status = response.status;
        return response.arrayBuffer();
      })
      .then(
        (data) => {
          clearTimeout(timer);
          deliver((arg, ptr, length) => exports.cd_net_http(arg, status, ptr, length), token, data);
        },
        () => {
          clearTimeout(timer);
          exports.cd_net_http(token, controller.signal.aborted ? 1 : 0, 0, 0);
        });
  }
  function epoch() {
    return Date.now();
  }

  // A key/value store over IndexedDB (src/cdom_store.c): bytes in, bytes out, under keys C chooses.
  // A promise of an IndexedDB request's result.
  function promised(request) {
    return new Promise((resolve, reject) => {
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
  }
  let database; // a promise of the open database, made on first use
  function openDatabase() {
    if (!database) {
      const request = indexedDB.open("cdom", 1);
      request.onupgradeneeded = () => request.result.createObjectStore("kv");
      database = promised(request);
    }
    return database;
  }
  // One request on the store in a transaction of its own; a promise of its result.
  async function storeRequest(mode, makeRequest) {
    const db = await openDatabase();
    return promised(makeRequest(db.transaction("kv", mode).objectStore("kv")));
  }
  // Keeps a copy of the bytes under the key, then answers with token: 1 kept, 0 failed. The key
  // and the bytes are read now: C's memory may hold something else once the database is open.
  function store_put(token, keyPtr, keyLen, bytesPtr, length) {
    const key = utf8(keyPtr, keyLen);
    const data = copyBytes(bytesPtr, length);
    storeRequest("readwrite", (store) => store.put(data, key)).then(
      () => exports.cd_store_put_done(token, 1),
      () => exports.cd_store_put_done(token, 0));
  }
  // Forgets the key, then answers with token: 1 done, 0 failed.
  function store_del(token, keyPtr, keyLen) {
    const key = utf8(keyPtr, keyLen);
    storeRequest("readwrite", (store) => store.delete(key)).then(
      () => exports.cd_store_del_done(token, 1),
      () => exports.cd_store_del_done(token, 0));
  }
  // Answers once with token: the bytes under the key, or length -1 for a miss, an error, or no
  // answer within ms (indexedDB.open can hang). Anything not bytes is a miss.
  function store_get(token, keyPtr, keyLen, ms) {
    const key = utf8(keyPtr, keyLen);
    let answered = false;
    const answer = (data) => {
      if (answered) {
        return;
      }
      answered = true;
      if (data instanceof Uint8Array) {
        deliver(exports.cd_store_got, token, data);
      } else {
        exports.cd_store_got(token, 0, -1);
      }
    };
    setTimeout(answer, ms);
    // the error must not reach answer: only a miss may follow a failure
    storeRequest("readonly", (store) => store.get(key)).then(answer, () => answer());
  }

  // An input's value into C memory as UTF-8; returns the bytes written.
  function input(id, destPtr, capacity) {
    const result = textEncoder.encodeInto(String(nodes[id].value ?? ""), bytes(destPtr, capacity));
    return result.written;
  }

  // Starts the app: mount is an element, or the id of one that may not be parsed yet: then the
  // wasm loads while the parser and the stylesheets run, and the app starts once the DOM is ready.
  // options.openStore: an app that reads its store as it starts opens it now, while the wasm is
  // fetched and compiled, so its first get finds the database open.
  async function start(url, mount, env, options) {
    if (options && options.openStore) {
      openDatabase().catch(() => {}); // a failure shows again, as a miss, at the first get
    }
    const imports = {
      cdom: {
        strings, at, open, open_svg, close, text, text_num, attr, class: setClass, name, name_open,
        listen, value, value_num, tpl_save, clone, slot, tpl_text, tpl_text_num, walk, walk_value, walk_value_num,
        set_text, set_text_num, set_attr, set_class, style, style_num, encode, image_bytes, remove, free, move, clear,
        detach, attach, input, win, unwin, hit, viewport, rect, timer, raf, now, ws_open, ws_send, ws_close, ws_buffered, http, epoch,
        store_put, store_get, store_del,
      },
      env: env || {},
    };
    const response = fetch(url);
    let instance;
    if (WebAssembly.instantiateStreaming) {
      instance = (await WebAssembly.instantiateStreaming(response, imports)).instance;
    } else {
      instance = (await WebAssembly.instantiate(await (await response).arrayBuffer(), imports)).instance;
    }
    if (typeof mount === "string") {
      if (doc.readyState === "loading") {
        await new Promise((resolve) => doc.addEventListener("DOMContentLoaded", resolve, { once: true }));
      }
      mount = doc.getElementById(mount);
    }
    nodes[0] = mount;
    exports = instance.exports;
    memory = exports.memory;
    views();
    const layout = exports.cd_layout() >> 2;
    stringBytes = u32[layout];
    stringMeta = u32[layout + 1];
    inbox = u32[layout + 2];
    exports.cd_start((Math.random() * 4294967296) >>> 0);
    return exports;
  }
  return { start };
})();
