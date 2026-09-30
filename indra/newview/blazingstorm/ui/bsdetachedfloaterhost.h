/**
 * @file bsdetachedfloaterhost.h
 * @brief Native detached-window host for Blazing Storm floaters.
 */
#ifndef BS_DETACHED_FLOATER_HOST_H
#define BS_DETACHED_FLOATER_HOST_H

#include "llsingleton.h"
#include <string>

class LLFloater;

class BSDetachedFloaterHost final : public LLSingleton<BSDetachedFloaterHost>
{
    LLSINGLETON(BSDetachedFloaterHost);
    ~BSDetachedFloaterHost();

public:
    // Opens the native host for a floater. The floater remains owned by the viewer.
    bool detach(LLFloater* floater, const std::string& title);
    void attach();
    bool isDetached() const { return mFloater != nullptr; }
    bool isDetached(const LLFloater* floater) const { return mFloater == floater; }

private:
    LLFloater* mFloater = nullptr;

#ifdef LL_WINDOWS
    void* mNativeWindow = nullptr;
    static long long __stdcall windowProc(void* hwnd, unsigned int message,
                                           unsigned long long wparam, long long lparam);
    bool createNativeWindow(const std::string& title);
    void destroyNativeWindow();
#endif
};

#endif
