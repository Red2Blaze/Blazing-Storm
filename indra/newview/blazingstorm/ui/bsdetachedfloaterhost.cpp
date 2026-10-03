/**
 * @file bsdetachedfloaterhost.cpp
 * @brief Native detached-window host for Blazing Storm floaters.
 *
 * Phase one intentionally creates a non-GL native host. This proves independent
 * window lifetime without allowing a secondary LLWindow to tear down the
 * viewer's global GL state. Rendering/input forwarding is layered on next.
 */
#include "llviewerprecompiledheaders.h"

#include "bsdetachedfloaterhost.h"

#include "llfloater.h"
#include "llview.h"
#include "llrender.h"
#include "llui.h"
#include "llviewerwindow.h"
#include "llglslshader.h"
#include "pipeline.h"

#ifdef LL_WINDOWS
# include <windows.h>
#endif

BSDetachedFloaterHost::BSDetachedFloaterHost() = default;

BSDetachedFloaterHost::~BSDetachedFloaterHost()
{
    attach();
}

bool BSDetachedFloaterHost::detach(LLFloater* floater, const std::string& title)
{
    if (!floater)
    {
        return false;
    }

    if (mFloater == floater)
    {
        return true;
    }

    // MVP supports one detached floater. Reattach an existing one first.
    attach();

#ifdef LL_WINDOWS
    if (!createNativeWindow(title))
    {
        return false;
    }

    mFloater = floater;
    LL_INFOS("DetachedFloaters") << "Opened native host for " << floater->getName() << LL_ENDL;
    return true;
#else
    LL_WARNS("DetachedFloaters") << "Native detached hosts are currently Windows-only." << LL_ENDL;
    return false;
#endif
}

void BSDetachedFloaterHost::attach()
{
#ifdef LL_WINDOWS
    destroyNativeWindow();
#endif
    mFloater = nullptr;
}

#ifdef LL_WINDOWS
namespace
{
    const wchar_t* const BS_DETACHED_WINDOW_CLASS = L"BlazingStormDetachedFloater";
}

bool BSDetachedFloaterHost::createNativeWindow(const std::string& title)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&BSDetachedFloaterHost::windowProc);
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = BS_DETACHED_WINDOW_CLASS;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LL_WARNS("DetachedFloaters") << "RegisterClassExW failed: " << GetLastError() << LL_ENDL;
        return false;
    }

    const int title_chars = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::wstring wide_title;
    if (title_chars > 0)
    {
        wide_title.resize(static_cast<size_t>(title_chars));
        MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, &wide_title[0], title_chars);
    }
    if (wide_title.empty())
    {
        wide_title = L"Blazing Storm";
    }
    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        BS_DETACHED_WINDOW_CLASS,
        wide_title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 420,
        nullptr, nullptr, instance, this);

    if (!hwnd)
    {
        LL_WARNS("DetachedFloaters") << "CreateWindowExW failed: " << GetLastError() << LL_ENDL;
        return false;
    }

    mNativeWindow = hwnd;
    if (!createGLSurface())
    {
        DestroyWindow(hwnd);
        mNativeWindow = nullptr;
        return false;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}

void BSDetachedFloaterHost::destroyNativeWindow()
{
    destroyGLSurface();
    if (mNativeWindow)
    {
        HWND hwnd = static_cast<HWND>(mNativeWindow);
        mNativeWindow = nullptr;
        if (IsWindow(hwnd))
        {
            DestroyWindow(hwnd);
        }
    }
}


