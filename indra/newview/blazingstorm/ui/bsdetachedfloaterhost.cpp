/**
 * @file bsdetachedfloaterhost.cpp
 * @brief Native detached-window host for Blazing Storm floaters.
 */
#include "llviewerprecompiledheaders.h"

#include "bsdetachedfloaterhost.h"

#include "llfloater.h"
#include "llview.h"
#include "llrender.h"
#include "llglheaders.h"
#include "llrendertarget.h"
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
        floater->setShape(original_rect);
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

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}

void BSDetachedFloaterHost::destroyNativeWindow()
{
    mRenderTarget.reset();
    mPixelBuffer.clear();
    mRenderWidth = 0;
    mRenderHeight = 0;

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

    const LLVector2 ui_scale = LLUI::getScaleFactor();
    const S32 logical_width =
        llmax<S32>(1, ll_round((F32)width / ui_scale.mV[VX]));
    const S32 logical_height =
        llmax<S32>(1, ll_round((F32)height / ui_scale.mV[VY]));

    mFloater->setShape(LLRect(0, logical_height, logical_width, 0));

    const S32 ui_x = ll_round((F32)x / ui_scale.mV[VX]);
    const S32 ui_y = ll_round((F32)y / ui_scale.mV[VY]);

    switch (message)
    {
        case WM_MOUSEMOVE:
            mFloater->handleHover(ui_x, ui_y, mask);
            break;

        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            SetCapture(hwnd);
            mFloater->handleMouseDown(ui_x, ui_y, mask);
            break;

        case WM_LBUTTONUP:
            mFloater->handleMouseUp(ui_x, ui_y, mask);
            if (GetCapture() == hwnd)
            {
                ReleaseCapture();
            }
            break;

        case WM_LBUTTONDBLCLK:
            SetFocus(hwnd);
            mFloater->handleDoubleClick(ui_x, ui_y, mask);
            break;

        case WM_RBUTTONDOWN:
            SetFocus(hwnd);
            mFloater->handleRightMouseDown(ui_x, ui_y, mask);
            break;

        case WM_RBUTTONUP:
            mFloater->handleRightMouseUp(ui_x, ui_y, mask);
            break;

        case WM_MBUTTONDOWN:
            SetFocus(hwnd);
            SetCapture(hwnd);
            mFloater->handleMiddleMouseDown(ui_x, ui_y, mask);
            break;

        case WM_MBUTTONUP:
            mFloater->handleMiddleMouseUp(ui_x, ui_y, mask);
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
                mFloater->handleScrollWheel(ui_x, ui_y, clicks);
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
                mFloater->handleScrollHWheel(ui_x, ui_y, clicks);
            }
            break;
        }

        default:
            break;
    }

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

void BSDetachedFloaterHost::presentPixels(S32 width, S32 height)
{
    if (!mNativeWindow || mPixelBuffer.empty())
    {
        return;
    }

    HWND hwnd = static_cast<HWND>(mNativeWindow);
    HDC dc = GetDC(hwnd);
    if (!dc)
    {
        return;
    }

    BITMAPINFO bitmap = {};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = width;
    // Positive height means a bottom-up DIB, matching glReadPixels().
    bitmap.bmiHeader.biHeight = height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(
        dc,
        0, 0, width, height,
        0, 0, width, height,
        mPixelBuffer.data(),
        &bitmap,
        DIB_RGB_COLORS,
        SRCCOPY);

    ReleaseDC(hwnd, dc);
}

void BSDetachedFloaterHost::draw()
{
    // This HWND lives on the viewer's main thread, so pump it every frame.
    pumpMessages();

    if (!mFloater || !mNativeWindow ||
        !IsWindowVisible(static_cast<HWND>(mNativeWindow)) ||
        IsIconic(static_cast<HWND>(mNativeWindow)))
    {
        return;
    }

    RECT client = {};
    if (!GetClientRect(static_cast<HWND>(mNativeWindow), &client))
    {
        return;
    }

    const S32 width = llmax<S32>(1, client.right - client.left);
    const S32 height = llmax<S32>(1, client.bottom - client.top);
    const LLVector2 ui_scale = LLUI::getScaleFactor();
    const S32 logical_width =
        llmax<S32>(1, ll_round((F32)width / ui_scale.mV[VX]));
    const S32 logical_height =
        llmax<S32>(1, ll_round((F32)height / ui_scale.mV[VY]));

    // Keep the detached floater laid out for the native window. Its original
    // shape is restored by attach().
    mFloater->setShape(LLRect(0, logical_height, logical_width, 0));

    if (!mRenderTarget)
    {
        mRenderTarget = std::make_unique<LLRenderTarget>();
    }

    if (!mRenderTarget->isComplete())
    {
        if (!mRenderTarget->allocate(width, height, GL_RGBA, false))
        {
            LL_WARNS("DetachedFloaters")
                << "Failed to allocate detached UI render target "
                << width << "x" << height << LL_ENDL;
            return;
        }
        mRenderWidth = width;
        mRenderHeight = height;
    }
    else if (mRenderWidth != width || mRenderHeight != height)
    {
        mRenderTarget->resize(width, height);
        mRenderWidth = width;
        mRenderHeight = height;
    }

    mPixelBuffer.resize(
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    // Render using the viewer's already-initialized GL context. This is the
    // key difference from the previous black-window implementation: all
    // Firestorm VAOs, shaders and cached GL state remain valid.
    mRenderTarget->bindTarget();
    glClearColor(0.f, 0.f, 0.f, 1.f);
    mRenderTarget->clear(GL_COLOR_BUFFER_BIT);

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadIdentity();
    gGL.ortho(0.f, (F32)width, 0.f, (F32)height, -1.f, 1.f);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadIdentity();
    gGL.pushUIMatrix();
    gGL.scaleUI(ui_scale.mV[VX], ui_scale.mV[VY], 1.f);

    {
        LLGLSUIDefault gls_ui;
        gUIProgram.bind();
        gGL.color4f(1.f, 1.f, 1.f, 1.f);

        const bool was_drawing = LLView::sIsDrawing;
        LLView::sIsDrawing = true;
        mFloater->draw();
        LLView::sIsDrawing = was_drawing;

        gGL.flush();
        gUIProgram.unbind();
    }

    gGL.popUIMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(
        0, 0, width, height,
        GL_BGRA, GL_UNSIGNED_BYTE,
        mPixelBuffer.data());

    mRenderTarget->flush();

    // Presentation is deliberately GDI for the MVP. It avoids introducing a
    // second OpenGL context while we validate detach/reattach and input.
    presentPixels(width, height);
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
