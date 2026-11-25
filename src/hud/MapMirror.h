// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "hud/Scaleform.h"

#include <cstdint>
#include <string>

/**
 * Mirrors the HUD minimap and the map in the pause menu so that they match the mirrored world.
 *
 * Both are Scaleform containers holding a background and one UI object per marker. Giving the container a negative
 * scale mirrors all of that at once, and the marker objects are then turned back one by one so that labels and icons
 * stay readable. Since the engine keeps moving, rotating and zooming the container, its later writes are corrected too.
 */
namespace MapMirror {
    /** Creates the hooks without enabling them. p_SetDisplayInfo has to be the original, unhooked setter. */
    bool Prepare(void* p_Module, Scaleform::SetDisplayInfo_t p_SetDisplayInfo, std::string* p_Error);
    bool Enable(std::string* p_Error);
    void Remove();

    void SetActive(bool p_Active);
    void SetMinimapEnabled(bool p_Enabled);
    void SetFullMapEnabled(bool p_Enabled);

    /** 0 mirrors the map in the pause menu horizontally, 1 vertically. Applies at once, even while it is open. */
    void SetFullMapAxis(int p_Axis);

    void ResetScene();

    uint64_t GetFlipCount();

    /** Whether a rotation written from the game code at p_Caller belongs to a mirrored map and has to be reversed. */
    bool IsRotationMirrored(uintptr_t p_Caller, void* p_Control);

    /** Handles a layout offset written from the game code at p_Caller. Returns true if p_Adjusted has to be used instead. */
    bool AdjustLayoutOffset(uintptr_t p_Caller, void* p_Control, const float* p_Offset, float* p_Adjusted);

    /**
     * Where the hover popup of the mirrored pause map belongs, on the mirrored axis. The engine computes the position as
     * map position plus the marker's distance from it plus the popup's offset, but on the mirrored map that distance
     * points the other way.
     */
    float MirrorHoverPosition(float p_GamePosition, float p_MapPosition, float p_PopupOffset);

    /**
     * Which axis of a map marker has to be negated on a map mirrored on p_MapAxis (0 horizontal, 1 vertical), or -1.
     * Negating the axis the map is mirrored on draws an icon or text the right way round. Characters and cameras that
     * turn with their entity stay mirrored with the map, which is what turns their direction correctly. Staircase arrows
     * are rotated by 90 degrees in their art, so on a vertically mirrored map only the other axis keeps "up" pointing up.
     */
    int TrackerTurnAxis(int32_t p_Layer, bool p_RotatesWithEntity, int p_MapAxis);

    /**
     * Where the map has to be centered for a pan by the player to go the other way, seen from its current center.
     * The pause menu's map is mirrored on screen, so without this it would move against the mouse.
     */
    float MirrorPanTarget(float p_Current, float p_Target);

    /** The ZUIMapTrackerAspect of the map marker using this UI control, or null. p_Manager is the ZUIMapTrackerManager. */
    const void* FindTrackerAspect(const void* p_Manager, const void* p_Control);
}
