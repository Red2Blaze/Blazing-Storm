/**
 * @file bsfakelookat.cpp
 * @brief Multi-target fake look-at generator for Blazing Storm.
 */

#include "llviewerprecompiledheaders.h"

#include "blazingstorm/lookat/bsfakelookat.h"

#include "llagent.h"
#include "llhudmanager.h"
#include "llhudobject.h"
#include "llmath.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    constexpr F32 TWO_PI_F = 6.28318530717958647692f;
    constexpr F32 MIN_REFRESH_SECONDS = 0.5f;

    F32 clampDistance(F32 value)
    {
        return llclamp(value, 0.f, 4096.f);
    }
}

namespace BlazingStorm
{
    FakeLookAtManager& FakeLookAtManager::instance()
    {
        static FakeLookAtManager manager;
        return manager;
    }

    FakeLookAtManager::FakeLookAtManager()
        : LLEventTimer(0.1f)
    {
    }

    void FakeLookAtManager::setConfig(const FakeLookAtConfig& incoming)
    {
        FakeLookAtConfig config = incoming;
        config.count = llclamp(config.count, 0, MAX_EFFECTS);
        config.minDistance = clampDistance(config.minDistance);
        config.maxDistance = clampDistance(config.maxDistance);
        if (config.maxDistance < config.minDistance)
        {
            std::swap(config.minDistance, config.maxDistance);
        }
        config.refreshSeconds = llmax(MIN_REFRESH_SECONDS, config.refreshSeconds);
        config.verticalScale = llclamp(config.verticalScale, 0.f, 1.f);
        config.objectJitter = llclamp(config.objectJitter, 0.f, 64.f);

        const bool look_type_changed = config.lookType != mConfig.lookType;
        mConfig = config;

        // Look-at priorities prevent changing an existing high-priority effect
        // into a lower-priority type. Recreate the pool when the type changes.
        if (look_type_changed && !mActiveEffects.empty())
        {
            clearActiveEffects();
        }

        if (mRunning)
        {
            rebuild();
            mRefreshTimer.reset();
        }
    }

    void FakeLookAtManager::start()
    {
        mRunning = true;
        mLastRegion = gAgent.getRegion();
        rebuild();
        mRefreshTimer.reset();
    }

    void FakeLookAtManager::stop()
    {
        mRunning = false;
        clearActiveEffects();
        mLastAvatarCandidates = 0;
        mLastObjectCandidates = 0;
    }

    void FakeLookAtManager::refreshNow()
    {
        if (!mRunning)
        {
            return;
        }

        rebuild();
        mRefreshTimer.reset();
    }

    bool FakeLookAtManager::tick()
    {
        serviceRetiringEffects();

        if (!mRunning)
        {
            return false;
        }

        LLViewerRegion* region = gAgent.getRegion();
        if (region != mLastRegion)
        {
            mLastRegion = region;
            rebuild();
            mRefreshTimer.reset();
            return false;
        }

        if (mRefreshTimer.getElapsedTimeF32() >= mConfig.refreshSeconds)
        {
            rebuild();
            mRefreshTimer.reset();
        }
        else if (!mActiveEffects.empty() && activeEffectCount() == 0)
        {
            // Short-lived look-at types (Hover, Respond, etc.) can expire before
            // the configured refresh interval. Recreate them when needed.
            rebuild();
            mRefreshTimer.reset();
        }

        return false;
    }

    S32 FakeLookAtManager::activeEffectCount() const
    {
        S32 count = 0;
        for (const auto& effect : mActiveEffects)
        {
            if (effect.notNull() && !effect->isDead())
            {
                ++count;
            }
        }
        return count;
    }

    void FakeLookAtManager::serviceRetiringEffects()
    {
        for (auto it = mRetiringEffects.begin(); it != mRetiringEffects.end();)
        {
            if (it->effect.isNull() || it->effect->isDead())
            {
                it = mRetiringEffects.erase(it);
                continue;
            }

            if (--it->ticksRemaining <= 0)
            {
                it->effect->markDead();
                it = mRetiringEffects.erase(it);
                continue;
            }

            ++it;
        }
    }

    void FakeLookAtManager::clearActiveEffects()
    {
        for (auto& effect : mActiveEffects)
        {
            if (effect.isNull() || effect->isDead())
            {
                continue;
            }

            // Give the normal ViewerEffect path a few timer ticks to transmit
            // the clear before retiring the local HUD effect.
            effect->setLookAt(LOOKAT_TARGET_CLEAR, nullptr, LLVector3::zero);
            mRetiringEffects.push_back({effect, 4});
        }
        mActiveEffects.clear();
    }

