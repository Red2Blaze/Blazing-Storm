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
#include "llglstates.h"
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
    mFloater = floater;
    mOriginalParent = floater->getParent();
    mOriginalRect = floater->getRect();

    if (!createNativeWindow(title))
    {
        mFloater = nullptr;
        mOriginalParent = nullptr;
        return false;
    }

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
        floater->setShape(original_rect);
        parent->addChild(floater);
        parent->sendChildToFront(floater);
    }
}

void BSDetachedFloaterHost::setAlwaysOnTop(bool enabled)
{
    mAlwaysOnTop = enabled;

#ifdef LL_WINDOWS
    if (!mNativeWindow)
    {
        return;
    }

    HWND hwnd = static_cast<HWND>(mNativeWindow);
    SetWindowPos(
        hwnd,
        enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
#endif
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

    // The OS window deliberately has no Windows caption. Firestorm's own
    // floater header is the visible chrome, making this look like the floater
    // was pulled straight out of the viewer.
    const DWORD ex_style = WS_EX_TOOLWINDOW;
    const DWORD style =
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

    const LLVector2 ui_scale = LLUI::getScaleFactor();
    const S32 client_width = llmax<S32>(
        1, ll_round((F32)mOriginalRect.getWidth() * ui_scale.mV[VX]));
    const S32 client_height = llmax<S32>(
        1, ll_round((F32)mOriginalRect.getHeight() * ui_scale.mV[VY]));

    HWND hwnd = CreateWindowExW(
        ex_style,
        BS_DETACHED_WINDOW_CLASS,
        wide_title.c_str(),
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, client_width, client_height,
        nullptr, nullptr, instance, this);

    if (!hwnd)
    {
        LL_WARNS("DetachedFloaters") << "CreateWindowExW failed: "
                                      << GetLastError() << LL_ENDL;
        return false;
    }

    mNativeWindow = hwnd;
    mAlwaysOnTop = false;

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

    // The detached floater is laid out at the native client size using the
    // viewer's normal UI scale, so mouse coordinates map directly back into
    // the current responsive Firestorm layout.
    const LLVector2 ui_scale = LLUI::getScaleFactor();
    const S32 logical_width = llmax<S32>(
        1, ll_round((F32)width / ui_scale.mV[VX]));
    const S32 logical_height = llmax<S32>(
        1, ll_round((F32)height / ui_scale.mV[VY]));

    const S32 ui_x = llclamp(
        ll_round((F32)x / ui_scale.mV[VX]),
        0, logical_width - 1);
    const S32 ui_y = llclamp(
        ll_round((F32)y / ui_scale.mV[VY]),
        0, logical_height - 1);

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

void BSDetachedFloaterHost::presentPixels(
    S32 source_width, S32 source_height,
    S32 dest_width, S32 dest_height)
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
    bitmap.bmiHeader.biWidth = source_width;
    // Positive height means a bottom-up DIB, matching glReadPixels().
    bitmap.bmiHeader.biHeight = source_height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;

    if (source_width == dest_width && source_height == dest_height)
    {
        SetDIBitsToDevice(
            dc,
            0, 0,
            source_width, source_height,
            0, 0,
            0, source_height,
            mPixelBuffer.data(),
            &bitmap,
            DIB_RGB_COLORS);
    }
    else
    {
        // Fallback only. Normal detached rendering now produces a frame at
        // the exact native client size, so this path should be uncommon.
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(
            dc,
            0, 0, dest_width, dest_height,
            0, 0, source_width, source_height,
            mPixelBuffer.data(),
            &bitmap,
            DIB_RGB_COLORS,
            SRCCOPY);
    }

    ReleaseDC(hwnd, dc);
}

void BSDetachedFloaterHost::draw()
{
    // This HWND lives on the viewer's main thread, so pump it every frame.
    pumpMessages();

    if (!mFloater || !mNativeWindow)
    {
        return;
    }

    HWND hwnd = static_cast<HWND>(mNativeWindow);

    // Let Firestorm's own title-bar buttons behave naturally in the detached
    // host: X closes/hides Conversations and the minimize button minimizes the
    // native OS window.
    if (!mFloater->getVisible())
    {
        attach();
        return;
    }

    if (mFloater->isMinimized())
    {
        mFloater->setMinimized(false);
        ShowWindow(hwnd, SW_MINIMIZE);
        return;
    }

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
    {
        return;
    }

    RECT client = {};
    if (!GetClientRect(hwnd, &client))
    {
        return;
    }

    const S32 dest_width = llmax<S32>(1, client.right - client.left);
    const S32 dest_height = llmax<S32>(1, client.bottom - client.top);

    // Resize the detached Firestorm floater itself while it is outside the
    // main viewer hierarchy. This lets tabs/panels reflow normally instead of
    // stretching the old layout. attach() performs the inverse setShape() back
    // to mOriginalRect, restoring the original viewer layout recursively.
    const LLVector2 ui_scale = LLUI::getScaleFactor();
    const S32 logical_width = llmax<S32>(
        1, ll_round((F32)dest_width / ui_scale.mV[VX]));
    const S32 logical_height = llmax<S32>(
        1, ll_round((F32)dest_height / ui_scale.mV[VY]));

    const LLRect detached_rect(0, logical_height, logical_width, 0);
    if (mFloater->getRect() != detached_rect)
    {
        mFloater->setShape(detached_rect);
    }

    const S32 width = dest_width;
    const S32 height = dest_height;

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

    // Draw at the viewer's native UI scale. The floater hierarchy itself has
    // already been responsively laid out for the detached client size.
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
    presentPixels(width, height, dest_width, dest_height);
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
        case WM_NCCALCSIZE:
            // Remove the Windows non-client frame. Firestorm draws the only
            // visible title bar/chrome.
            if (wparam)
            {
                return 0;
            }
            break;

        case WM_NCHITTEST:
            if (self && self->mFloater)
            {
                POINT pt = {
                    GET_X_LPARAM(static_cast<LPARAM>(lparam)),
                    GET_Y_LPARAM(static_cast<LPARAM>(lparam))
                };
                ScreenToClient(hwnd, &pt);

                RECT rc = {};
                GetClientRect(hwnd, &rc);
                const S32 width = llmax<S32>(1, rc.right - rc.left);
                const S32 height = llmax<S32>(1, rc.bottom - rc.top);
                const S32 border = 6;

                const bool left = pt.x < border;
                const bool right = pt.x >= width - border;
                const bool top = pt.y < border;
                const bool bottom = pt.y >= height - border;

                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;

                // Treat the Firestorm header itself as the OS drag region, but
                // reserve the right side for Firestorm's minimize/close buttons.
                const LLVector2 ui_scale = LLUI::getScaleFactor();
                const S32 header_px = llmax<S32>(
                    20,
                    ll_round((F32)self->mFloater->getHeaderHeight() *
                             ui_scale.mV[VY]));
                const S32 button_reserve = llmax<S32>(
                    110,
                    ll_round(116.f * ui_scale.mV[VX]));

                if (pt.y < header_px && pt.x < width - button_reserve)
                {
                    return HTCAPTION;
                }
            }
            return HTCLIENT;

        case WM_GETMINMAXINFO:
            if (self && self->mFloater)
            {
                MINMAXINFO* info =
                    reinterpret_cast<MINMAXINFO*>(lparam);
                const LLVector2 ui_scale = LLUI::getScaleFactor();
                info->ptMinTrackSize.x = llmax<LONG>(
                    180,
                    ll_round((F32)self->mFloater->getMinWidth() *
                             ui_scale.mV[VX]));
                info->ptMinTrackSize.y = llmax<LONG>(
                    120,
                    ll_round((F32)self->mFloater->getMinHeight() *
                             ui_scale.mV[VY]));
                return 0;
            }
            break;

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
            // The off-screen UI frame paints the entire client area.
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
