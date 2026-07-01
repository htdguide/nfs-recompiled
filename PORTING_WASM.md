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
- [ ] **Present frames to the visible canvas.** Open issue: under
  `PROXY_TO_PTHREAD`, SDL3's port never sizes the page canvas (it stays 0x0) and
  the `OFFSCREEN_FRAMEBUFFER` frames rendered on the worker are not composited to
  it, so the canvas is black even though the game is running. Calling
  `emscripten_set_canvas_element_size("#canvas", …)` from the worker in
  `Renderer::setVideoMode` does not take effect across the thread boundary.
  Likely needs a fix in how `SDL_GL_SwapWindow` commits the proxied frame, or a
  patched SDL3 build. This is the main remaining blocker to seeing the menu.
- [ ] Networking: `wsock32`/sockets stubs for browser (multiplayer disabled).
- [ ] Runtime FS: file-picker → IDBFS so users can supply data without rebuild.
- [ ] Verify GLES shader output matches desktop (palette byte order, blit V flip).