    void FakeLookAtManager::ensureEffectCount(S32 count)
    {
        if (!isAgentAvatarValid())
        {
            return;
        }

        while (static_cast<S32>(mActiveEffects.size()) < count)
        {
            auto* effect = static_cast<LLHUDEffectLookAt*>(
                LLHUDManager::getInstance()->createViewerEffect(LLHUDObject::LL_HUD_EFFECT_LOOKAT));
            if (!effect)
            {
                break;
            }

            effect->setSourceObject(gAgentAvatarp);
            effect->setBypassPrivacy(mConfig.bypassViewerPrivacy);
            effect->setBypassDistanceLimit(mConfig.bypassViewerDistanceLimit);
            mActiveEffects.emplace_back(effect);
        }
    }

    void FakeLookAtManager::trimEffectCount(S32 count)
    {
        while (static_cast<S32>(mActiveEffects.size()) > count)
        {
            auto effect = mActiveEffects.back();
            mActiveEffects.pop_back();

            if (effect.notNull() && !effect->isDead())
            {
                effect->setLookAt(LOOKAT_TARGET_CLEAR, nullptr, LLVector3::zero);
                mRetiringEffects.push_back({effect, 4});
            }
        }
    }

    std::vector<LLPointer<LLViewerObject>> FakeLookAtManager::collectTargets(bool avatars)
    {
        std::vector<LLPointer<LLViewerObject>> result;
        const LLVector3d agent_pos = gAgent.getPositionGlobal();
        LLViewerRegion* current_region = gAgent.getRegion();

        const S32 object_count = gObjectList.getNumObjects();
        result.reserve(avatars ? 32 : llmin(object_count, 512));

        for (S32 i = 0; i < object_count; ++i)
        {
            LLViewerObject* object = gObjectList.getObject(i);
            if (!object || object->isDead() || object->getID() == gAgentID)
            {
                continue;
            }

            if (object->isAvatar() != avatars)
            {
                continue;
            }

            if (mConfig.currentRegionOnly && current_region && object->getRegion() != current_region)
            {
                continue;
            }

            if (!avatars)
            {
                if (mConfig.excludeAttachments && object->isAttachment())
                {
                    continue;
                }

                if (mConfig.rootObjectsOnly && object->getParent())
                {
                    continue;
                }
            }

            const F64 distance = (object->getPositionGlobal() - agent_pos).magVec();
            if (distance < mConfig.minDistance || distance > mConfig.maxDistance)
            {
                continue;
            }

            result.emplace_back(object);
        }

        return result;
    }

    std::vector<LLPointer<LLViewerObject>> FakeLookAtManager::takeRandom(
        std::vector<LLPointer<LLViewerObject>> candidates, S32 count) const
    {
        std::vector<LLPointer<LLViewerObject>> result;
        count = llclamp(count, 0, llmin(static_cast<S32>(candidates.size()), MAX_EFFECTS));
        result.reserve(count);

        while (count-- > 0 && !candidates.empty())
        {
            const S32 index = ll_rand(static_cast<S32>(candidates.size()));
            result.push_back(candidates[index]);
            candidates[index] = candidates.back();
            candidates.pop_back();
        }

        return result;
    }

    S32 FakeLookAtManager::resolveRequestedCount(S32 available) const
    {
        const S32 capped_available = llclamp(available, 0, MAX_EFFECTS);

        if (mConfig.mode == FakeLookAtMode::AllAvatars && mConfig.count == 0)
        {
            return capped_available;
        }

        S32 requested = llclamp(mConfig.count, 1, MAX_EFFECTS);
        requested = llmin(requested, capped_available);

        if (mConfig.randomizeCount && requested > 1)
        {
            requested = 1 + ll_rand(requested);
        }

        return requested;
    }

    FakeLookAtManager::Target FakeLookAtManager::makeRandomPositionTarget() const
    {
        const F32 min_distance = mConfig.minDistance;
        const F32 max_distance = llmax(min_distance, mConfig.maxDistance);
        const F32 distance = min_distance + ll_frand(max_distance - min_distance);
        const F32 angle = ll_frand(TWO_PI_F);

        const F32 z_limit = distance * mConfig.verticalScale;
        const F32 z = z_limit > 0.f ? ll_frand(z_limit * 2.f) - z_limit : 0.f;
        const F32 horizontal = sqrtf(llmax(0.f, distance * distance - z * z));

        Target target;
        target.position = gAgent.getPositionAgent() +
            LLVector3(cosf(angle) * horizontal, sinf(angle) * horizontal, z);
        return target;
    }

