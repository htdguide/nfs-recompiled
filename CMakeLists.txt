cmake_minimum_required(VERSION 3.15)
project(nfs_recompiled LANGUAGES C CXX ASM)

# options
option(WITH_PEDANTIC_FPU "Compile pedantic FPU implementation" OFF)
option(WITH_MMX "Enable MMX support" ON)

# add compile definitions based on options
if(WITH_PEDANTIC_FPU)
    add_compile_definitions(WITH_PEDANTIC_FPU)
endif()
if(WITH_MMX)
    add_compile_definitions(WITH_MMX)
endif()

# Detect Windows target: covers native Windows (WIN32/MINGW), an explicit
# -DCMAKE_SYSTEM_NAME=Windows, and cross-compilation via a MinGW toolchain
# supplied directly with -DCMAKE_C_COMPILER=…/x86_64-w64-mingw32-gcc (in
# which case WIN32/MINGW may not yet be set if no toolchain file is used).
if(WIN32 OR MINGW OR CMAKE_SYSTEM_NAME STREQUAL "Windows"
        OR CMAKE_C_COMPILER MATCHES "mingw"
        OR CMAKE_CXX_COMPILER MATCHES "mingw")
    set(TARGET_WINDOWS TRUE)
else()
    set(TARGET_WINDOWS FALSE)
endif()

# Detect the Emscripten/WebAssembly target. CMake sets EMSCRIPTEN when the
# project is configured with the emcmake wrapper (or the Emscripten toolchain
# file). On this target we render through WebGL 2 and rely on the bundled
# SDL3 emscripten port instead of a system SDL3.
if(EMSCRIPTEN)
    set(TARGET_WEB TRUE)
    add_compile_definitions(TARGET_WEB __EMSCRIPTEN__)
else()
    set(TARGET_WEB FALSE)
endif()

if (MSVC)
    add_compile_options(/W4 /WX)
elseif(TARGET_WEB)
    # The generated disassembly is huge and trips a slightly different clang
    # warning set under emscripten; keep warnings but do not fail the build.
    add_compile_options(-Wall -Wextra -Wstrict-aliasing=2 -Wno-unused-command-line-argument)
else()
    add_compile_options(-Wall -Wextra -Werror -Wstrict-aliasing=2)
endif()

# When building for Windows we have a local copy of SDL3 under the
# `sdl3` subdirectory.  Point CMake at it so find_package() will
# locate the bundled config file automatically.
if(TARGET_WINDOWS)
    message(STATUS "Windows target detected – adding ${CMAKE_SOURCE_DIR}/sdl3/cmake to CMAKE_PREFIX_PATH for SDL3")
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/sdl3/cmake")
    set(SDL3_DIR "${CMAKE_SOURCE_DIR}/sdl3/cmake" CACHE PATH "SDL3 CMake directory")
endif()

if(TARGET_WEB)
    # ---------------------------------------------------------------------
    # WebAssembly / Emscripten backend
    # ---------------------------------------------------------------------
    # SDL3 and OpenGL ES/WebGL are provided by the Emscripten SDL3 port; there
    # is nothing to find_package(). We expose an INTERFACE target that carries
    # the flags every translation unit needs (SDL3 headers + pthreads), and a
    # matching empty GL target so the rest of the file stays backend-agnostic.
    add_library(nfs_web_flags INTERFACE)
    # -msimd128 lets emscripten emulate the SSE/MMX intrinsics used by mmx.h and
    # the recompiled code (immintrin.h) on top of WebAssembly SIMD.
    target_compile_options(nfs_web_flags INTERFACE
        "-sUSE_SDL=3" "-pthread" "-msimd128"
        "-msse" "-msse2" "-msse3" "-mssse3" "-msse4.1")
    target_link_options(nfs_web_flags INTERFACE "-sUSE_SDL=3" "-pthread")

    set(SDL3_LIB_TARGET  nfs_web_flags)
    set(SDL3_MAIN_TARGET "")
    add_library(nfs_gl_target INTERFACE)   # GL is folded into the SDL3 port
    set(GL_LIB_TARGET    nfs_gl_target)
