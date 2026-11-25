// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/VtableHook.h"

#include "core/Log.h"

#include <windows.h>

namespace {
    bool WriteEntry(void** p_Vtable, size_t p_Index, void* p_Value) {
        DWORD s_OldProtect = 0;

        if (!VirtualProtect(&p_Vtable[p_Index], sizeof(void*), PAGE_READWRITE, &s_OldProtect)) {
            Log::Error("hooks", "VirtualProtect failed on vtable {} slot {}: error {}", static_cast<void*>(p_Vtable), p_Index, GetLastError());
            return false;
        }

        p_Vtable[p_Index] = p_Value;
        VirtualProtect(&p_Vtable[p_Index], sizeof(void*), s_OldProtect, &s_OldProtect);
        return true;
    }
}

bool VtableHook::Install(void** p_Vtable, std::span<const Slot> p_Slots) {
    for (size_t i = 0; i < p_Slots.size(); ++i) {
        *p_Slots[i].m_Original = p_Vtable[p_Slots[i].m_Index];

        if (!WriteEntry(p_Vtable, p_Slots[i].m_Index, p_Slots[i].m_Detour)) {
            Remove(p_Vtable, p_Slots.first(i));
            return false;
        }
    }

    return true;
}

void VtableHook::Remove(void** p_Vtable, std::span<const Slot> p_Slots) {
    for (const Slot& s_Slot : p_Slots) {
        WriteEntry(p_Vtable, s_Slot.m_Index, *s_Slot.m_Original);
    }
}
