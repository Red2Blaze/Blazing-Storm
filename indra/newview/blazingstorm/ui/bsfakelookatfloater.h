/**
 * @file bsfakelookatfloater.h
 * @brief UI for Blazing Storm fake look-at generator.
 */

#ifndef BS_FAKE_LOOK_AT_FLOATER_H
#define BS_FAKE_LOOK_AT_FLOATER_H

#include "llfloater.h"

namespace BlazingStorm
{
    class FakeLookAtFloater final : public LLFloater
    {
    public:
        explicit FakeLookAtFloater(const LLSD& key);

        bool postBuild() override;
        void draw() override;
        void onClose(bool app_quitting) override;

    private:
        void applyControls(bool refresh_running);
        void refreshStatus();
        void onStart();
        void onStop();
        void onRefreshNow();
    };
}

#endif // BS_FAKE_LOOK_AT_FLOATER_H
