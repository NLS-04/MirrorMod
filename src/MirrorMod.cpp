// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MirrorMod.h"

#include "audio/AudioHooks.h"
#include "core/Log.h"
#include "diagnostics/UiObjectDescriber.h"
#include "diagnostics/UiTrace.h"
#include "game/SceneEvents.h"
#include "hud/HudMirror.h"
#include "hud/MapMirror.h"
#include "render/CommandListHooks.h"

#include <Functions.h>
#include <Globals.h>
#include <Logging.h>

#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZHM5InputManager.h>
#include <Glacier/ZHitman5.h>

#include <cstring>
#include <format>
#include <utility>
#include <vector>

namespace {
    constexpr const char* c_Log = "mod";
    constexpr int c_UpdatePriority = 1;

    void ForwardToConsole(Log::Level p_Level, const std::string& p_Text) {
        if (p_Level == Log::Level::Error) {
            Logger::Error("[MirrorMod] {}", p_Text);
        }
        else {
            Logger::Warn("[MirrorMod] {}", p_Text);
        }
    }

    void* GameModule() {
        return GetModuleHandleW(nullptr);
    }

    std::wstring ModFolderFile(const wchar_t* p_Name) {
        HMODULE s_Module = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ModFolderFile), &s_Module
        );

        wchar_t s_Path[MAX_PATH] {};
        GetModuleFileNameW(s_Module, s_Path, MAX_PATH);
        wcsrchr(s_Path, L'\\')[1] = L'\0';
        wcscat_s(s_Path, p_Name);
        return s_Path;
    }

    /** The version of the running game. Every signature mismatch comes down to this, so it opens the log. */
    std::string GameVersion() {
        wchar_t s_Path[MAX_PATH] {};
        GetModuleFileNameW(nullptr, s_Path, MAX_PATH);

        DWORD s_Ignored = 0;
        std::vector<uint8_t> s_Data(GetFileVersionInfoSizeW(s_Path, &s_Ignored));
        VS_FIXEDFILEINFO* s_Info = nullptr;
        UINT s_Length = 0;

        if (s_Data.empty() || !GetFileVersionInfoW(s_Path, 0, static_cast<DWORD>(s_Data.size()), s_Data.data()) ||
            !VerQueryValueW(s_Data.data(), L"\\", reinterpret_cast<void**>(&s_Info), &s_Length) || !s_Info) {
            return "unknown";
        }

        return std::format(
            "{}.{}.{}.{}", HIWORD(s_Info->dwFileVersionMS), LOWORD(s_Info->dwFileVersionMS), HIWORD(s_Info->dwFileVersionLS), LOWORD(s_Info->dwFileVersionLS)
        );
    }

    bool IsHorizontalLook(const char* p_Name) {
        return strcmp(p_Name, "eIAKBMLookHorizontal") == 0 || strcmp(p_Name, "eIAStickRightHorizontal_Analog") == 0 ||
            strcmp(p_Name, "eIAStickRightHorizontal_Raw") == 0;
    }

    bool IsHorizontalMove(const char* p_Name) {
        return strcmp(p_Name, "eIAKBMMoveHorizontal") == 0 || strcmp(p_Name, "eIAStickLeftHorizontal_Analog") == 0 ||
            strcmp(p_Name, "eIAStickLeftHorizontal_Raw") == 0;
    }

    // The bindings the SDK's FreeCam mod gives the editor style free camera. MousePosX is a relative mouse movement
    // (rel(ms,x)), which turns and pans the camera; MoveX flies it sideways.
    bool IsHorizontalFreeCam(const char* p_Name) {
        return strcmp(p_Name, "MousePosX") == 0 || strcmp(p_Name, "MoveX") == 0;
    }
}

MirrorMod::MirrorMod() :
    m_Settings(*this),
    m_FrameMirror(m_Settings, m_Scene),
    m_MoveLeftAction("eIAKBMMoveLeft"),
    m_MoveRightAction("eIAKBMMoveRight") {
}

