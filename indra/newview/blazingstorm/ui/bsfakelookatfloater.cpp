/**
 * @file bsfakelookatfloater.cpp
 * @brief UI for Blazing Storm fake look-at generator.
 */

#include "llviewerprecompiledheaders.h"

#include "blazingstorm/ui/bsfakelookatfloater.h"
#include "blazingstorm/lookat/bsfakelookat.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llspinctrl.h"
#include "lltextbox.h"

#include <sstream>

namespace BlazingStorm
{
    FakeLookAtFloater::FakeLookAtFloater(const LLSD& key)
        : LLFloater(key)
    {
    }

    bool FakeLookAtFloater::postBuild()
    {
        auto config_changed = [this](LLUICtrl*, const LLSD&)
        {
            applyControls(true);
        };

        getChild<LLComboBox>("mode")->setCommitCallback(config_changed);
        getChild<LLComboBox>("look_type")->setCommitCallback(config_changed);

        for (const char* name : {"count", "min_distance", "max_distance", "refresh_seconds",
                                 "vertical_scale", "object_jitter"})
        {
            getChild<LLSpinCtrl>(name)->setCommitCallback(config_changed);
        }

        for (const char* name : {"randomize_count", "current_region_only",
                                 "root_objects_only", "exclude_attachments"})
        {
            getChild<LLCheckBoxCtrl>(name)->setCommitCallback(config_changed);
        }

        getChild<LLButton>("start")->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onStart(); });
        getChild<LLButton>("stop")->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onStop(); });
        getChild<LLButton>("refresh_now")->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onRefreshNow(); });

        applyControls(false);
        refreshStatus();
        return true;
    }

    void FakeLookAtFloater::draw()
    {
        refreshStatus();
        LLFloater::draw();
    }

    void FakeLookAtFloater::onClose(bool app_quitting)
    {
        if (!app_quitting && getChild<LLCheckBoxCtrl>("stop_on_close")->getValue().asBoolean())
        {
            FakeLookAtManager::instance().stop();
        }
        LLFloater::onClose(app_quitting);
    }

    void FakeLookAtFloater::applyControls(bool refresh_running)
    {
        FakeLookAtConfig config;

        const std::string mode = getChild<LLComboBox>("mode")->getValue().asString();
        if (mode == "all_avatars")
            config.mode = FakeLookAtMode::AllAvatars;
        else if (mode == "random_avatars")
            config.mode = FakeLookAtMode::RandomAvatars;
        else if (mode == "random_objects")
            config.mode = FakeLookAtMode::RandomObjects;
        else if (mode == "mixed")
            config.mode = FakeLookAtMode::Mixed;
        else
            config.mode = FakeLookAtMode::RandomPositions;

        config.count = getChild<LLSpinCtrl>("count")->getValue().asInteger();
        config.minDistance = static_cast<F32>(getChild<LLSpinCtrl>("min_distance")->getValue().asReal());
        config.maxDistance = static_cast<F32>(getChild<LLSpinCtrl>("max_distance")->getValue().asReal());
        config.refreshSeconds = static_cast<F32>(getChild<LLSpinCtrl>("refresh_seconds")->getValue().asReal());
        config.verticalScale = static_cast<F32>(getChild<LLSpinCtrl>("vertical_scale")->getValue().asReal());
        config.objectJitter = static_cast<F32>(getChild<LLSpinCtrl>("object_jitter")->getValue().asReal());
        config.lookType = static_cast<ELookAtType>(
            getChild<LLComboBox>("look_type")->getValue().asInteger());

        config.randomizeCount = getChild<LLCheckBoxCtrl>("randomize_count")->getValue().asBoolean();
        config.currentRegionOnly = getChild<LLCheckBoxCtrl>("current_region_only")->getValue().asBoolean();
        config.rootObjectsOnly = getChild<LLCheckBoxCtrl>("root_objects_only")->getValue().asBoolean();
        config.excludeAttachments = getChild<LLCheckBoxCtrl>("exclude_attachments")->getValue().asBoolean();

        auto& manager = FakeLookAtManager::instance();
        const bool was_running = manager.isRunning();
        manager.setConfig(config);

        if (refresh_running && was_running)
        {
            manager.refreshNow();
        }
    }

    void FakeLookAtFloater::onStart()
    {
        applyControls(false);
        FakeLookAtManager::instance().start();
        refreshStatus();
    }

    void FakeLookAtFloater::onStop()
    {
        FakeLookAtManager::instance().stop();
        refreshStatus();
    }

    void FakeLookAtFloater::onRefreshNow()
    {
        applyControls(false);
        FakeLookAtManager::instance().refreshNow();
        refreshStatus();
    }

    void FakeLookAtFloater::refreshStatus()
    {
        auto& manager = FakeLookAtManager::instance();

        std::ostringstream status;
        status << (manager.isRunning() ? "RUNNING" : "Stopped")
               << " | active effects: " << manager.activeEffectCount()
               << " / " << FakeLookAtManager::MAX_EFFECTS
               << "\nCandidates in range: "
               << manager.lastAvatarCandidateCount() << " avatars, "
               << manager.lastObjectCandidateCount() << " world objects";

        getChild<LLTextBox>("status")->setText(status.str());
        getChild<LLButton>("stop")->setEnabled(manager.isRunning());
        getChild<LLButton>("refresh_now")->setEnabled(manager.isRunning());
        getChild<LLButton>("start")->setEnabled(!manager.isRunning());
    }
}
