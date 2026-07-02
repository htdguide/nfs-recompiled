#include <lib/renderer.h>
#include <lib/window.h>
#include <lib/gamepad.h>
#include <SDL3/SDL.h>
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <emscripten.h>
#else
#include <SDL3/SDL_opengl.h>
#include <GL/gl.h>
#endif

namespace win32
{

#ifndef __EMSCRIPTEN__
void GLAPIENTRY errorCallback(GLenum /*source*/, GLenum type, GLuint /*id*/, GLenum severity, GLsizei /*length*/, const GLchar* message, const void* /*userParam*/)
{
    if (severity != GL_DEBUG_SEVERITY_NOTIFICATION)
    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "OpenGL: %s type = 0x%x, severity = 0x%x, \"%s\"\n",
            (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""),
            type, severity, message);
}
PFNGLDEBUGMESSAGECALLBACKPROC glDebugMessageCallback;
#endif

#ifdef __EMSCRIPTEN__
// ---------------------------------------------------------------------------
// WebGL fullscreen blit. Desktop GL uses fixed-function glBegin/glOrtho to draw
// the software-rendered frame; WebGL has no immediate mode, so we lazily build
// a tiny shader + triangle-strip quad and reuse it every present().
// ---------------------------------------------------------------------------
namespace
{
GLuint s_blitProgram = 0;
GLuint s_blitVbo     = 0;
GLint  s_blitPosLoc  = -1;
GLint  s_blitUvLoc   = -1;

GLuint compileBlitShader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "blit shader: %s", log);
    }
    return sh;
}

void ensureBlitProgram()
{
    if (s_blitProgram)
        return;

    static const char* vs =
        "attribute vec2 a_pos;"
        "attribute vec2 a_uv;"
        "varying vec2 v_uv;"
        "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }";
    static const char* fs =
        "precision mediump float;"
        "varying vec2 v_uv;"
        "uniform sampler2D u_tex;"
        "void main(){ gl_FragColor = texture2D(u_tex, v_uv); }";

    s_blitProgram = glCreateProgram();
    GLuint v = compileBlitShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileBlitShader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(s_blitProgram, v);
    glAttachShader(s_blitProgram, f);
    glLinkProgram(s_blitProgram);
    s_blitPosLoc = glGetAttribLocation(s_blitProgram, "a_pos");
    s_blitUvLoc  = glGetAttribLocation(s_blitProgram, "a_uv");

    // Fullscreen triangle strip. UV v is flipped so the game's top-left texture
    // origin maps to the top of the viewport (desktop used glOrtho(0,w,h,0)).
    static const float quad[] = {
        //  x     y     u     v
        -1.f, -1.f, 0.f, 1.f,
         1.f, -1.f, 1.f, 1.f,
        -1.f,  1.f, 0.f, 0.f,
         1.f,  1.f, 1.f, 0.f,
    };
    glGenBuffers(1, &s_blitVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_blitVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
}
} // namespace
#endif

Renderer::Renderer(WinApplication* application, Window *window)
    :   m_application(application)
    ,   m_window(window)
    ,   m_renderer(SDL_GL_CreateContext(m_window->m_window))
    ,   m_videoMemory(new MemMap(800*600*2*2)) // double buffer 16 bits 800x600
    ,   m_currentBuffer(0)
    ,   m_depth(16)
    ,   m_colorPalette()
{
    setCurrent();
#ifdef __EMSCRIPTEN__
    SDL_GL_SetSwapInterval(0);
#else
    SDL_GL_SetSwapInterval(1);
#endif
#ifndef __EMSCRIPTEN__
    // Debug output and fixed-function GL_TEXTURE_2D enable do not exist in
    // WebGL/GLES; texturing is driven entirely by the shader on that path.
    glDebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKPROC)SDL_GL_GetProcAddress("glDebugMessageCallback");
    if (glDebugMessageCallback)
    {
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(errorCallback, 0);
    }
