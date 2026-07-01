# Porting nfs-recompiled to WebAssembly

This document tracks the work to run the static recompilation in a browser via
Emscripten. It is a living document — the port is in progress.

## Approach

The native build is SDL3 + desktop OpenGL. Emscripten already ships an SDL3 port
and maps GL to WebGL, so the strategy is:

| Concern            | Native            | Web target                                  |
|--------------------|-------------------|---------------------------------------------|
| Windowing / input  | SDL3              | SDL3 emscripten port (`-sUSE_SDL=3`)        |
| Rendering          | OpenGL (compat)   | WebGL 2 / GLES 3 (`-sFULL_ES3`, `MAX_WEBGL_VERSION=2`) |
| Threads            | SDL threads       | pthreads (`-pthread`) + `SharedArrayBuffer` |
| Blocking main loop | `SDL_WaitEvent`   | `-sPROXY_TO_PTHREAD` (game runs off main thread) + `-sASYNCIFY` |
| MMX / SSE          | x86 intrinsics    | WASM SIMD (`-msimd128 -msse..-msse4.1`)     |
| File I/O           | host filesystem   | MEMFS via `--preload-file DIR@/data`        |

### Why PROXY_TO_PTHREAD

The game drives its own Win32-style message pump that **blocks** on
`SDL_WaitEvent`, and spawns worker threads with semaphores (`src/lib/thread.cpp`,
`src/lib/window.cpp`). A browser tab cannot block its main thread. Running the
whole program on a pthread (`PROXY_TO_PTHREAD`) lets the blocking pump and worker
threads behave as on desktop while the browser main thread stays responsive and
owns the WebGL canvas (SDL proxies GL calls to it).

## Changes made

### Build system — `CMakeLists.txt`
- Added `TARGET_WEB` (`if(EMSCRIPTEN)`) detection.
- Under web: skip `find_package(SDL3/OpenGL)`; an INTERFACE target carries
  `-sUSE_SDL=3 -pthread -msimd128 -msse..-msse4.1`.
- Dropped `-Werror` on web (different clang warning set over generated code).
- Per-game link options: `PROXY_TO_PTHREAD`, WebGL2/`FULL_ES3`,
  `ALLOW_MEMORY_GROWTH`, `INITIAL_MEMORY=512M`, `STACK_SIZE=8M`, `ASYNCIFY`,
  pthread pool, HTML shell, optional `--preload-file` via `-DNFS_DATA_DIR`.
- Output suffix `.html`.

### Assert trap — `include/x86.h`
`NFS2_ASSERT` used `asm("int3")`; wasm has no int3 → use `__builtin_trap()`.

### 2D blit — `src/lib/renderer.cpp`
The software-rendered framebuffer was blitted with fixed-function
`glBegin/glOrtho/glEnable(GL_TEXTURE_2D)` and `GL_UNSIGNED_INT_8_8_8_8`.
WebGL has none of these. Added a `__EMSCRIPTEN__` path:
- a minimal GLES2 shader + triangle-strip fullscreen quad;
- 8-bit palette frames byte-swapped and uploaded as `GL_UNSIGNED_BYTE`;
- GL debug-callback / `GL_TEXTURE_2D` enable compiled out.

### Glide/3D renderer — `src/lib/gliderenderer.cpp`
- GLSL `#version 400` vertex+fragment shaders ported to `#version 300 es`
  (added fragment `precision`, `texture2D`→`texture`, float literals).
- `swap()` built its projection with the fixed-function matrix stack
  (`glOrtho` + `glGetFloatv(GL_PROJECTION_MATRIX)`); replaced on web with a
  hand-computed column-major ortho matrix fed to the existing `u_transform`.

### Front-end — `web/`
- `shell.html` — Emscripten shell with a responsive canvas, status line, and
  `arguments:['/data']`.
- `serve.py` — static server sending COOP/COEP headers (required for threads).
- `README.md` — build & run instructions.

## Threading model on the web

`nfs2se` boots with real NFS II: SE data and runs its logic on worker pthreads.
Getting there needed two more decisions beyond the initial port:

- **`-sPROXY_TO_PTHREAD`** — main() (window + the blocking `SDL_WaitEvent` pump)
  runs on a pthread; emscripten proxies the workers' GL to the browser main
  thread's WebGL context (`-sOFFSCREEN_FRAMEBUFFER=1`).
- **`web/pthread-dom-shim.js`** (a `--pre-js`) — SDL3's *experimental* Emscripten
  port registers some event handlers with plain `EM_ASM` blocks that touch
  `document` on the calling (worker) thread, where it doesn't exist. The shim
  installs an inert `document`/`window` on the worker so those blocks don't throw.
  Without it the game aborts at the menu with "document is not defined".

Tried and rejected: `-sOFFSCREENCANVAS_SUPPORT` + `OFFSCREENCANVASES_TO_PTHREAD`.
The canvas is transferred to one pthread, but the game renders from a *different*
thread it spawns itself, which stalls rendering.

## Status / TODO

- [x] `nfs_core` + full game disassembly compile to wasm.
- [x] Link `nfs2se` end-to-end (.html/.js/.wasm/.data).
- [x] Boot in-browser with real NFS II: SE data preloaded at `/data`.
- [x] Runs game logic + audio (SDL3 ScriptProcessorNode) without crashing.
- [x] Canvas sizing / present pipeline wired up: `Renderer::setVideoMode` sizes
  the emscripten offscreen buffer *and* the DOM canvas (main thread, via
  `MAIN_THREAD_EM_ASM`); `web/shell.html` clamps the canvas so it never drops to
  0x0; web present renders at the game resolution and lets CSS letterbox; vsync
  is disabled on the web (`SDL_GL_SetSwapInterval(0)`) because vsync needs a
  registered emscripten main loop.
- [ ] **Game renders only one frame, then stalls.** Current blocker. With SE
  data the sequence is: `setVideoMode 640x480` → `unlock(0)` → `present #1` →
  then no further `unlock`/`swap`/`present`. Verified with logging that SDL
  timers *do* fire continuously and worker threads stay alive but mostly blocked
  (low syscall activity), and keypresses do not trigger a redraw. So the frontend
  is not an interactive static menu waiting on input — it is stuck early (note
  `joycal.cfg` is also missing) in intro/attract or device init, waiting on a
  synchronization primitive or a DirectDraw flip-complete signal that the port
  does not raise. Next: trace `winapp.cpp`'s message pump + the DirectDraw
  `Flip`/`unlock` path and the thread that owns the frontend to find what it
  blocks on. Whether the single presented frame actually composites (vs. being a
  genuinely black first frame) can only be confirmed once the game advances far
  enough to draw non-black content.
- [ ] Networking: `wsock32`/sockets stubs for browser (multiplayer disabled).
- [ ] Runtime FS: file-picker → IDBFS so users can supply data without rebuild.
- [ ] Verify GLES shader output matches desktop (palette byte order, blit V flip).