bool BSDetachedFloaterHost::createGLSurface()
{
    HWND hwnd = static_cast<HWND>(mNativeWindow);
    HDC dc = GetDC(hwnd);
    if (!dc)
    {
        return false;
    }

    // Match the main viewer pixel format. This is required for WGL resource
    // sharing and avoids running the viewer's global GL initialization again.
    HDC main_dc = wglGetCurrentDC();
    HGLRC main_rc = wglGetCurrentContext();
    const int pixel_format = main_dc ? GetPixelFormat(main_dc) : 0;
    PIXELFORMATDESCRIPTOR pfd = {};

    if (!main_dc || !main_rc || pixel_format == 0 ||
        !DescribePixelFormat(main_dc, pixel_format, sizeof(pfd), &pfd) ||
        !SetPixelFormat(dc, pixel_format, &pfd))
    {
        ReleaseDC(hwnd, dc);
        LL_WARNS("DetachedFloaters") << "Unable to match the viewer OpenGL pixel format." << LL_ENDL;
        return false;
    }

    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglShareLists(main_rc, rc))
    {
        if (rc)
        {
            wglDeleteContext(rc);
        }
        ReleaseDC(hwnd, dc);
        LL_WARNS("DetachedFloaters") << "Unable to create shared OpenGL context." << LL_ENDL;
        return false;
    }

    mNativeDC = dc;
    mGLContext = rc;
    return true;
}

void BSDetachedFloaterHost::destroyGLSurface()
{
    if (mGLContext)
    {
        HGLRC rc = static_cast<HGLRC>(mGLContext);
        if (wglGetCurrentContext() == rc)
        {
            wglMakeCurrent(nullptr, nullptr);
        }
        wglDeleteContext(rc);
        mGLContext = nullptr;
    }

    if (mNativeDC && mNativeWindow)
    {
        ReleaseDC(static_cast<HWND>(mNativeWindow), static_cast<HDC>(mNativeDC));
        mNativeDC = nullptr;
    }
}

void BSDetachedFloaterHost::draw()
{
    if (!mFloater || !mNativeWindow || !mNativeDC || !mGLContext ||
        !IsWindowVisible(static_cast<HWND>(mNativeWindow)) ||
        IsIconic(static_cast<HWND>(mNativeWindow)))
    {
        return;
    }

    HDC previous_dc = wglGetCurrentDC();
    HGLRC previous_rc = wglGetCurrentContext();

    HDC dc = static_cast<HDC>(mNativeDC);
    HGLRC rc = static_cast<HGLRC>(mGLContext);
    if (!wglMakeCurrent(dc, rc))
    {
        return;
    }

    RECT client = {};
    GetClientRect(static_cast<HWND>(mNativeWindow), &client);
    const S32 width = llmax(1L, client.right - client.left);
    const S32 height = llmax(1L, client.bottom - client.top);

    glViewport(0, 0, width, height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Set up a simple UI projection for this HWND, then draw only the floater.
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadIdentity();
    gGL.ortho(0.f, (F32)width, 0.f, (F32)height, -1.f, 1.f);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadIdentity();

    const LLRect old_rect = mFloater->getRect();
    mFloater->reshape(width, height, false);
    mFloater->setOrigin(0, 0);

    gUIProgram.bind();
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    LLView::sIsDrawing = true;
    mFloater->draw();
    LLView::sIsDrawing = false;
    gGL.flush();
    gUIProgram.unbind();

    // Drawing must not permanently alter the floater's in-viewer geometry.
    mFloater->setRect(old_rect);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);

    SwapBuffers(dc);
    wglMakeCurrent(previous_dc, previous_rc);
}

long long __stdcall BSDetachedFloaterHost::windowProc(void* raw_hwnd, unsigned int message,
                                                       unsigned long long wparam, long long lparam)
{
    HWND hwnd = static_cast<HWND>(raw_hwnd);
    BSDetachedFloaterHost* self =
        reinterpret_cast<BSDetachedFloaterHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<BSDetachedFloaterHost*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    switch (message)
    {
        case WM_CLOSE:
            // Do not route this through LLViewerWindow: closing a detached
            // window must never request application shutdown.
            if (self)
            {
                self->attach();
            }
            return 0;

        case WM_DESTROY:
            if (self && self->mNativeWindow == hwnd)
            {
                self->mNativeWindow = nullptr;
                self->mFloater = nullptr;
            }
            return 0;

        default:
            break;
    }

    return static_cast<long long>(DefWindowProcW(hwnd, message,
        static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam)));
}
#endif
