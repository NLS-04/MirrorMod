// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Glacier/ZScene.h>

#include <atomic>
#include <cstdint>

/** What the game is doing right now, as far as the mirror cares. Used from the game and the render thread. */
class SceneState {
public:
    /** A scene starts loading. */
    void OnSceneLoading(ZEntitySceneContext* p_Context);

    /** Runs once per frame of actual gameplay, which stops in menus and while the game is paused. */
    void OnPlayFrame(bool p_PlayerHasControl);

    bool IsPlaying();

    /** Whether a level is on screen, so not while its loading image is still shown. */
    bool IsGameVisible();

    /** False in menus, so that navigating them is left alone. */
    bool IsPlayerInControl() const;

    /** Whether gameplay frames are running, with or without the player in control. False in menus and while paused. */
    bool IsGameplayRunning() const;

private:
    std::atomic<ZEntitySceneContext*> m_Context = nullptr;

    // True from the start for a mod loaded into a running level.
    std::atomic<bool> m_GameVisible = true;
    std::atomic<uint64_t> m_PlayingSince = 0;

    std::atomic<bool> m_PlayerHasControl = false;
    std::atomic<uint64_t> m_LastPlayFrame = 0;
    std::atomic<int> m_LastStage = -1;
};