#endif

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
#ifndef __EMSCRIPTEN__
    glEnable(GL_TEXTURE_2D);
#endif

    clearCurrent();
}

Renderer::~Renderer()
{
    SDL_GL_DestroyContext(static_cast<SDL_GLContext>(m_renderer));
}

void Renderer::setCurrent()
{
    SDL_GL_MakeCurrent(m_window->m_window, static_cast<SDL_GLContext>(m_renderer));
}

void Renderer::clearCurrent()
{
    SDL_GL_MakeCurrent(m_window->m_window, nullptr);
}

void Renderer::setVideoMode(x86::reg32 w, x86::reg32 h, x86::reg32 bpp)
{
    m_width = w;
    m_height = h;
    m_depth = bpp;
#ifdef __EMSCRIPTEN__
    // Under -sPROXY_TO_PTHREAD SDL does not size the page canvas, so both it and
    // the OFFSCREEN_FRAMEBUFFER back buffer stay 0x0 and nothing composites.
    // Size them to the game's video mode explicitly (proxied to the main thread).
    // Size the emscripten offscreen back buffer (proxied GL target)...
    EMSCRIPTEN_RESULT r = emscripten_set_canvas_element_size("#canvas", int(w), int(h));
    // ...and the actual DOM canvas backing store on the main thread. The proxied
    // call above only resizes the offscreen buffer; the DOM canvas the browser
    // composites stays 0x0 unless we set it here, which leaves the screen black.
    MAIN_THREAD_EM_ASM({
        var c = Module['canvas'];
        if (c) { c.width = $0; c.height = $1; }
    }, int(w), int(h));
    (void)r;
#endif
    setCurrent();
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_width, m_height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, nullptr);
    clearCurrent();
    //glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, nullptr);
}

void Renderer::updatePalette(x86::reg32 colorCount, const x86::reg32* colors)
{
    NFS2_ASSERT(colorCount <= 256);
    memcpy(m_colorPalette, colors, colorCount*4);
}

x86::reg32 Renderer::getFrontBuffer() const
{
    return m_videoMemory->getBlockStart() + m_currentBuffer * m_width * m_height * 2;
}

x86::reg32 Renderer::getBackBuffer() const
{
    return m_videoMemory->getBlockStart() + (1 - m_currentBuffer) * m_width * m_height * 2;
}