MirrorMod::~MirrorMod() {
    const ZMemberDelegate<MirrorMod, void(const SGameUpdateEvent&)> s_PlayDelegate(this, &MirrorMod::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_PlayDelegate, c_UpdatePriority, EUpdateMode::eUpdatePlayMode);

    const ZMemberDelegate<MirrorMod, void(const SGameUpdateEvent&)> s_AlwaysDelegate(this, &MirrorMod::OnAlwaysUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_AlwaysDelegate, c_UpdatePriority, EUpdateMode::eUpdateAlways);

    // Not through SetEnabled: unloading the mod must not change what the player chose.
    m_Settings.m_Enabled.m_Value = false;

    SceneEvents::Uninstall();
    HudMirror::Uninstall();
    UiTrace::Uninstall();
    AudioHooks::Uninstall();
    m_FrameMirror.Shutdown();
    Log::Close();
}

void MirrorMod::Init() {
    // Runs under the loader lock, so this only creates hooks. It has to be this early: the hooks need memory close to
    // the game's code, and the game's own allocations use up that address space while it runs.
    std::string s_Error;

    if (!HudMirror::Prepare(GameModule(), &s_Error)) {
        Log::Error(c_Log, "HUD hooks not available: {}", s_Error);
    }

    if (!UiTrace::Prepare(GameModule(), &s_Error)) {
        Log::Warning(c_Log, "UI trace not available: {}", s_Error);
    }

    if (!SceneEvents::Prepare(GameModule(), &s_Error)) {
        Log::Warning(c_Log, "Scene events not available: {}", s_Error);
    }
}

void MirrorMod::OnEngineInitialized() {
    Log::Open(ModFolderFile(L"MirrorMod.log"), &ForwardToConsole);
    Log::Info(c_Log, "MirrorMod {} running on game version {}.", MIRRORMOD_VERSION, GameVersion());

    m_Settings.Load();
    m_SettingsPending = true;

    m_ShoulderUsable = ShoulderSwap::IsCompatible(GameModule());

    if (!m_ShoulderUsable) {
        Log::Warning(c_Log, "The shoulder swap does not match this game version and is off.");
    }

    if (std::string s_Error; !SceneEvents::Install(&s_Error)) {
        Log::Warning(c_Log, "Scene events failed: {}. Mirroring after loading relies on the timeout.", s_Error);
    }

    // Registered here rather than in Init, so that it runs after the SDK's own DrawScaleform detour.
    // This detour ends the chain because it has to call the original function itself.
    Hooks::DrawScaleform->AddDetour(this, &MirrorMod::DrawScaleform);
    Hooks::ZInputAction_Analog->AddDetour(this, &MirrorMod::ZInputAction_Analog);
    Hooks::ZInputAction_Digital->AddDetour(this, &MirrorMod::ZInputAction_Digital);
    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &MirrorMod::OnLoadScene);

    const ZMemberDelegate<MirrorMod, void(const SGameUpdateEvent&)> s_PlayDelegate(this, &MirrorMod::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_PlayDelegate, c_UpdatePriority, EUpdateMode::eUpdatePlayMode);

    // Play mode frames stop while the game is paused, and the picture-in-picture has to follow a toggle in the pause menu.
    const ZMemberDelegate<MirrorMod, void(const SGameUpdateEvent&)> s_AlwaysDelegate(this, &MirrorMod::OnAlwaysUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_AlwaysDelegate, c_UpdatePriority, EUpdateMode::eUpdateAlways);
}

void MirrorMod::OnDrawUI(bool p_HasFocus) {
    // Not from OnEngineInitialized: this installs hooks, and here it happens on the same thread as for the checkboxes.
    if (std::exchange(m_SettingsPending, false)) {
        SetEnabled(m_Settings.m_Enabled.Get());
    }

    PollToggleKey(p_HasFocus);

    if (m_ShowWindow && p_HasFocus) {
        DrawWindow();
    }
}

void MirrorMod::PollToggleKey(bool p_HasFocus) {
    const bool s_Held = Hotkey::IsHeld(m_Settings.GetToggleKey());

    // While the overlay has focus, keys belong to it. The game's own input is switched off then as well.
    if (m_ToggleKeyPress.Update(s_Held, !p_HasFocus && s_Held && Hotkey::IsGameInForeground())) {
        SetEnabled(!m_Settings.m_Enabled.Get());
    }
}

