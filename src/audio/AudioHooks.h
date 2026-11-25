// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <windows.h>
#include <mmreg.h>

#include <cstdint>
#include <string>

/**
 * Swaps the left and right channels of everything the process plays.
 *
 * The hook sits on IAudioRenderClient, the last step before the audio engine. Wwise's WASAPI, XAudio2 and
 * DirectSound outputs all end up there, so it does not matter which one the game picks.
 */
namespace AudioHooks {
    struct ChannelLayout {
        uint16_t m_BlockAlign = 0;
        uint16_t m_BytesPerSample = 0;
        uint16_t m_Channels = 0;
        uint16_t m_NumPairs = 0;
        uint16_t m_Pairs[4][2] {};
    };

    struct Stats {
        uint64_t m_FramesSwapped = 0;
        uint64_t m_Buffers = 0;
        uint32_t m_Streams = 0;
        uint16_t m_Channels = 0;
        uint16_t m_NumPairs = 0;
    };

    /** Works out which channels mirror each other. Formats it cannot handle get no pairs, so they stay untouched. */
    ChannelLayout LayoutFromFormat(const WAVEFORMATEX* p_Format);

    void SwapChannels(uint8_t* p_Data, uint32_t p_Frames, const ChannelLayout& p_Layout);

    /**
     * Patches the WASAPI vtables. Needs a working default playback device. Safe to call more than once.
     * Waits for a helper thread, so it must never run under the loader lock (DllMain, or a plugin's Init).
     * Streams opened earlier keep working and are assumed to use the device's mix format.
     */
    bool Install(std::string* p_Error);
    void Uninstall();

    void SetEnabled(bool p_Enabled);
    Stats GetStats();
}
