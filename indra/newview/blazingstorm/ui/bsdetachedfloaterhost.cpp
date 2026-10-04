/**
 * @file bsdetachedfloaterhost.cpp
 * @brief Native detached-window host for Blazing Storm floaters.
 */
#include "llviewerprecompiledheaders.h"

#include "bsdetachedfloaterhost.h"

#include "llfloater.h"
#include "llview.h"
#include "llrender.h"
#include "llui.h"
#include "llviewerwindow.h"
#include "llglslshader.h"
#include "llkeyboard.h"
#include "pipeline.h"

#ifdef LL_WINDOWS
# include <windows.h>
# include <windowsx.h>
# include "llkeyboardwin32.h"
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

    // MVP supports one native detached root at a time.
    attach();

#ifdef LL_WINDOWS
    if (!createNativeWindow(title))
    {
        return false;
    }

    mFloater = floater;
    mOriginalParent = floater->getParent();
    mOriginalRect = floater->getRect();

    // This is a real detach: remove the floater from the viewer's normal
    // hierarchy so it is not also drawn/interacted with in the main window.
    if (mOriginalParent)
    {
        mOriginalParent->removeChild(floater);
    }

    LL_INFOS("DetachedFloaters") << "Opened native host for "
                                  << floater->getName() << LL_ENDL;
    return true;
#else
    LL_WARNS("DetachedFloaters")
        << "Native detached hosts are currently Windows-only." << LL_ENDL;
    return false;
#endif
}

void BSDetachedFloaterHost::attach()
{
    LLFloater* floater = mFloater;
    LLView* parent = mOriginalParent;
    const LLRect original_rect = mOriginalRect;

    // Clear these before destroying the native window because DestroyWindow()
    // synchronously sends WM_DESTROY back through windowProc().
    mFloater = nullptr;
    mOriginalParent = nullptr;

#ifdef LL_WINDOWS
    destroyNativeWindow();
#endif

    if (floater && parent)
    {
        parent->addChild(floater);
        floater->setRect(original_rect);
        parent->sendChildToFront(floater);
    }
}

#ifdef LL_WINDOWS
namespace
{
    const wchar_t* const BS_DETACHED_WINDOW_CLASS = L"BlazingStormDetachedFloater";

    MASK detachedMask(bool for_mouse)
    {
        MASK mask = MASK_NONE;
        if (GetKeyState(VK_SHIFT) & 0x8000)
        {
            mask |= MASK_SHIFT;
        }
        if (GetKeyState(VK_CONTROL) & 0x8000)
        {
            mask |= MASK_CONTROL;
        }
        if (GetKeyState(VK_MENU) & 0x8000)
        {
            mask |= MASK_ALT;
        }
        (void)for_mouse;
        return mask;
    }
}

bool BSDetachedFloaterHost::createNativeWindow(const std::string& title)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&BSDetachedFloaterHost::windowProc);
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = BS_DETACHED_WINDOW_CLASS;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LL_WARNS("DetachedFloaters") << "RegisterClassExW failed: "
                                      << GetLastError() << LL_ENDL;
        return false;
    }

    const int title_chars =
        MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::wstring wide_title;
    if (title_chars > 0)
    {
        wide_title.resize(static_cast<size_t>(title_chars));
        MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1,
                            &wide_title[0], title_chars);
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
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 500,
        nullptr, nullptr, instance, this);

    if (!hwnd)
    {
        LL_WARNS("DetachedFloaters") << "CreateWindowExW failed: "
                                      << GetLastError() << LL_ENDL;
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

    // Match the main viewer pixel format. This lets the detached context share
    // the viewer's textures/program objects without invoking global GL init.
    HDC main_dc = wglGetCurrentDC();
    HGLRC main_rc = wglGetCurrentContext();
    const int pixel_format = main_dc ? GetPixelFormat(main_dc) : 0;
    PIXELFORMATDESCRIPTOR pfd = {};

    if (!main_dc || !main_rc || pixel_format == 0 ||
        !DescribePixelFormat(main_dc, pixel_format, sizeof(pfd), &pfd) ||
        !SetPixelFormat(dc, pixel_format, &pfd))
    {
        ReleaseDC(hwnd, dc);
        LL_WARNS("DetachedFloaters")
            << "Unable to match the viewer OpenGL pixel format." << LL_ENDL;
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
        LL_WARNS("DetachedFloaters")
            << "Unable to create shared OpenGL context." << LL_ENDL;
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
        ReleaseDC(static_cast<HWND>(mNativeWindow),
                  static_cast<HDC>(mNativeDC));
        mNativeDC = nullptr;
    }
}