void MirrorMod::SetEnabled(bool p_Enabled) {
    m_Settings.Set(m_Settings.m_Enabled, p_Enabled);

    if (m_Settings.m_SwapShoulder.Get()) {
        m_Shoulder.FollowMirror(p_Enabled);
    }

    ApplySettings();
    Log::Info(c_Log, "Mirror mode {}.", p_Enabled ? "on" : "off");
}

void MirrorMod::ApplySettings() {
    const bool s_Enabled = m_Settings.m_Enabled.Get();

    ApplyAudioSettings(s_Enabled);
    ApplyHudSettings(s_Enabled);
}

void MirrorMod::ApplyAudioSettings(bool p_Enabled) {
    const bool s_Swap = p_Enabled && m_Settings.m_SwapAudio.Get();

    // Installed on first use, never from Init: the SDK calls Init inside DllMain, where installing would deadlock.
    // It also keeps the mod from touching anything until it is switched on.
    if (s_Swap && !std::exchange(m_AudioInstallTried, true)) {
        if (AudioHooks::Install(&m_AudioError)) {
            Log::Info("audio", "Hooks ready.");
        }
        else {
            Log::Error("audio", "Installing the hooks failed: {}", m_AudioError);
        }
    }

    AudioHooks::SetEnabled(s_Swap);
}

void MirrorMod::ApplyHudSettings(bool p_Enabled) {
    // With the whole frame flipped, the HUD is already on the same side as the world.
    const bool s_Active = p_Enabled && m_Settings.m_CorrectHud.Get() && m_Settings.GetFlipMode() == FlipMode::BeforeUi;

    if (s_Active && !std::exchange(m_HudInstallTried, true)) {
        if (HudMirror::Install(GameModule(), &m_HudError)) {
            Log::Info("hud", "Hooks ready.");
        }
        else {
            Log::Error("hud", "Installing the hooks failed: {}", m_HudError);
        }
    }

    MapMirror::SetMinimapEnabled(m_Settings.m_MirrorMinimap.Get());
    MapMirror::SetFullMapEnabled(m_Settings.m_MirrorFullMap.Get());
    MapMirror::SetFullMapAxis(m_Settings.GetFullMapAxis());
    HudMirror::SetActive(s_Active && m_HudError.empty());
}

void MirrorMod::SetUiTrace(bool p_Enabled) {
    // Installed on first use, never from Init: it waits on engine threads, and an unknown game version must stay untouched.
    if (p_Enabled && !std::exchange(m_UiTraceInstallTried, true)) {
        UiTrace::SetLogPath(ModFolderFile(L"MirrorMod-uitrace.log"));

        if (!UiTrace::Install(GameModule(), &DescribeUiObject, &m_UiTraceError)) {
            Log::Error("trace", "Installing the UI trace failed: {}", m_UiTraceError);
        }
    }

    const bool s_Active = p_Enabled && m_UiTraceError.empty();

    UiTrace::SetEnabled(s_Active);
    CommandListHooks::SetViewportTracing(s_Active);
    Log::Info("trace", "UI trace {}.", s_Active ? "on" : "off");
}

void MirrorMod::OnFrameUpdate(const SGameUpdateEvent&) {
    const auto s_Player = SDK()->GetLocalPlayer();
    bool s_HasControl = false;

    if (s_Player) {
        const auto* s_InputControl = Functions::ZHM5InputManager_GetInputControlForLocalPlayer->Call(Globals::InputManager);
        s_HasControl = s_InputControl && s_InputControl->m_bActive;
    }

    m_Scene.OnPlayFrame(s_HasControl);

    if (m_ShoulderUsable && s_Player && m_Scene.IsPlaying()) {
        m_Shoulder.Update(s_Player.m_pInterfaceRef, m_Settings.m_Enabled.Get());
    }
}

void MirrorMod::OnAlwaysUpdate(const SGameUpdateEvent&) {
    HudMirror::RefreshPip();
}

