/**
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026 Blazing Storm contributors
 * $/LicenseInfo$
 */

/**
 * Blazing Storm local-only Blender pose preview.
 * LGPL-2.1-or-later, compatible with the viewer's LGPL codebase.
 * Runs entirely on the viewer main thread; never sends avatar animations to the simulator.
 */
#ifndef BS_BLENDER_LIVE_LINK_H
#define BS_BLENDER_LIVE_LINK_H

#include <memory>
class LLVOAvatar;

namespace BlazingStorm
{
class BlenderLiveLink final
{
public:
    static BlenderLiveLink& instance();
    // Call immediately after the local avatar's normal updateCharacter() pass.
    void apply(LLVOAvatar* avatar);

private:
    struct Impl;
    BlenderLiveLink();
    ~BlenderLiveLink();
    BlenderLiveLink(const BlenderLiveLink&) = delete;
    BlenderLiveLink& operator=(const BlenderLiveLink&) = delete;
    std::unique_ptr<Impl> mImpl;
};
}
#endif
