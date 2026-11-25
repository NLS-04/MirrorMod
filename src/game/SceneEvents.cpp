// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "game/SceneEvents.h"

#include "core/GameHooks.h"
#include "core/GameMemory.h"

#include <atomic>
#include <cstring>

namespace {
    // The function raising the engine's named events, and the name it receives. Game version 3.290.1.0.
    constexpr uintptr_t c_RaiseEventRva = 0xF0F7C0;
    constexpr uintptr_t c_LoadScreenGoneNameRva = 0x1D4D370;
    constexpr size_t c_ZStringChars = 0x8;

    const char c_LoadScreenGoneName[] = "LoadScreenGone";

    // Long enough for the 20 frames the game waits at 30 frames per second.
    constexpr uint64_t c_LoadScreenTimeoutMs = 1000;

    using RaiseEvent_t = void*(__fastcall*)(void*, const void*);

    RaiseEvent_t g_RaiseEvent = nullptr;
    const char* g_LoadScreenGoneChars = nullptr;
    std::atomic<bool> g_LoadScreenGone = false;

    void* __fastcall RaiseEventDetour(void* p_This, const void* p_Name) {
        // The engine passes the literal itself, so the address identifies the event.
        if (GameMemory::Read<const char*>(p_Name, c_ZStringChars) == g_LoadScreenGoneChars) {
            g_LoadScreenGone = true;
        }

        return g_RaiseEvent(p_This, p_Name);
    }

    const GameHooks::Site c_Sites[] = {
        {
            "named event dispatcher", c_RaiseEventRva,
            { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x10, 0x05, 0x9F, 0x90, 0x44 },
            reinterpret_cast<void*>(&RaiseEventDetour), reinterpret_cast<void**>(&g_RaiseEvent)
        },
    };

    GameHooks::HookSet g_Hooks(c_Sites);
}

bool SceneEvents::Prepare(void* p_Module, std::string* p_Error) {
    if (g_Hooks.IsPrepared()) {
        return true;
    }

    const auto* s_Name = static_cast<const char*>(p_Module) + c_LoadScreenGoneNameRva;

    if (!GameHooks::CodeMatches(s_Name, reinterpret_cast<const uint8_t*>(c_LoadScreenGoneName), sizeof(c_LoadScreenGoneName))) {
        if (p_Error) {
            *p_Error = "The name of the load screen event does not match this game version";
        }

        return false;
    }

    g_LoadScreenGoneChars = s_Name;
    return g_Hooks.Prepare(p_Module, p_Error);
}

bool SceneEvents::Install(std::string* p_Error) {
    return g_Hooks.Enable(p_Error);
}

void SceneEvents::Uninstall() {
    g_Hooks.Remove();
}

void SceneEvents::Reset() {
    g_LoadScreenGone = false;
}

bool SceneEvents::LoadScreenGone() {
    return g_LoadScreenGone;
}

bool SceneEvents::IsGameVisible(bool p_LoadScreenGone, uint64_t p_PlayingMs) {
    return p_LoadScreenGone || p_PlayingMs >= c_LoadScreenTimeoutMs;
}