bool MirrorMod::ShouldMirrorInput() const {
    return m_Settings.m_Enabled.Get() && m_Scene.IsPlayerInControl();
}

bool MirrorMod::ShouldMirrorFreeCam() const {
    // The FreeCam takes control away from 47, unless it lets him walk along. So this only asks for gameplay to be running,
    // which leaves the pause menu alone. The action names only belong to the free camera.
    return m_Settings.m_Enabled.Get() && m_Settings.m_InvertFreeCam.Get() && m_Scene.IsGameplayRunning();
}

void MirrorMod::NoteAction(const char* p_Name) {
    std::scoped_lock s_Lock(m_ActionsMutex);

    if (m_SeenActions.emplace(p_Name).second) {
        Log::Info("input", "Action used: {}", p_Name);
    }
}

DEFINE_PLUGIN_DETOUR(
    MirrorMod, void, DrawScaleform, ZRenderContext* ctx, ZRenderTargetView** rtv, uint32_t a3,
    ZRenderDepthStencilView** dsv, uint32_t a5, bool bCaptureOnly
) {
    const bool s_Armed = m_FrameMirror.BeforeUiPass(rtv, bCaptureOnly);

    p_Hook->CallOriginal(ctx, rtv, a3, dsv, a5, bCaptureOnly);

    m_FrameMirror.AfterUiPass(s_Armed);

    return HookResult<void>(HookAction::Return());
}

DEFINE_PLUGIN_DETOUR(MirrorMod, bool, OnLoadScene, ZEntitySceneContext* th, SSceneInitParameters& parameters) {
    m_Scene.OnSceneLoading(th);
    m_Shoulder.OnSceneLoading();
    HudMirror::ResetScene();

    return HookResult<bool>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(MirrorMod, float, ZInputAction_Analog, ZInputAction* th, int32_t controllerId) {
    if (!th || !th->m_szName) {
        return HookResult<float>(HookAction::Continue());
    }

    const bool s_Log = m_LogActions;
    const bool s_MirrorPlayer = ShouldMirrorInput() &&
        ((m_Settings.m_InvertLook.Get() && IsHorizontalLook(th->m_szName)) || (m_Settings.m_InvertMove.Get() && IsHorizontalMove(th->m_szName)));
    const bool s_MirrorFreeCam = IsHorizontalFreeCam(th->m_szName) && ShouldMirrorFreeCam();
    const bool s_Mirror = s_MirrorPlayer || s_MirrorFreeCam;

    if (!s_Log && !s_Mirror) {
        return HookResult<float>(HookAction::Continue());
    }

    const float s_Value = p_Hook->CallOriginal(th, controllerId);

    if (s_Log && s_Value != 0.f) {
        NoteAction(th->m_szName);
    }

    return HookResult<float>(HookAction::Return(), s_Mirror ? -s_Value : s_Value);
}

DEFINE_PLUGIN_DETOUR(MirrorMod, bool, ZInputAction_Digital, ZInputAction* th, int32_t controllerId) {
    if (!th || !th->m_szName) {
        return HookResult<bool>(HookAction::Continue());
    }

    ZInputAction* s_Swapped = nullptr;

    if (m_Settings.m_InvertMove.Get() && ShouldMirrorInput()) {
        if (strcmp(th->m_szName, "eIAKBMMoveLeft") == 0) {
            s_Swapped = &m_MoveRightAction;
        }
        else if (strcmp(th->m_szName, "eIAKBMMoveRight") == 0) {
            s_Swapped = &m_MoveLeftAction;
        }
    }

    const bool s_Log = m_LogActions;

    if (!s_Log && !s_Swapped) {
        return HookResult<bool>(HookAction::Continue());
    }

    const bool s_Value = p_Hook->CallOriginal(s_Swapped ? s_Swapped : th, controllerId);

    if (s_Log && s_Value) {
        NoteAction(th->m_szName);
    }

    return HookResult<bool>(HookAction::Return(), s_Value);
}

DEFINE_ZHM_PLUGIN(MirrorMod);
