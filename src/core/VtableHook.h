// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

/** Detours COM methods by replacing their vtable entries, which affects every object sharing that vtable. */
namespace VtableHook {
    struct Slot {
        size_t m_Index;
        void* m_Detour;
        void** m_Original;
    };

    /** Replaces all entries or none of them. */
    bool Install(void** p_Vtable, std::span<const Slot> p_Slots);

    /** Puts the originals back. They stay valid, because another thread may still be inside a detour. */
    void Remove(void** p_Vtable, std::span<const Slot> p_Slots);
}
