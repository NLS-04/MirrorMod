// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone check of the shared parts of the mod: key names, the log and the refusal of unknown game versions.

#include "TestSupport.h"

#include "core/GameHooks.h"
#include "core/Hotkey.h"
#include "core/Log.h"
#include "core/VtableHook.h"

#include <windows.h>

#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {
    std::string ReadAll(const std::wstring& p_Path) {
        std::ifstream s_File(p_Path);
        std::stringstream s_Stream;
        s_Stream << s_File.rdbuf();
        return s_Stream.str();
    }

    bool Contains(const std::string& p_Text, const std::string& p_Needle) {
        return p_Text.find(p_Needle) != std::string::npos;
    }

    int g_SinkCalls = 0;
    std::string g_SinkText;

    void Sink(Log::Level, const std::string& p_Text) {
        ++g_SinkCalls;
        g_SinkText = p_Text;
    }

    void TestKeyNames() {
        Check(Hotkey::KeyName(VK_F7) == "F7", "function keys have names");
        Check(Hotkey::KeyName('M') == "M" && Hotkey::KeyName('4') == "4", "letters and digits name themselves");
        Check(Hotkey::KeyName(VK_NUMPAD3) == "Numpad3", "numpad keys have names");
        Check(Hotkey::KeyName(VK_PRIOR) == "PageUp", "named keys have readable names");
        Check(Hotkey::KeyName(VK_LBUTTON).empty(), "mouse buttons cannot be bound");
        Check(Hotkey::KeyName(VK_ESCAPE).empty(), "Escape stays free for cancelling");
        Check(Hotkey::KeyName(VK_SHIFT).empty() && Hotkey::KeyName(VK_LCONTROL).empty(), "modifiers cannot be bound");
        Check(Hotkey::KeyName(VK_OEM_5).empty(), "punctuation keys, which depend on the layout, cannot be bound");

        Check(Hotkey::ParseKeyName("F7") == VK_F7, "a name gives its key");
        Check(Hotkey::ParseKeyName("f7") == VK_F7, "in any case");
        Check(Hotkey::ParseKeyName("pageup") == VK_PRIOR, "also for named keys");
        Check(!Hotkey::ParseKeyName("").has_value(), "an empty name is no key");
        Check(!Hotkey::ParseKeyName("NotAKey").has_value(), "an unknown name is no key");

        bool s_RoundTrip = true;

        for (int s_Key = 1; s_Key < 255; ++s_Key) {
            const std::string s_Name = Hotkey::KeyName(s_Key);
            s_RoundTrip &= s_Name.empty() || Hotkey::ParseKeyName(s_Name) == s_Key;
        }

        Check(s_RoundTrip, "every name leads back to its key");
    }

    void TestPressDetector() {
        Hotkey::PressDetector s_Detector;

        Check(!s_Detector.Update(false, true), "nothing held is no press");
        Check(s_Detector.Update(true, true), "holding a key is a press");
        Check(!s_Detector.Update(true, true), "keeping it held is not another one");
        Check(!s_Detector.Update(false, true), "releasing it is none");
        Check(s_Detector.Update(true, true), "pressing it again is");
        s_Detector.Update(false, true);
        Check(!s_Detector.Update(true, false), "a press that is not accepted does not count");
        Check(!s_Detector.Update(true, true), "and does not fire later while the key stays held");
    }

    void TestCapture() {
        Hotkey::Capture s_Capture;
        Check(!s_Capture.IsActive(), "a capture starts out idle");
        Check(!s_Capture.Update(VK_F8).has_value(), "and ignores keys then");

        s_Capture.Start();
        Check(s_Capture.IsActive(), "it can be started");
        Check(!s_Capture.Update(VK_SPACE).has_value(), "a key still held from before does not count");
        Check(!s_Capture.Update(std::nullopt).has_value(), "releasing it starts the listening");
        Check(s_Capture.Update(VK_F8) == VK_F8, "the next key is chosen");
        Check(!s_Capture.IsActive(), "which ends the capture");

        s_Capture.Start();
        s_Capture.Update(std::nullopt);
        s_Capture.Cancel();
        Check(!s_Capture.IsActive() && !s_Capture.Update(VK_F8).has_value(), "a cancelled capture takes no key");
    }

    void TestLog() {
        Log::Budget s_Budget(2);
        Check(s_Budget.Take() && s_Budget.Take(), "a budget lets its lines through");
        Check(!s_Budget.Take() && !s_Budget.Take(), "and no more than that");

        wchar_t s_Temp[MAX_PATH] {};
        GetTempPathW(MAX_PATH, s_Temp);
        const std::wstring s_Path = std::wstring(s_Temp) + L"MirrorCoreTest.log";

        Log::Info("early", "written before the file was open: {}", 1);
        Log::Warning("early", "an early warning");

        Log::Open(s_Path, &Sink);
        Check(g_SinkCalls == 1 && g_SinkText == "early: an early warning", "early warnings reach the console once it exists");

        Log::Debug("test", "hidden {}", 1);
        Log::SetVerbose(true);
        Log::Debug("test", "shown {}", 2);
        Log::SetVerbose(false);
        Log::Info("test", "plain info");
        Log::Error("render", "something failed: {:#x}", 0x80004005u);
        Check(g_SinkCalls == 2 && g_SinkText == "render: something failed: 0x80004005", "errors reach the console, info does not");

        Log::Close();
        const std::string s_Text = ReadAll(s_Path);

        Check(Contains(s_Text, "written before the file was open: 1"), "lines from before Open are kept");
        Check(!Contains(s_Text, "hidden"), "debug lines are dropped unless verbose");
        Check(Contains(s_Text, "shown 2"), "and written when verbose");
        Check(std::regex_search(s_Text, std::regex(R"(\d\d:\d\d:\d\d\.\d\d\d ERROR render   something failed)")), "a line has time, level, component and message");

        DeleteFileW(s_Path.c_str());
    }

    void TestVersionRefusal() {
        std::vector<uint8_t> s_FakeModule(0x1000);
        int s_Target = 0;
        void* s_Original = nullptr;

        const GameHooks::Site s_Sites[] = {
            { "test site", 0x100, { 0x48, 0x89 }, &s_Target, &s_Original },
        };

        GameHooks::HookSet s_Hooks(s_Sites);
        std::string s_Error;

        Check(!s_Hooks.Prepare(s_FakeModule.data(), &s_Error), "an unknown game version is refused");
        Check(Contains(s_Error, "test site") && Contains(s_Error, "0x100") && Contains(s_Error, "does not match"), "the error names the site");
        Check(!s_Hooks.IsPrepared() && !s_Hooks.Enable(&s_Error), "nothing can be enabled afterwards");

        Check(!GameHooks::CodeMatches(reinterpret_cast<const void*>(0x10), s_FakeModule.data(), 4), "unreadable code does not match, and does not crash");
    }

    void TestVtableHook() {
        void* s_Vtable[3] = { reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), reinterpret_cast<void*>(3) };
        void* s_Original = nullptr;
        const VtableHook::Slot s_Slots[] = { { 1, reinterpret_cast<void*>(9), &s_Original } };

        Check(VtableHook::Install(s_Vtable, s_Slots), "a vtable can be hooked");
        Check(s_Vtable[1] == reinterpret_cast<void*>(9) && s_Original == reinterpret_cast<void*>(2), "the slot is replaced and the original kept");
        Check(s_Vtable[0] == reinterpret_cast<void*>(1) && s_Vtable[2] == reinterpret_cast<void*>(3), "other slots stay");

        VtableHook::Remove(s_Vtable, s_Slots);
        Check(s_Vtable[1] == reinterpret_cast<void*>(2), "removing restores the original");
        Check(s_Original == reinterpret_cast<void*>(2), "which stays valid for detours still running");
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    TestKeyNames();
    TestPressDetector();
    TestCapture();
    TestLog();
    TestVersionRefusal();
    TestVtableHook();

    return Finish();
}
