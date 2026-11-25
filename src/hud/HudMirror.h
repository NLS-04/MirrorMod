// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

/**
 * Keeps the HUD consistent with the mirrored world without mirroring the HUD itself.
 *
 * Everything anchored in the world (action prompts, NPC markers, waypoints) gets its position from one engine function,
 * so changing that function's result moves those elements and leaves their text readable. The elements placed around
 * 47 or the screen center are corrected in the UI setters they go through, told apart by who calls. Inventory, timer
 * and objectives go through neither and stay where they are.
 *
 * The picture-in-picture is drawn into the world image before the flip, so it is placed in the opposite corner and the
 * flip brings it back to its original one.
 *
 * The hook sites are specific to game version 3.290.1.0, see GameHooks. The maps are handled by MapMirror.
 */
namespace HudMirror {
    struct Stats {
        uint64_t m_ProjectedPoints = 0;
        uint64_t m_Wedges = 0;
        uint64_t m_PipRects = 0;
        uint64_t m_Prompts = 0;
        uint64_t m_AttentionMoves = 0;
        uint64_t m_HitMarkers = 0;
        uint64_t m_MapFlips = 0;
    };

    /** Creates the hooks without enabling them, which is safe under the loader lock. */
    bool Prepare(void* p_Module, std::string* p_Error);

    /** Enables the hooks, preparing them first if that did not happen yet. Must not run under the loader lock. */
    bool Install(void* p_Module, std::string* p_Error);
    void Uninstall();

    void SetActive(bool p_Active);

    /** Width of the image the picture-in-picture rectangle refers to. Without it the rectangle is left alone. */
    void SetScreenWidth(uint32_t p_Width);

    Stats GetStats();

    /** Forgets the engine objects the mirroring refers to. They are replaced whenever a scene loads. */
    void ResetScene();

    /**
     * Sends the picture-in-picture rectangle again if the mirror changed since the engine set it.
     * The rectangle goes into the engine's render queue, so this has to run on the game thread.
     */
    void RefreshPip();

    /**
     * Mirrors a projected point, which the engine reports relative to the screen center.
     * Off-screen elements also report the edge they were pushed to, so that one is swapped as well.
     */
    void MirrorProjectedPoint(float* p_Xy, uint32_t* p_Edges);

    /** Mirrors a picture-in-picture rectangle in pixels. Empty rectangles mean "hidden" and stay unchanged. */
    void MirrorPipRect(const float* p_In, float* p_Out, uint32_t p_ScreenWidth);

    /**
     * Mirrors a tilted action prompt in Scaleform's display info: its position, and the rotations that change under a
     * mirror. The scale stays positive, so the text remains readable. Applying it twice restores the original.
     */
    void MirrorPromptDisplayInfo(uint8_t* p_Info);
}
