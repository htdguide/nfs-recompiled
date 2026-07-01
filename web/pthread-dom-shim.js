// Worker-side DOM shim.
//
// With -sPROXY_TO_PTHREAD the program's main() (and therefore SDL3's video and
// event initialisation) runs on a Web Worker. SDL3's *experimental* Emscripten
// port registers some event handlers with plain EM_ASM blocks that touch
// `document` directly on the calling thread instead of proxying to the browser
// main thread. In a Worker `document` does not exist, so those blocks throw and
// abort the game.
//
// This pre-js installs a minimal inert `document`/`window` on the Worker global
// so those handler-registration blocks run without throwing. Any unknown
// property resolves to a no-op function via a Proxy, so we don't have to
// enumerate every DOM method SDL happens to call; the few properties whose
// *values* matter (focus/visibility/fullscreen state) are given sane defaults
// so SDL doesn't decide the page is hidden and stop rendering. Real input still
// arrives through Emscripten's proxied event callbacks; these stubbed listeners
// simply never fire. This is a workaround for the SDL3 port, not a real DOM.
(function () {
  if (typeof document !== 'undefined') return;          // main thread: real DOM
  if (typeof importScripts !== 'function') return;      // not a worker

  var noop = function () { return undefined; };

  // Wrap a base object so any unlisted property read returns a callable no-op
  // (covers the "X is not a function" cases) while listed props keep their value.
  function wrap(base) {
    return new Proxy(base, {
      get: function (t, prop) {
        if (prop in t) return t[prop];
        if (typeof prop === 'symbol') return undefined;
        return noop;
      },
      set: function (t, prop, val) { t[prop] = val; return true; },
      has: function () { return true; },
    });
  }

  function stubElement() {
    return wrap({
      style: {},
      dataset: {},
      classList: { add: noop, remove: noop, toggle: noop, contains: function () { return false; } },
      getAttribute: function () { return null; },
      getContext: function () { return null; },
      getBoundingClientRect: function () {
        return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 };
      },
      children: [],
      childNodes: [],
    });
  }

  var shared = stubElement();
  globalThis.document = wrap({
    createElement: function () { return stubElement(); },
    createElementNS: function () { return stubElement(); },
    createTextNode: function () { return stubElement(); },
    querySelector: function () { return null; },
    querySelectorAll: function () { return []; },
    getElementById: function () { return null; },
    getElementsByTagName: function () { return []; },
    getElementsByClassName: function () { return []; },
    body: shared,
    head: shared,
    documentElement: shared,
    activeElement: null,
    hasFocus: function () { return true; },
    hidden: false,
    visibilityState: 'visible',
    fullscreenElement: null,
    fullscreenEnabled: false,
    pointerLockElement: null,
    title: '',
  });

  if (typeof window === 'undefined') {
    globalThis.window = globalThis;
    if (typeof globalThis.devicePixelRatio === 'undefined') globalThis.devicePixelRatio = 1;
  }
})();
