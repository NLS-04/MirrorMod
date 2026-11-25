// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/GameHooks.h"

#include "core/Log.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>
#include <format>
#include <mutex>

namespace {
    // MinHook has one global state, but several parts of the mod install hooks at different times.
    std::mutex g_MinHookMutex;
    int g_MinHookUsers = 0;

    MH_STATUS AcquireMinHook() {
        std::scoped_lock s_Lock(g_MinHookMutex);

        if (g_MinHookUsers == 0) {
            if (const MH_STATUS s_Status = MH_Initialize(); s_Status != MH_OK) {
                return s_Status;
            }
        }

        ++g_MinHookUsers;
        return MH_OK;
    }

    void ReleaseMinHook() {
        std::scoped_lock s_Lock(g_MinHookMutex);

        if (--g_MinHookUsers == 0) {
            MH_Uninitialize();
        }
    }

    bool Fail(std::string* p_Error, std::string p_Message) {
        if (p_Error) {
            *p_Error = std::move(p_Message);
        }

        return false;
    }

    std::string VersionMismatch(const char* p_Label, uintptr_t p_Rva) {
        return std::format("{} at {:#x} does not match this game version", p_Label, p_Rva);
    }
}

bool GameHooks::CodeMatches(const void* p_Code, const uint8_t* p_Signature, size_t p_Size) {
    __try {
        return memcmp(p_Code, p_Signature, p_Size) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GameHooks::ResolveImports(void* p_Module, std::span<const Import> p_Imports, std::string* p_Error) {
    auto* s_Base = static_cast<uint8_t*>(p_Module);

    for (const Import& s_Import : p_Imports) {
        if (!CodeMatches(s_Base + s_Import.m_Rva, s_Import.m_Signature, c_SignatureSize)) {
            return Fail(p_Error, VersionMismatch(s_Import.m_Label, s_Import.m_Rva));
        }
    }

    for (const Import& s_Import : p_Imports) {
        *s_Import.m_Address = s_Base + s_Import.m_Rva;
    }

    return true;
}

bool GameHooks::HookSet::Prepare(void* p_Module, std::string* p_Error) {
    if (IsPrepared()) {
        return true;
    }

    auto* s_Base = static_cast<uint8_t*>(p_Module);

    for (const Site& s_Site : m_Sites) {
        if (!CodeMatches(s_Base + s_Site.m_Rva, s_Site.m_Signature, c_SignatureSize)) {
            return Fail(p_Error, VersionMismatch(s_Site.m_Label, s_Site.m_Rva));
        }
    }

    if (const MH_STATUS s_Status = AcquireMinHook(); s_Status != MH_OK) {
        return Fail(p_Error, std::format("MH_Initialize failed: {}", MH_StatusToString(s_Status)));
    }

    for (const Site& s_Site : m_Sites) {
        if (const MH_STATUS s_Status = MH_CreateHook(s_Base + s_Site.m_Rva, s_Site.m_Detour, s_Site.m_Original); s_Status != MH_OK) {
            RemoveCreatedHooks(s_Base);
            ReleaseMinHook();
            return Fail(p_Error, std::format("Could not create the hook on {}: {}", s_Site.m_Label, MH_StatusToString(s_Status)));
        }
    }

    m_Module = s_Base;
    return true;
}

bool GameHooks::HookSet::Enable(std::string* p_Error) {
    if (m_Enabled) {
        return true;
    }

    if (!IsPrepared()) {
        return Fail(p_Error, "The hooks were not prepared");
    }

    for (const Site& s_Site : m_Sites) {
        MH_QueueEnableHook(m_Module + s_Site.m_Rva);
    }

    if (const MH_STATUS s_Status = MH_ApplyQueued(); s_Status != MH_OK) {
        return Fail(p_Error, std::format("MH_ApplyQueued failed: {}", MH_StatusToString(s_Status)));
    }

    m_Enabled = true;
    return true;
}

void GameHooks::HookSet::Remove() {
    if (!IsPrepared()) {
        return;
    }

    if (m_Enabled) {
        for (const Site& s_Site : m_Sites) {
            MH_QueueDisableHook(m_Module + s_Site.m_Rva);
        }

        if (const MH_STATUS s_Status = MH_ApplyQueued(); s_Status != MH_OK) {
            Log::Warning("hooks", "Disabling hooks failed: {}", MH_StatusToString(s_Status));
        }
    }

    RemoveCreatedHooks(m_Module);
    ReleaseMinHook();
    m_Enabled = false;
    m_Module = nullptr;
}

void GameHooks::HookSet::RemoveCreatedHooks(uint8_t* p_Module) const {
    for (const Site& s_Site : m_Sites) {
        MH_RemoveHook(p_Module + s_Site.m_Rva);
    }
}
