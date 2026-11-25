// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

/** Access to the game's objects, whose layouts are only known as offsets. */
namespace GameMemory {
    template <typename T>
    T Read(const void* p_Base, size_t p_Offset) {
        T s_Value {};
        memcpy(&s_Value, static_cast<const uint8_t*>(p_Base) + p_Offset, sizeof(T));
        return s_Value;
    }

    template <typename T>
    void Write(void* p_Base, size_t p_Offset, T p_Value) {
        memcpy(static_cast<uint8_t*>(p_Base) + p_Offset, &p_Value, sizeof(T));
    }

    /** Copies from an address that may not be readable, and returns whether it was. */
    bool TryCopy(const void* p_From, void* p_To, size_t p_Size);
}
