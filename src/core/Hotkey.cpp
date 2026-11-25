// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/Hotkey.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <format>

namespace {
    struct NamedKey {
        const char* m_Name;
        int m_Key;
    };

    // Modifiers and the punctuation keys are left out on purpose. The former are game controls on their own, and the
    // latter differ between keyboard layouts and include the key that opens the SDK's overlay.
    constexpr NamedKey c_NamedKeys[] = {
        { "Space", VK_SPACE },
        { "Enter", VK_RETURN },
        { "Tab", VK_TAB },
        { "Backspace", VK_BACK },
        { "Insert", VK_INSERT },
        { "Delete", VK_DELETE },
        { "Home", VK_HOME },
        { "End", VK_END },
        { "PageUp", VK_PRIOR },
        { "PageDown", VK_NEXT },
        { "Up", VK_UP },
        { "Down", VK_DOWN },
        { "Left", VK_LEFT },
        { "Right", VK_RIGHT },
        { "Pause", VK_PAUSE },
        { "ScrollLock", VK_SCROLL },
        { "NumpadMultiply", VK_MULTIPLY },
        { "NumpadAdd", VK_ADD },
        { "NumpadSubtract", VK_SUBTRACT },
        { "NumpadDecimal", VK_DECIMAL },
        { "NumpadDivide", VK_DIVIDE },
    };

    constexpr int c_FirstVirtualKey = 0x01;
    constexpr int c_LastVirtualKey = 0xFE;

    bool EqualsIgnoringCase(std::string_view p_A, std::string_view p_B) {
        return std::ranges::equal(p_A, p_B, [](unsigned char p_Left, unsigned char p_Right) {
            return std::tolower(p_Left) == std::tolower(p_Right);
        });
    }
}

std::string Hotkey::KeyName(int p_VirtualKey) {
    if ((p_VirtualKey >= 'A' && p_VirtualKey <= 'Z') || (p_VirtualKey >= '0' && p_VirtualKey <= '9')) {
        return std::string(1, static_cast<char>(p_VirtualKey));
    }

    if (p_VirtualKey >= VK_F1 && p_VirtualKey <= VK_F24) {
        return std::format("F{}", p_VirtualKey - VK_F1 + 1);
    }

    if (p_VirtualKey >= VK_NUMPAD0 && p_VirtualKey <= VK_NUMPAD9) {
        return std::format("Numpad{}", p_VirtualKey - VK_NUMPAD0);
    }

    for (const NamedKey& s_Key : c_NamedKeys) {
        if (s_Key.m_Key == p_VirtualKey) {
            return s_Key.m_Name;
        }
    }

    return {};
}

std::optional<int> Hotkey::ParseKeyName(std::string_view p_Name) {
    if (p_Name.empty()) {
        return std::nullopt;
    }

    for (int s_Key = c_FirstVirtualKey; s_Key <= c_LastVirtualKey; ++s_Key) {
        if (EqualsIgnoringCase(KeyName(s_Key), p_Name)) {
            return s_Key;
        }
    }

    return std::nullopt;
}

bool Hotkey::IsHeld(int p_VirtualKey) {
    return (GetAsyncKeyState(p_VirtualKey) & 0x8000) != 0;
}

std::optional<int> Hotkey::FindHeldKey() {
    for (int s_Key = c_FirstVirtualKey; s_Key <= c_LastVirtualKey; ++s_Key) {
        if (IsHeld(s_Key) && !KeyName(s_Key).empty()) {
            return s_Key;
        }
    }

    return std::nullopt;
}

bool Hotkey::IsGameInForeground() {
    DWORD s_Process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_Process);
    return s_Process == GetCurrentProcessId();
}

bool Hotkey::PressDetector::Update(bool p_Held, bool p_Accepted) {
    const bool s_Pressed = p_Held && !m_WasHeld && p_Accepted;
    m_WasHeld = p_Held;
    return s_Pressed;
}

void Hotkey::Capture::Start() {
    m_Phase = Phase::WaitingForRelease;
}

void Hotkey::Capture::Cancel() {
    m_Phase = Phase::Idle;
}

bool Hotkey::Capture::IsActive() const {
    return m_Phase != Phase::Idle;
}

std::optional<int> Hotkey::Capture::Update(std::optional<int> p_HeldKey) {
    if (m_Phase == Phase::WaitingForRelease && !p_HeldKey) {
        m_Phase = Phase::Listening;
    }
    else if (m_Phase == Phase::Listening && p_HeldKey) {
        m_Phase = Phase::Idle;
        return p_HeldKey;
    }

    return std::nullopt;
}