    LLVector3 FakeLookAtManager::makeObjectOffset(bool avatar) const
    {
        if (avatar || mConfig.objectJitter <= 0.f)
        {
            return LLVector3::zero;
        }

        const F32 r = mConfig.objectJitter;
        return LLVector3(
            ll_frand(r * 2.f) - r,
            ll_frand(r * 2.f) - r,
            ll_frand(r * 2.f) - r);
    }

    std::vector<FakeLookAtManager::Target> FakeLookAtManager::buildTargets()
    {
        std::vector<Target> targets;
        auto avatars = collectTargets(true);
        auto objects = collectTargets(false);

        mLastAvatarCandidates = static_cast<S32>(avatars.size());
        mLastObjectCandidates = static_cast<S32>(objects.size());

        if (mConfig.mode == FakeLookAtMode::RandomPositions)
        {
            const S32 count = resolveRequestedCount(MAX_EFFECTS);
            targets.reserve(count);
            for (S32 i = 0; i < count; ++i)
            {
                targets.push_back(makeRandomPositionTarget());
            }
            return targets;
        }

        if (mConfig.mode == FakeLookAtMode::AllAvatars ||
            mConfig.mode == FakeLookAtMode::RandomAvatars)
        {
            const S32 count = resolveRequestedCount(static_cast<S32>(avatars.size()));
            auto selected = takeRandom(std::move(avatars), count);
            targets.reserve(selected.size());
            for (auto& object : selected)
            {
                targets.push_back({object, makeObjectOffset(true)});
            }
            return targets;
        }

        if (mConfig.mode == FakeLookAtMode::RandomObjects)
        {
            const S32 count = resolveRequestedCount(static_cast<S32>(objects.size()));
            auto selected = takeRandom(std::move(objects), count);
            targets.reserve(selected.size());
            for (auto& object : selected)
            {
                targets.push_back({object, makeObjectOffset(false)});
            }
            return targets;
        }

        // Mixed mode chooses from avatars and world objects, then fills any
        // shortage with random positions so the requested count is maintained.
        std::vector<LLPointer<LLViewerObject>> mixed;
        mixed.reserve(avatars.size() + objects.size());
        mixed.insert(mixed.end(), avatars.begin(), avatars.end());
        mixed.insert(mixed.end(), objects.begin(), objects.end());

        const S32 requested = resolveRequestedCount(MAX_EFFECTS);
        auto selected = takeRandom(std::move(mixed), requested);
        targets.reserve(requested);

        for (auto& object : selected)
        {
            const bool avatar = object.notNull() && object->isAvatar();
            targets.push_back({object, makeObjectOffset(avatar)});
        }

        while (static_cast<S32>(targets.size()) < requested)
        {
            targets.push_back(makeRandomPositionTarget());
        }

        return targets;
    }

    void FakeLookAtManager::rebuild()
    {
        if (!mRunning || !isAgentAvatarValid())
        {
            return;
        }

        auto targets = buildTargets();
        const S32 target_count = llmin(static_cast<S32>(targets.size()), MAX_EFFECTS);

        trimEffectCount(target_count);
        ensureEffectCount(target_count);

        for (S32 i = 0; i < target_count && i < static_cast<S32>(mActiveEffects.size()); ++i)
        {
            auto& effect = mActiveEffects[i];
            if (effect.isNull() || effect->isDead())
            {
                if (effect.notNull())
                {
                    effect->markDead();
                }

                auto* replacement = static_cast<LLHUDEffectLookAt*>(
                    LLHUDManager::getInstance()->createViewerEffect(LLHUDObject::LL_HUD_EFFECT_LOOKAT));
                if (!replacement)
                {
                    continue;
                }
                replacement->setSourceObject(gAgentAvatarp);
                effect = replacement;
            }

            effect->setBypassPrivacy(mConfig.bypassViewerPrivacy);
            effect->setBypassDistanceLimit(mConfig.bypassViewerDistanceLimit);
            effect->setLookAt(mConfig.lookType, targets[i].object, targets[i].position);
        }
    }
}
