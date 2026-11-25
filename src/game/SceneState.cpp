// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "game/SceneState.h"

#include "core/Log.h"
#include "game/SceneEvents.h"

#include <windows.h>

namespace {
    constexpr const char* c_Log = "scene";

    // Longest gap between gameplay frames before input counts as belonging to a menu.
    constexpr uint64_t c_PlayFrameTimeoutMs = 250;
}

void SceneState::OnSceneLoading(ZEntitySceneContext* p_Context) {
    m_Context = p_Context;
    m_GameVisible = false;
    m_PlayingSince = 0;
    SceneEvents::Reset();
}

void SceneState::OnPlayFrame(bool p_PlayerHasControl) {
    m_LastPlayFrame = GetTickCount64();
    m_PlayerHasControl = p_PlayerHasControl;
}

bool SceneState::IsPlaying() {
    const ZEntitySceneContext* s_Context = m_Context;

    // Without a context the mod was loaded into a running game, where a player in control means a level is up.
    if (!s_Context) {
        return m_PlayerHasControl;
    }

    const ESceneLoadingStage s_Stage = s_Context->m_LoadingStage;

    if (static_cast<int>(s_Stage) != m_LastStage.load(std::memory_order_relaxed)) {
        m_LastStage = static_cast<int>(s_Stage);
        Log::Debug(c_Log, "Loading stage {}.", static_cast<int>(s_Stage));
    }

    return s_Stage == ESceneLoadingStage::eLoading_ScenePlaying;
}

bool SceneState::IsGameVisible() {
    if (!IsPlaying()) {
        return false;
    }

    if (m_GameVisible) {
        return true;
    }

    const uint64_t s_Now = GetTickCount64();
    uint64_t s_Since = m_PlayingSince;

    if (s_Since == 0) {
        s_Since = s_Now;
        m_PlayingSince = s_Now;
    }

    const bool s_Gone = SceneEvents::LoadScreenGone();

    if (!SceneEvents::IsGameVisible(s_Gone, s_Now - s_Since)) {
        return false;
    }

    m_GameVisible = true;

    if (s_Gone) {
        Log::Info(c_Log, "Mirroring starts {} ms after the scene began playing.", s_Now - s_Since);
    }
    else {
        Log::Warning(c_Log, "The load screen event did not arrive within {} ms, mirroring starts on the timeout.", s_Now - s_Since);
    }

    return true;
}

bool SceneState::IsPlayerInControl() const {
    return m_PlayerHasControl && IsGameplayRunning();
}

bool SceneState::IsGameplayRunning() const {
    return GetTickCount64() - m_LastPlayFrame.load() < c_PlayFrameTimeoutMs;
}
