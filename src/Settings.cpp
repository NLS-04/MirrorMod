// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Settings.h"

#include "core/Hotkey.h"
#include "core/Log.h"

#include <IPluginInterface.h>

#include <string>

namespace {
    constexpr const char* c_Log = "settings";

    constexpr const char* c_GeneralSection = "general";
    constexpr const char* c_HudSection = "hud";
    constexpr const char* c_FlipModeKey = "mode";
    constexpr const char* c_FullMapAxisKey = "full_map_axis";
    constexpr const char* c_ToggleKeyKey = "toggle_key";

    constexpr int c_FlipModeCount = 2;
    constexpr int c_MapAxisCount = 2;

    /** The SDK only takes its own string type. These names are literals, so a view of them is enough. */
    ZString Name(const char* p_Literal) {
        return std::string_view(p_Literal);
    }
}

void Settings::Load() {
    Toggle* const s_Toggles[] = {
        &m_Enabled, &m_CorrectHud, &m_MirrorMinimap, &m_MirrorFullMap, &m_InvertLook, &m_InvertMove, &m_InvertFreeCam, &m_SwapAudio, &m_SwapShoulder,
    };

    for (Toggle* s_Toggle : s_Toggles) {
        s_Toggle->m_Value = m_Plugin.GetSettingBool(Name(s_Toggle->m_Section), Name(s_Toggle->m_Key), s_Toggle->m_Default);

        if (!m_Plugin.HasSetting(Name(s_Toggle->m_Section), Name(s_Toggle->m_Key))) {
            m_Plugin.SetSettingBool(Name(s_Toggle->m_Section), Name(s_Toggle->m_Key), s_Toggle->m_Default);
        }
    }

    m_FlipMode = static_cast<FlipMode>(LoadChoice(c_GeneralSection, c_FlipModeKey, static_cast<int>(FlipMode::BeforeUi), c_FlipModeCount));
    m_FullMapAxis = LoadChoice(c_HudSection, c_FullMapAxisKey, 0, c_MapAxisCount);
    LoadToggleKey();
}

int Settings::LoadChoice(const char* p_Section, const char* p_Key, int p_Default, int p_Count) {
    if (!m_Plugin.HasSetting(Name(p_Section), Name(p_Key))) {
        m_Plugin.SetSettingInt(Name(p_Section), Name(p_Key), p_Default);
        return p_Default;
    }

    const int64 s_Value = m_Plugin.GetSettingInt(Name(p_Section), Name(p_Key), p_Default);

    if (s_Value < 0 || s_Value >= p_Count) {
        Log::Warning(c_Log, "[{}] {} = {} is out of range, using {}.", p_Section, p_Key, s_Value, p_Default);
        return p_Default;
    }

    return static_cast<int>(s_Value);
}

void Settings::LoadToggleKey() {
    const std::string s_DefaultName = Hotkey::KeyName(Hotkey::c_DefaultKey);

    if (!m_Plugin.HasSetting(Name(c_GeneralSection), Name(c_ToggleKeyKey))) {
        m_Plugin.SetSetting(Name(c_GeneralSection), Name(c_ToggleKeyKey), s_DefaultName);
    }

    const ZString s_Name = m_Plugin.GetSetting(Name(c_GeneralSection), Name(c_ToggleKeyKey), s_DefaultName);
    const std::optional<int> s_Key = Hotkey::ParseKeyName(std::string_view(s_Name.c_str(), s_Name.size()));

    if (!s_Key) {
        Log::Warning(c_Log, "[{}] {} = {} is not a key that can be bound, using {}.", c_GeneralSection, c_ToggleKeyKey, s_Name.c_str(), s_DefaultName);
    }

    m_ToggleKey = s_Key.value_or(Hotkey::c_DefaultKey);
}

void Settings::Set(Toggle& p_Toggle, bool p_Value) {
    p_Toggle.m_Value = p_Value;
    m_Plugin.SetSettingBool(Name(p_Toggle.m_Section), Name(p_Toggle.m_Key), p_Value);
}

void Settings::SetFlipMode(FlipMode p_Mode) {
    m_FlipMode = p_Mode;
    m_Plugin.SetSettingInt(Name(c_GeneralSection), Name(c_FlipModeKey), static_cast<int>(p_Mode));
}

void Settings::SetFullMapAxis(int p_Axis) {
    m_FullMapAxis = p_Axis;
    m_Plugin.SetSettingInt(Name(c_HudSection), Name(c_FullMapAxisKey), p_Axis);
}

void Settings::SetToggleKey(int p_VirtualKey) {
    m_ToggleKey = p_VirtualKey;
    m_Plugin.SetSetting(Name(c_GeneralSection), Name(c_ToggleKeyKey), Hotkey::KeyName(p_VirtualKey));
}
