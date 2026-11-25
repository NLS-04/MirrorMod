// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

class IPluginInterface;

enum class FlipMode : int {
    // Flip inside the engine's command list right before the UI, so the UI stays readable.
    BeforeUi = 0,
    // Flip the finished frame at present. The UI is mirrored too, but this needs no engine internals.
    WholeFrame = 1,
};

/**
 * The player's choices, stored in the mod's ini file in Retail/mods.
 *
 * Values are read from the game, render and audio threads, so they are atomic. Changing them goes through the setters,
 * which write the file. Developer switches are deliberately not stored here, since they should not survive a restart.
 */
class Settings {
public:
    struct Toggle {
        const char* m_Section;
        const char* m_Key;
        bool m_Default;
        std::atomic<bool> m_Value = false;

        bool Get() const { return m_Value.load(std::memory_order_relaxed); }
    };

    explicit Settings(IPluginInterface& p_Plugin) : m_Plugin(p_Plugin) {}

    /** Reads the file, and adds every setting it does not contain yet so that all of them can be edited there. */
    void Load();

    void Set(Toggle& p_Toggle, bool p_Value);

    FlipMode GetFlipMode() const { return m_FlipMode.load(std::memory_order_relaxed); }
    void SetFlipMode(FlipMode p_Mode);

    int GetFullMapAxis() const { return m_FullMapAxis.load(std::memory_order_relaxed); }
    void SetFullMapAxis(int p_Axis);

    int GetToggleKey() const { return m_ToggleKey.load(std::memory_order_relaxed); }
    void SetToggleKey(int p_VirtualKey);

    Toggle m_Enabled { "general", "enabled", false };
    Toggle m_CorrectHud { "hud", "mirror_positions", true };
    Toggle m_MirrorMinimap { "hud", "mirror_minimap", true };
    Toggle m_MirrorFullMap { "hud", "mirror_full_map", true };
    Toggle m_InvertLook { "input", "invert_look", true };
    Toggle m_InvertMove { "input", "invert_move", true };
    Toggle m_InvertFreeCam { "input", "invert_freecam", true };
    Toggle m_SwapAudio { "audio", "swap_channels", true };
    Toggle m_SwapShoulder { "camera", "swap_shoulder", true };

private:
    int LoadChoice(const char* p_Section, const char* p_Key, int p_Default, int p_Count);
    void LoadToggleKey();

    IPluginInterface& m_Plugin;
    std::atomic<FlipMode> m_FlipMode = FlipMode::BeforeUi;
    std::atomic<int> m_FullMapAxis = 0;
    std::atomic<int> m_ToggleKey = 0;
};