else()
    find_package(SDL3 REQUIRED)
    find_package(OpenGL REQUIRED)

    # prefer modern imported targets if available
    if(TARGET SDL3::SDL3)
        set(SDL3_LIB_TARGET SDL3::SDL3)
    else()
        include_directories(${SDL3_INCLUDE_DIRS})
        set(SDL3_LIB_TARGET ${SDL3_LIBRARIES})
    endif()

    # SDL3main provides the WinMain→main entry-point shim required on Windows.
    # The modern config exposes it as a separate target; the legacy
    # FindSDL3.cmake already folds it into SDL3_LIBRARIES, so we only add it on
    # top for the imported-target path.
    if(TARGET SDL3::SDL3main)
        set(SDL3_MAIN_TARGET SDL3::SDL3main)
    else()
        set(SDL3_MAIN_TARGET "")
    endif()

    set(GL_LIB_TARGET OpenGL::GL)
endif()

file(GLOB NFS2SE_DISASM_SOURCES
     "${CMAKE_SOURCE_DIR}/src/nfs2se/disassembly/*.cpp")
file(GLOB NFS2SE_DISASM_HEADERS
     "${CMAKE_SOURCE_DIR}/src/nfs2se/disassembly/*.h")

file(GLOB NFS3HP_DISASM_SOURCES
     "${CMAKE_SOURCE_DIR}/src/nfs3hp/disassembly/*.cpp")
file(GLOB NFS3HP_DISASM_HEADERS
     "${CMAKE_SOURCE_DIR}/src/nfs3hp/disassembly/*.h")

# ---------------------------------------------------------------------------
# core static library
# ---------------------------------------------------------------------------

file(GLOB LIB_CPP_SOURCES
     "${CMAKE_SOURCE_DIR}/src/lib/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/lib/sdl-backend/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/lib/winapi/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/lib/winapi/ddraw/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/lib/winapi/dinput/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/lib/winapi/dsound/*.cpp")

# x87.asm provides 80-bit extended-precision FPU helpers and is only needed
# (and only supported) on Linux with WITH_PEDANTIC_FPU=ON.
if(WITH_PEDANTIC_FPU AND TARGET_WINDOWS)
    message(FATAL_ERROR
        "WITH_PEDANTIC_FPU is not supported on Windows: the x87 NASM helper "
        "relies on Linux-specific 80-bit long double ABI conventions.")
endif()

if(WITH_PEDANTIC_FPU AND NOT TARGET_WINDOWS)
    set(X87_OBJ "${CMAKE_CURRENT_BINARY_DIR}/x87.o")
    add_custom_command(
        OUTPUT ${X87_OBJ}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND nasm -f elf64 -o ${X87_OBJ} ${CMAKE_SOURCE_DIR}/src/lib/x87.asm
        DEPENDS ${CMAKE_SOURCE_DIR}/src/lib/x87.asm
        COMMENT "Assembling x87.asm (elf64, pedantic FPU)"
        VERBATIM
    )
    list(APPEND LIB_CPP_SOURCES ${X87_OBJ})
endif()

