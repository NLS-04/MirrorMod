// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/GameMemory.h"

#include <windows.h>

bool GameMemory::TryCopy(const void* p_From, void* p_To, size_t p_Size) {
    __try {
        memcpy(p_To, p_From, p_Size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