void Renderer::present()
{
    Gamepad::updateKeys();
    glBindTexture(GL_TEXTURE_2D, m_texture);
    int w, h;
    SDL_GetWindowSizeInPixels(m_window->m_window, &w, &h);
#ifdef __EMSCRIPTEN__
    // The page canvas backing store is sized to the game resolution (see
    // setVideoMode) and CSS "object-fit: contain" letterboxes it to the display,
    // so render to the whole backing store; SDL's window size is the CSS size and
    // must not be used here.
    w = int(m_width);
    h = int(m_height);
#endif

    float gameAspect = float(m_width) / float(m_height);
    float windowAspect = float(w) / float(h);
    int vpX, vpY, vpW, vpH;
    if (windowAspect > gameAspect)
    {
        vpH = h;
        vpW = int(h * gameAspect + 0.5f);
        vpX = (w - vpW) / 2;
        vpY = 0;
    }
    else
    {
        vpW = w;
        vpH = int(w / gameAspect + 0.5f);
        vpX = 0;
        vpY = (h - vpH) / 2;
    }

    glViewport(0, 0, w, h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Render the game texture into the aspect-ratio-correct viewport
    glViewport(vpX, vpY, vpW, vpH);
#ifdef __EMSCRIPTEN__
    ensureBlitProgram();
    glUseProgram(s_blitProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glBindBuffer(GL_ARRAY_BUFFER, s_blitVbo);
    glEnableVertexAttribArray(GLuint(s_blitPosLoc));
    glVertexAttribPointer(GLuint(s_blitPosLoc), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(GLuint(s_blitUvLoc));
    glVertexAttribPointer(GLuint(s_blitUvLoc), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFlush();
    SDL_GL_SwapWindow(m_window->m_window);
    // With -sOFFSCREEN_FRAMEBUFFER the drawing goes to an offscreen buffer; it
    // only reaches the visible canvas when the frame is explicitly committed.
    // SDL_GL_SwapWindow does not do this under the proxied-to-pthread context.
    emscripten_webgl_commit_frame();
#else
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, m_width, m_height, 0, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glBegin(GL_TRIANGLES);
        glTexCoord2f(0.f, 0.f);
        glVertex3f(0.f, 0.f, 1.f);
        glTexCoord2f(1.f, 0.f);
        glVertex3f(GLfloat(m_width), 0.f, 1.f);
        glTexCoord2f(0.f, 1.f);
        glVertex3f(0.f, GLfloat(m_height), 1.f);

        glTexCoord2f(1.f, 0.f);
        glVertex3f(GLfloat(m_width), 0.f, 1.f);
        glTexCoord2f(1.f, 1.f);
        glVertex3f(GLfloat(m_width), GLfloat(m_height), 1.f);
        glTexCoord2f(0.f, 1.f);
        glVertex3f(0.f, GLfloat(m_height), 1.f);
    glEnd();
    glFlush();
    SDL_GL_SwapWindow(m_window->m_window);
#endif
}

void Renderer::update()
{
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (m_depth == 16)
    {
        //MemMap::fillDebugGraph(&m_application->getMemory<x86::reg16>(getFrontBuffer()) + m_width*2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_width, m_height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, &m_application->getMemory<x86::reg16>(getFrontBuffer()));
    }
    else
    {
        x86::reg8* srcData = &m_application->getMemory<x86::reg8>(getFrontBuffer());
        x86::reg32* screenData = reinterpret_cast<x86::reg32*>(malloc(m_width * m_height * 4));
        for (x86::reg32 i = 0; i < m_width*m_height; ++i)
        {
            screenData[i] = m_colorPalette[srcData[i]];
        }
        //MemMap::fillDebugGraph(screenData + m_width*2);
#ifdef __EMSCRIPTEN__
        // WebGL/GLES lacks GL_UNSIGNED_INT_8_8_8_8. Byte-swap each pixel so the
        // resulting little-endian byte order (R,G,B,A) matches what desktop GL
        // produced from the packed uint, then upload as plain UNSIGNED_BYTE.
        for (x86::reg32 i = 0; i < m_width*m_height; ++i)
        {
            x86::reg32 p = screenData[i];
            screenData[i] = ((p & 0xFF000000u) >> 24) | ((p & 0x00FF0000u) >> 8)
                          | ((p & 0x0000FF00u) << 8)  | ((p & 0x000000FFu) << 24);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, screenData);
#else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, screenData);
#endif
        free(screenData);
    }
    present();
}

x86::reg32 Renderer::lock(x86::reg32 index)
{
    return index == 0 ? getFrontBuffer() : getBackBuffer();
}

void Renderer::unlock(x86::reg32 index)
{
    if (index == 0)
    {
        setCurrent();
        SDL_GL_SetSwapInterval(0);
        update();
        clearCurrent();
    }
}

void Renderer::swap()
{
    setCurrent();
#ifdef __EMSCRIPTEN__
    // No vsync on the web: SDL_GL_SetSwapInterval(1) needs a registered
    // emscripten main loop (hence the "no main loop" warnings) and can wedge the
    // render thread after the first frame. The browser composites the canvas on
    // its own schedule, so present unthrottled and let the game pace itself.
    SDL_GL_SetSwapInterval(0);
    m_currentBuffer = 1 - m_currentBuffer;
    update();
#else
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    SDL_GL_SetSwapInterval(1);
    m_currentBuffer = 1 - m_currentBuffer;
    for (int i = 0; i < (mode ? (int)(mode->refresh_rate / 30) : 2); ++i)
    {
        // We enabled vsync and now render until we have a refresh rate of 30
        update();
    }
#endif
    clearCurrent();
}

}
