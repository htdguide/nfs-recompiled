# WebAssembly port (browser)

This directory contains the browser front-end for the WebAssembly build of
`nfs-recompiled`. The recompiled game is compiled to `.wasm` with
[Emscripten](https://emscripten.org/) and rendered through WebGL 2.

> Status: **early port.** It builds and boots in the browser. You still need the
> original game data to see anything, exactly like the native build.

## Prerequisites

- The Emscripten SDK (`emsdk`) installed and activated:
  ```bash
  git clone https://github.com/emscripten-core/emsdk
  cd emsdk && ./emsdk install latest && ./emsdk activate latest
  source ./emsdk_env.sh
  ```
- CMake ≥ 3.15, Python 3.

## Building

Configure with the `emcmake` wrapper so CMake picks up the Emscripten
toolchain, then build a game target (`nfs2se` or `nfs3hp`):

```bash
source /path/to/emsdk/emsdk_env.sh

# Optionally bake the game data into the .data bundle at configure time:
#   -DNFS_DATA_DIR=/absolute/path/to/gamefiles
emcmake cmake -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web --target nfs2se -j4
```

Each game target emits `build-web/<name>.html`, `<name>.js`, `<name>.wasm`
and (if data was preloaded) `<name>.data`.

## Running

WebAssembly threads require `SharedArrayBuffer`, which browsers only expose to
[cross-origin isolated](https://web.dev/coop-coep/) pages. Serve with the
included helper, which sets the required COOP/COEP headers:

```bash
python3 web/serve.py 8080 build-web
# then open http://localhost:8080/nfs2se.html
```

Opening the `.html` directly from `file://` will **not** work (no COOP/COEP,
no cross-origin isolation).

## Game data

The game needs the original `Fedata` / `GameData` files (see the top-level
README). Two ways to provide them:

1. **Bake at build time** — pass `-DNFS_DATA_DIR=/path/to/gamefiles` at configure
   time. The files are preloaded into Emscripten's virtual FS at `/data`, and the
   HTML shell launches the game with `/data` as both the install and CD path.
   The directory must contain `FEDATA/`, `GAMEDATA/`, `install.win` (SE CDs ship
   it as `INSTALL.NFS` — rename it), and `NFS2SEN.EXE` — the game's CD check
   verifies its own executable exists on the "CD" and quits without it.
2. **Mount at runtime** — extend `web/shell.html` to populate MEMFS/IDBFS before
   the module starts (e.g. from a file picker). Not wired up yet.

## What was changed for the web target

See [`../PORTING_WASM.md`](../PORTING_WASM.md) for the full list. In short:

- `CMakeLists.txt` — Emscripten branch: SDL3 port (`-sUSE_SDL=3`), WebGL2/GLES3,
  pthreads + `PROXY_TO_PTHREAD`, SSE→WASM SIMD, `--preload-file`, HTML shell.
- `src/lib/renderer.cpp` — fixed-function fullscreen blit rewritten as a GLES2
  shader (WebGL has no immediate mode); `GL_UNSIGNED_INT_8_8_8_8` path adapted.
- `src/lib/gliderenderer.cpp` — GLSL `#version 400` shaders ported to
  `#version 300 es`.
- `include/x86.h` — `asm("int3")` assert trap replaced with `__builtin_trap()`
  on wasm.
