/**
 * @file bsfakelookat.h
 * @brief Multi-target fake look-at generator for Blazing Storm.
 */

#ifndef BS_FAKE_LOOK_AT_H
#define BS_FAKE_LOOK_AT_H

#include "lleventtimer.h"
#include "llframetimer.h"
#include "llhudeffectlookat.h"
#include "llpointer.h"
#include "v3math.h"

#include <vector>

class LLViewerObject;
class LLViewerRegion;

namespace BlazingStorm
{
    enum class FakeLookAtMode : S32
    {
        RandomPositions = 0,
        AllAvatars,
        RandomAvatars,
        RandomObjects,
        Mixed
    };

    struct FakeLookAtConfig
    {
        FakeLookAtMode mode = FakeLookAtMode::RandomPositions;
        S32 count = 8;
        F32 minDistance = 2.f;
        F32 maxDistance = 25.f;
        F32 refreshSeconds = 3.f;
        F32 verticalScale = 0.35f;
        F32 objectJitter = 0.25f;
        ELookAtType lookType = LOOKAT_TARGET_FOCUS;
        bool randomizeCount = false;
        bool currentRegionOnly = true;
        bool rootObjectsOnly = true;
        bool excludeAttachments = true;
    };

    class FakeLookAtManager final : public LLEventTimer
    {
    public:
        static FakeLookAtManager& instance();

        void setConfig(const FakeLookAtConfig& config);
        const FakeLookAtConfig& config() const { return mConfig; }

        void start();
        void stop();
        void refreshNow();

        bool isRunning() const { return mRunning; }
        S32 activeEffectCount() const;
        S32 lastAvatarCandidateCount() const { return mLastAvatarCandidates; }
        S32 lastObjectCandidateCount() const { return mLastObjectCandidates; }

        static constexpr S32 MAX_EFFECTS = 64;

    private:
        FakeLookAtManager();
        bool tick() override;

        struct Target
        {
            LLPointer<LLViewerObject> object;
            LLVector3 position;
        };

        struct RetiringEffect
        {
            LLPointer<LLHUDEffectLookAt> effect;
            S32 ticksRemaining = 0;
        };

        void rebuild();
        void clearActiveEffects();
        void serviceRetiringEffects();
        void ensureEffectCount(S32 count);
        void trimEffectCount(S32 count);

        std::vector<LLPointer<LLViewerObject>> collectTargets(bool avatars);
        std::vector<LLPointer<LLViewerObject>> takeRandom(
            std::vector<LLPointer<LLViewerObject>> candidates, S32 count) const;
        std::vector<Target> buildTargets();
        Target makeRandomPositionTarget() const;
        LLVector3 makeObjectOffset(bool avatar) const;
        S32 resolveRequestedCount(S32 available = MAX_EFFECTS) const;

        FakeLookAtConfig mConfig;
        bool mRunning = false;
        LLFrameTimer mRefreshTimer;
        LLViewerRegion* mLastRegion = nullptr;
        S32 mLastAvatarCandidates = 0;
        S32 mLastObjectCandidates = 0;

        std::vector<LLPointer<LLHUDEffectLookAt>> mActiveEffects;
        std::vector<RetiringEffect> mRetiringEffects;
    };
}

#endif // BS_FAKE_LOOK_AT_H
