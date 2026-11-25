// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <IPluginInterface.h>

#include <Glacier/SGameUpdateEvent.h>
#include <Glacier/ZInput.h>
#include <Glacier/ZRender.h>
#include <Glacier/ZScene.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>

#include "Settings.h"
#include "core/Hotkey.h"
#include "game/SceneState.h"
#include "game/ShoulderSwap.h"
#include "render/FrameMirror.h"

/** The plugin: connects the game's hooks and update loops to the parts of the mod, and draws its window. */
class MirrorMod : public IPluginInterface {
public:
    MirrorMod();
    ~MirrorMod() override;

    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);
    void OnAlwaysUpdate(const SGameUpdateEvent& p_UpdateEvent);
    void PollToggleKey(bool p_HasFocus);

    void SetEnabled(bool p_Enabled);
    void ApplySettings();
    void ApplyAudioSettings(bool p_Enabled);
    void ApplyHudSettings(bool p_Enabled);
    void SetUiTrace(bool p_Enabled);

    bool ShouldMirrorInput() const;
    bool ShouldMirrorFreeCam() const;
    void NoteAction(const char* p_Name);

    // The window, in MirrorModUi.cpp.
    void DrawWindow();
    void DrawToggleKey();
    void DrawProblems();
    void DrawConfig();
    void DrawDeveloper();
    void DrawAbout();
    bool DrawToggle(const char* p_Label, Settings::Toggle& p_Toggle, const char* p_Tooltip);

    DECLARE_PLUGIN_DETOUR(
        MirrorMod, void, DrawScaleform, ZRenderContext* ctx, ZRenderTargetView** rtv, uint32_t a3,
        ZRenderDepthStencilView** dsv, uint32_t a5, bool bCaptureOnly
    );
    DECLARE_PLUGIN_DETOUR(MirrorMod, bool, OnLoadScene, ZEntitySceneContext* th, SSceneInitParameters& parameters);
    DECLARE_PLUGIN_DETOUR(MirrorMod, float, ZInputAction_Analog, ZInputAction* th, int32_t controllerId);
    DECLARE_PLUGIN_DETOUR(MirrorMod, bool, ZInputAction_Digital, ZInputAction* th, int32_t controllerId);

private:
    Settings m_Settings;
    SceneState m_Scene;
    FrameMirror m_FrameMirror;
    ShoulderSwap::Controller m_Shoulder;
    bool m_ShoulderUsable = false;

    ZInputAction m_MoveLeftAction;
    ZInputAction m_MoveRightAction;

    // The window and the toggle key. Only touched on the render thread.
    bool m_ShowWindow = false;
    bool m_SettingsPending = false;
    Hotkey::PressDetector m_ToggleKeyPress;
    Hotkey::Capture m_ToggleKeyCapture;

    // Parts that install their hooks on first use, and why they could not.
    bool m_AudioInstallTried = false;
    bool m_HudInstallTried = false;
    bool m_UiTraceInstallTried = false;
    std::string m_AudioError;
    std::string m_HudError;
    std::string m_UiTraceError;

    std::atomic<bool> m_LogActions = false;
    std::mutex m_ActionsMutex;
    std::set<std::string> m_SeenActions;
};

DECLARE_ZHM_PLUGIN(MirrorMod)
