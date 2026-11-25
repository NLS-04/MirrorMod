// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

/**
 * Tells when a level has really become visible.
 *
 * The scene counts as playing some frames before the loading image disappears. The game announces that moment itself
 * with its event "LoadScreenGone", so that is what the mirror waits for.
 *
 * The hook site is specific to game version 3.290.1.0. On another version Prepare refuses and the event never arrives,
 * which leaves only the timeout.
 */
namespace SceneEvents {
    /** Creates the hook without enabling it, which is safe under the loader lock. */
    bool Prepare(void* p_Module, std::string* p_Error);

    bool Install(std::string* p_Error);
    void Uninstall();

    /** A new scene starts loading, so its loading image is still to come. */
    void Reset();

    bool LoadScreenGone();

    /** Whether the game can be mirrored, p_PlayingMs after its scene started playing. The timeout covers a missing event. */
    bool IsGameVisible(bool p_LoadScreenGone, uint64_t p_PlayingMs);
}
