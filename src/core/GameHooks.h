// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

/**
 * Detours on functions inside the game's executable.
 *
 * Every address is specific to one game version, so each comes with the first bytes of the function. They are compared
 * before anything is hooked, and a single mismatch refuses the whole set: an unknown version stays untouched.
 *
 * Creating and enabling are separate steps. Trampolines have to be close to the game's code, and the game's own
 * allocations use up that address space while it runs, so hooks should be created as early as possible. That is safe
 * under the loader lock. Enabling is not, because it suspends the other threads.
 */
namespace GameHooks {
    constexpr size_t c_SignatureSize = 16;

    struct Site {
        const char* m_Label;
        uintptr_t m_Rva;
        uint8_t m_Signature[c_SignatureSize];
        void* m_Detour;
        void** m_Original;
    };

    /** A function of the game that is called directly. */
    struct Import {
        const char* m_Label;
        uintptr_t m_Rva;
        uint8_t m_Signature[c_SignatureSize];
        void** m_Address;
    };

    bool CodeMatches(const void* p_Code, const uint8_t* p_Signature, size_t p_Size);

    /** Verifies all imports, then fills in their addresses. */
    bool ResolveImports(void* p_Module, std::span<const Import> p_Imports, std::string* p_Error);

    class HookSet {
    public:
        explicit HookSet(std::span<const Site> p_Sites) : m_Sites(p_Sites) {}

        bool Prepare(void* p_Module, std::string* p_Error);
        bool Enable(std::string* p_Error);
        void Remove();

        bool IsPrepared() const { return m_Module != nullptr; }
        bool IsEnabled() const { return m_Enabled; }

    private:
        void RemoveCreatedHooks(uint8_t* p_Module) const;

        std::span<const Site> m_Sites;
        uint8_t* m_Module = nullptr;
        bool m_Enabled = false;
    };
}
