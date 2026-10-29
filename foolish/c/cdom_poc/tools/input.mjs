// Real input through CDP, as a person's hand makes it: a mouse drag (pressed, moved in steps,
// released), the same by touch (Input.dispatchTouchEvent), and keys (Input.dispatchKeyEvent).
// Nothing is synthesised in the page: the browser makes the events, hit-tests them and runs its
// default actions (a native drag, a scroll, the mouse events after a touch) unless the page stops them.
import { sleep } from './cdp.mjs';

// The centre of the first element `sel` names, where it rests (a hand card slides for 0.1 s).
export async function centre(b, sel) {
  const rect = `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null;
    const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`;
  let at = await b.evaluate(rect);
  for (let n = 0; n < 40 && at; n++) {
    await sleep(20);
    const again = await b.evaluate(rect);
    if (again && again.x === at.x && again.y === at.y) break;
    at = again;
  }
  if (!at) throw new Error(`nothing at ${sel}`);
  return at;
}

// A press at `from`, moves through `points` (each reached in `steps` moves `stepMs` apart), the
// pointer held `holdMs` at the last one, then released there. `atEach(i)` is awaited at each
// point reached (the test reads the page mid-drag). touch: the same by one finger.
export async function drag(b, from, points, { touch = false, steps = 4, stepMs = 16, holdMs = 0, atEach = null } = {}) {
  const press = (type, x, y) => touch
    ? b.send('Input.dispatchTouchEvent', { type, touchPoints: type === 'touchEnd' ? [] : [{ x, y, id: 1 }] })
    : b.send('Input.dispatchMouseEvent', { type, x, y, button: 'left', buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1 });
  await press(touch ? 'touchStart' : 'mousePressed', from.x, from.y);
  let at = from;
  for (let i = 0; i < points.length; i++) {
    const to = points[i];
    for (let k = 1; k <= steps; k++) {
      const x = at.x + (to.x - at.x) * k / steps, y = at.y + (to.y - at.y) * k / steps;
      await press(touch ? 'touchMove' : 'mouseMoved', x, y);
      if (stepMs) await sleep(stepMs);
    }
    at = to;
    if (atEach) await atEach(i);
  }
  if (holdMs) await sleep(holdMs);
  await press(touch ? 'touchEnd' : 'mouseReleased', at.x, at.y);
}

// A key pressed and released. key is event.key ("a", " ", "ArrowUp", "Shift", "3"); mods: any of
// shift, ctrl, alt, meta.
const CODES = { ' ': ['Space', 32], ArrowLeft: ['ArrowLeft', 37], ArrowUp: ['ArrowUp', 38], ArrowRight: ['ArrowRight', 39],
  ArrowDown: ['ArrowDown', 40], Shift: ['ShiftLeft', 16], '-': ['Minus', 189], '=': ['Equal', 187] };
export async function key(b, k, mods = {}) {
  let [code, keyCode] = CODES[k] || [];
  if (!code && /^[a-z]$/i.test(k)) { code = 'Key' + k.toUpperCase(); keyCode = k.toUpperCase().charCodeAt(0); }
  if (!code && /^[0-9]$/.test(k)) { code = 'Digit' + k; keyCode = k.charCodeAt(0); }
  const modifiers = (mods.alt ? 1 : 0) | (mods.ctrl ? 2 : 0) | (mods.meta ? 4 : 0) | (mods.shift ? 8 : 0);
  const text = k.length === 1 && !mods.ctrl && !mods.meta ? k : undefined;
  // No nativeVirtualKeyCode: on macOS it is read as the Mac's own key code, and 16 for Shift hid the
  // headless page (document.hidden, no more frames) in every run that pressed it.
  const base = { key: k, code, windowsVirtualKeyCode: keyCode, modifiers };
  await b.send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', text, ...base });
  await b.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
}