void BSDetachedFloaterHost::pumpMessages()
{
    if (!mNativeWindow)
    {
        return;
    }

    const HWND hwnd = static_cast<HWND>(mNativeWindow);
    MSG msg = {};
    while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void BSDetachedFloaterHost::dispatchMouseMessage(
    unsigned int message, unsigned long long wparam, long long lparam)
{
    if (!mFloater || !mNativeWindow)
    {
        return;
    }

    HWND hwnd = static_cast<HWND>(mNativeWindow);
    RECT client = {};
    if (!GetClientRect(hwnd, &client))
    {
        return;
    }

    POINT point = {};
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
    {
        point.x = GET_X_LPARAM(static_cast<LPARAM>(lparam));
        point.y = GET_Y_LPARAM(static_cast<LPARAM>(lparam));
        ScreenToClient(hwnd, &point);
    }
    else
    {
        point.x = GET_X_LPARAM(static_cast<LPARAM>(lparam));
        point.y = GET_Y_LPARAM(static_cast<LPARAM>(lparam));
    }

    const S32 width = llmax<S32>(1, client.right - client.left);
    const S32 height = llmax<S32>(1, client.bottom - client.top);
    const S32 x = point.x;
    const S32 y = height - 1 - point.y;
    const MASK mask = detachedMask(true);

    const LLRect old_rect = mFloater->getRect();
    mFloater->reshape(width, height, false);
    mFloater->setOrigin(0, 0);

    switch (message)
    {
        case WM_MOUSEMOVE:
            mFloater->handleHover(x, y, mask);
            break;

        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            SetCapture(hwnd);
            mFloater->handleMouseDown(x, y, mask);
            break;

        case WM_LBUTTONUP:
            mFloater->handleMouseUp(x, y, mask);
            if (GetCapture() == hwnd)
            {
                ReleaseCapture();
            }
            break;

        case WM_LBUTTONDBLCLK:
            SetFocus(hwnd);
            mFloater->handleDoubleClick(x, y, mask);
            break;

        case WM_RBUTTONDOWN:
            SetFocus(hwnd);
            mFloater->handleRightMouseDown(x, y, mask);
            break;

        case WM_RBUTTONUP:
            mFloater->handleRightMouseUp(x, y, mask);
            break;

        case WM_MBUTTONDOWN:
            SetFocus(hwnd);
            SetCapture(hwnd);
            mFloater->handleMiddleMouseDown(x, y, mask);
            break;

        case WM_MBUTTONUP:
            mFloater->handleMiddleMouseUp(x, y, mask);
            if (GetCapture() == hwnd)
            {
                ReleaseCapture();
            }
            break;

        case WM_MOUSEWHEEL:
        {
            const S32 clicks =
                -GET_WHEEL_DELTA_WPARAM(static_cast<WPARAM>(wparam)) /
                WHEEL_DELTA;
            if (clicks)
            {
                mFloater->handleScrollWheel(x, y, clicks);
            }
            break;
        }

        case WM_MOUSEHWHEEL:
        {
            const S32 clicks =
                GET_WHEEL_DELTA_WPARAM(static_cast<WPARAM>(wparam)) /
                WHEEL_DELTA;
            if (clicks)
            {
                mFloater->handleScrollHWheel(x, y, clicks);
            }
            break;
        }

        default:
            break;
    }

    mFloater->setRect(old_rect);
}

void BSDetachedFloaterHost::dispatchKeyMessage(
    unsigned int message, unsigned long long wparam, long long lparam)
{
    if (!mFloater)
    {
        return;
    }

    switch (message)
    {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (gKeyboard)
            {
                const MASK native_mask =
                    (static_cast<LPARAM>(lparam) & (1LL << 24))
                        ? MASK_EXTENDED
                        : MASK_NONE;
                gKeyboard->handleKeyDown(
                    static_cast<LLKeyboard::NATIVE_KEY_TYPE>(wparam),
                    native_mask);
            }
            break;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (gKeyboard)
            {
                const MASK native_mask =
                    (static_cast<LPARAM>(lparam) & (1LL << 24))
                        ? MASK_EXTENDED
                        : MASK_NONE;
                gKeyboard->handleKeyUp(
                    static_cast<LLKeyboard::NATIVE_KEY_TYPE>(wparam),
                    native_mask);
            }
            break;

        case WM_CHAR:
            if (gViewerWindow)
            {
                gViewerWindow->handleUnicodeChar(
                    static_cast<llwchar>(wparam), detachedMask(false));
            }
            break;

        default:
            break;
    }
}

void BSDetachedFloaterHost::draw()
{
    // Our HWND belongs to the viewer main thread, so unlike LLWindowWin32 it
    // does not have a dedicated GetMessage() thread. Pump it once per frame.
    // This is what prevents Windows from ghosting it as "Not Responding".
    pumpMessages();

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
    const S32 width = llmax<S32>(1, client.right - client.left);
    const S32 height = llmax<S32>(1, client.bottom - client.top);

    glViewport(0, 0, width, height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadIdentity();
    gGL.ortho(0.f, static_cast<F32>(width),
              0.f, static_cast<F32>(height), -1.f, 1.f);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadIdentity();

    const LLRect old_rect = mFloater->getRect();
    mFloater->reshape(width, height, false);
    mFloater->setOrigin(0, 0);

    gUIProgram.bind();
    gGL.color4f(1.f, 1.f, 1.f, 1.f);

    const bool was_drawing = LLView::sIsDrawing;
    LLView::sIsDrawing = true;
    mFloater->draw();
    LLView::sIsDrawing = was_drawing;

    gGL.flush();
    gUIProgram.unbind();

    mFloater->setRect(old_rect);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);

    SwapBuffers(dc);
    wglMakeCurrent(previous_dc, previous_rc);
}

long long __stdcall BSDetachedFloaterHost::windowProc(
    void* raw_hwnd, unsigned int message,
    unsigned long long wparam, long long lparam)
{
    HWND hwnd = static_cast<HWND>(raw_hwnd);
    BSDetachedFloaterHost* self =
        reinterpret_cast<BSDetachedFloaterHost*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<BSDetachedFloaterHost*>(create->lpCreateParams);
        SetWindowLongPtrW(
            hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    switch (message)
    {
        case WM_CLOSE:
            // Closing the detached OS window reattaches the floater. It must
            // never be forwarded to LLViewerWindow's application-quit path.
            if (self)
            {
                self->attach();
            }
            return 0;

        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (self)
            {
                self->dispatchMouseMessage(message, wparam, lparam);
            }
            return 0;

        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
            if (self)
            {
                self->dispatchKeyMessage(message, wparam, lparam);
            }
            return 0;

        case WM_ERASEBKGND:
            // The GL surface paints the entire client area.
            return 1;

        case WM_DESTROY:
            if (self && self->mNativeWindow == hwnd)
            {
                self->mNativeWindow = nullptr;
            }
            return 0;

        default:
            break;
    }

    return static_cast<long long>(
        DefWindowProcW(hwnd, message,
                       static_cast<WPARAM>(wparam),
                       static_cast<LPARAM>(lparam)));
}
#endif
