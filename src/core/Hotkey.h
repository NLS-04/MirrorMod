// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

/**
 * The key that switches the mod on and off.
 *
 * It is read from Windows instead of the game's input system, whose bindings cannot be changed once registered and only
 * know their own key names. Keys are stored by name, so the ini file stays readable.
 */
namespace Hotkey {
    constexpr int c_DefaultKey = 0x76; // VK_F7

    /** The name of a key that can be bound, for example "F7" or "Numpad3", or an empty string for any other key. */
    std::string KeyName(int p_VirtualKey);

    /** The key with this name, in any case. */
    std::optional<int> ParseKeyName(std::string_view p_Name);

    bool IsHeld(int p_VirtualKey);

    /** The key that can be bound and is held down right now, if any. */
    std::optional<int> FindHeldKey();

    bool IsGameInForeground();

    /** Turns the held state of a key into single presses. */
    class PressDetector {
    public:
        /** A press counts only if p_Accepted, but the state is always followed so that a held key cannot fire later. */
        bool Update(bool p_Held, bool p_Accepted);

    private:
        bool m_WasHeld = false;
    };

    /** Waits for the player to press the key to bind. */
    class Capture {
    public:
        void Start();
        void Cancel();
        bool IsActive() const;

        /**
         * Call with the currently held key, once per frame. Returns the key once it is chosen.
         * Keys still held when the capture started, such as the one activating the button, do not count.
         */
        std::optional<int> Update(std::optional<int> p_HeldKey);

    private:
        enum class Phase { Idle, WaitingForRelease, Listening };

        Phase m_Phase = Phase::Idle;
    };
}
