/**
 * @file bsdetachedfloaterhost.h
 * @brief Native detached-window host for Blazing Storm floaters.
 */
#ifndef BS_DETACHED_FLOATER_HOST_H
#define BS_DETACHED_FLOATER_HOST_H

#include "llsingleton.h"
#include "llrect.h"
#include <string>
#include <memory>
#include <vector>

class LLFloater;
class LLView;
class LLRenderTarget;

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

    // Draw the detached floater after the main viewer frame has completed.
    void draw();

private:
    LLFloater* mFloater = nullptr;
    LLView* mOriginalParent = nullptr;
    LLRect mOriginalRect;

#ifdef LL_WINDOWS
    void* mNativeWindow = nullptr;
    std::unique_ptr<LLRenderTarget> mRenderTarget;
    std::vector<unsigned char> mPixelBuffer;
    S32 mRenderWidth = 0;
    S32 mRenderHeight = 0;
    void pumpMessages();
    void presentPixels(S32 width, S32 height);
    void dispatchMouseMessage(unsigned int message, unsigned long long wparam, long long lparam);
    void dispatchKeyMessage(unsigned int message, unsigned long long wparam, long long lparam);
    static long long __stdcall windowProc(void* hwnd, unsigned int message,
                                           unsigned long long wparam, long long lparam);
    bool createNativeWindow(const std::string& title);
    void destroyNativeWindow();
#endif
};

#endif
