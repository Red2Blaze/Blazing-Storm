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

    const std::wstring wide_title = utf8str_to_wstring(title);
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
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}

void BSDetachedFloaterHost::destroyNativeWindow()
{
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