add_library(nfs_core STATIC ${LIB_CPP_SOURCES})
target_include_directories(nfs_core PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(nfs_core PUBLIC ${SDL3_MAIN_TARGET} ${SDL3_LIB_TARGET} ${GL_LIB_TARGET})

# helper function for adding the game executables
function(add_game_exe name)
    # script and directories
    set(game_src_dir "${CMAKE_SOURCE_DIR}/src/${name}")

    # collect sources
    file(GLOB GAME_SOURCES "${game_src_dir}/*.cpp")

    # pick the hardcoded disasm set for this game
    if(name STREQUAL "nfs2se")
        set(DISASM_SOURCES ${NFS2SE_DISASM_SOURCES})
        set(DISASM_HEADERS ${NFS2SE_DISASM_HEADERS})
    elseif(name STREQUAL "nfs3hp")
        set(DISASM_SOURCES ${NFS3HP_DISASM_SOURCES})
        set(DISASM_HEADERS ${NFS3HP_DISASM_HEADERS})
    else()
        set(DISASM_SOURCES "")
        set(DISASM_HEADERS "")
    endif()

    # adding the headers to the executable source list is what makes CMake
    # aware that the custom command must run before compilation starts.
    add_executable(${name} ${GAME_SOURCES} ${DISASM_SOURCES} ${DISASM_HEADERS})
    set_source_files_properties(${DISASM_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
    target_include_directories(${name} PRIVATE
        ${game_src_dir}
        ${CMAKE_SOURCE_DIR}/src/${name}/disassembly
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(${name} PRIVATE ${SDL3_MAIN_TARGET} nfs_core ${SDL3_LIB_TARGET} ${GL_LIB_TARGET})
    if(WITH_PEDANTIC_FPU)
        target_compile_definitions(${name} PRIVATE WITH_PEDANTIC_FPU)
    endif()
    if(WITH_MMX)
        target_compile_definitions(${name} PRIVATE WITH_MMX)
    endif()

    if(TARGET_WEB)
        # Emit a ready-to-serve .html shell alongside the .js/.wasm/.data.
        set_target_properties(${name} PROPERTIES SUFFIX ".html")

        # Optional game data directory to bake into the virtual filesystem.
        # Pass -DNFS_DATA_DIR=/abs/path/to/gamefiles at configure time.
        set(NFS_PRELOAD_OPTS "")
        if(NFS_DATA_DIR AND EXISTS "${NFS_DATA_DIR}")
            message(STATUS "${name}: preloading game data from ${NFS_DATA_DIR}")
            list(APPEND NFS_PRELOAD_OPTS "SHELL:--preload-file ${NFS_DATA_DIR}@/data")
        else()
            message(STATUS "${name}: no NFS_DATA_DIR set — build will need data mounted at runtime")
        endif()

        target_link_options(${name} PRIVATE
            # main() (window + the blocking SDL_WaitEvent message pump) stays on
            # the browser main thread: SDL3's Emscripten backend touches `document`
            # from the calling thread during video/event init, which only exists
            # there. ASYNCIFY lets the blocking pump yield to the browser event
            # loop; the game's own worker threads (CPU + GL) run as real pthreads.
            "-sEXIT_RUNTIME=0"
            # threads: the game creates a handful of workers on demand.
            "-sPTHREAD_POOL_SIZE=8"
            "-sPTHREAD_POOL_SIZE_STRICT=0"
            # WebGL 2 with GLES3 emulation for the shader + immediate paths.
            "-sMIN_WEBGL_VERSION=2"
            "-sMAX_WEBGL_VERSION=2"
            "-sFULL_ES3=1"
            "-sGL_ENABLE_GET_PROC_ADDRESS=1"
            # The game creates and drives its GL context from a worker pthread,
            # but the WebGL context lives on the browser main thread. This routes
            # GL calls from the worker to the main-thread context.
            "-sOFFSCREEN_FRAMEBUFFER=1"
            # memory: the recompiled game maps a large flat address space.
            "-sALLOW_MEMORY_GROWTH=1"
            "-sINITIAL_MEMORY=536870912"
            "-sSTACK_SIZE=8388608"
            "-sASYNCIFY"
            "-sASSERTIONS=1"
            "SHELL:--shell-file ${CMAKE_SOURCE_DIR}/web/shell.html"
            ${NFS_PRELOAD_OPTS})
    endif()
endfunction()

add_game_exe(nfs2se)
add_game_exe(nfs3hp)