/**
 * @file bscatday.h
 * @brief Client-side International Cat Day avatar replacement helpers.
 *
 * Blazing Storm feature: on August 8, avatars may be rendered with a bundled
 * cat mesh while keeping the original LLVOAvatar skeleton, animation state,
 * identity, movement, and network state untouched.
 */
#ifndef BS_CAT_DAY_H
#define BS_CAT_DAY_H

#include "lluuid.h"
#include "llviewercontrol.h"

#include <ctime>
#include <cstdint>
#include <string>

namespace BSCatDay
{
    // International Cat Day is August 8 in the viewer user's local time.
    inline bool isInternationalCatDay()
    {
        const std::time_t now = std::time(nullptr);
        const std::tm* local = std::localtime(&now);
        return local && local->tm_mon == 7 && local->tm_mday == 8;
    }

    // Master switch + manual test override.
    inline bool isActive()
    {
        if (!gSavedSettings.getBOOL("BSCatDayEnabled"))
        {
            return false;
        }

        return gSavedSettings.getBOOL("BSCatDayForceEnabled") ||
               isInternationalCatDay();
    }

    // Stable FNV-1a hash. We deliberately hash the avatar UUID string rather
    // than animation/appearance data so an avatar always receives the same
    // cat variant during the event.
    inline std::uint32_t avatarHash(const LLUUID& avatar_id)
    {
        const std::string id = avatar_id.asString();

        std::uint32_t hash = 2166136261u;
        for (unsigned char ch : id)
        {
            hash ^= static_cast<std::uint32_t>(ch);
            hash *= 16777619u;
        }
        return hash;
    }

    inline U32 variantForAvatar(const LLUUID& avatar_id)
    {
        U32 variant_count = gSavedSettings.getU32("BSCatDayVariantCount");
        if (variant_count == 0)
        {
            variant_count = 1;
        }

        return static_cast<U32>(avatarHash(avatar_id) % variant_count);
    }

    // Separate deterministic bits are available for future coat/material
    // parameters without changing the primary variant assignment.
    inline U32 materialSeedForAvatar(const LLUUID& avatar_id)
    {
        std::uint32_t value = avatarHash(avatar_id);
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        value ^= value >> 16;
        return static_cast<U32>(value);
    }
}

#endif // BS_CAT_DAY_H
