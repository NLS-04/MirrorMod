// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

/**
 * Moves the third person camera to 47's other shoulder without the blend the game plays on a key press.
 *
 * The world is mirrored, so the shoulder the camera looks over appears on the other side of the screen.
 * The field offsets are specific to game version 3.290.1.0, which IsCompatible verifies.
 */
namespace ShoulderSwap {
    /** Compares the game's own shoulder swap code with the version the offsets were taken from. */
    bool IsCompatible(const void* p_Module);

    struct State {
        bool m_Side = true;
        float m_Position = 1.f;
    };

    /** Returns false if 47 has no third person camera yet, for example while a level is still starting. */
    bool GetState(const void* p_Hero, State* p_State);

    /** Returns false in the same case. */
    bool Swap(void* p_Hero);

    /**
     * Whether the game put the camera back on its default shoulder since p_ExpectedSide was seen. It does that without
     * a blend, for example when a level's intro ends, while a key press or taking cover starts one.
     */
    bool WasReset(bool p_ExpectedSide, const State& p_State);

    struct Plan {
        bool m_Swap = false;
        bool m_Swapped = false;
        bool m_Wanted = false;
    };

    /**
     * What to do this frame. p_Wanted is whether the camera should be on the other shoulder, p_Swapped whether it was.
     * After a reset the camera is back on the default shoulder, and with the mirror off it stays there.
     */
    Plan PlanSwap(bool p_ModEnabled, bool p_Wanted, bool p_Swapped, bool p_WasReset);

    /** Keeps the camera on the wanted shoulder across level starts and the game's own resets. */
    class Controller {
    public:
        /** The mod was switched on or off with the option to change shoulders set. Any thread. */
        void FollowMirror(bool p_Enabled);

        /** A new scene comes with a new camera. A level always starts on the other shoulder while the mirror is on. */
        void OnSceneLoading();

        /** Call once per gameplay frame. */
        void Update(void* p_Hero, bool p_ModEnabled);

    private:
        std::atomic<bool> m_Wanted = false;
        std::atomic<bool> m_Swapped = false;
        std::atomic<bool> m_LevelStarting = false;
        bool m_ExpectedSide = true;
    };
}
