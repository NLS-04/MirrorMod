// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone check of the UI trace: decoding of Scaleform values, the log, and the refusal on unknown game versions.

#include "TestSupport.h"

#include "diagnostics/UiTrace.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
    // Same layout the game uses for call arguments.
    struct FakeValue {
        uint8_t m_Header[0x18] {};
        uint32_t m_Type = 0;
        uint32_t m_Padding = 0;
        uint64_t m_Data = 0;
        uint64_t m_Aux = 0;
    };

    FakeValue Number(double p_Value) {
        FakeValue s_Value;
        s_Value.m_Type = 5;
        memcpy(&s_Value.m_Data, &p_Value, sizeof(p_Value));
        return s_Value;
    }

    FakeValue Int(int32_t p_Value) {
        FakeValue s_Value;
        s_Value.m_Type = 3;
        s_Value.m_Data = static_cast<uint32_t>(p_Value);
        return s_Value;
    }

    FakeValue Text(const char* p_Value) {
        FakeValue s_Value;
        s_Value.m_Type = 6;
        s_Value.m_Data = reinterpret_cast<uint64_t>(p_Value);
        return s_Value;
    }

    std::string ReadAll(const std::wstring& p_Path) {
        std::ifstream s_File(p_Path);
        std::stringstream s_Stream;
        s_Stream << s_File.rdbuf();
        return s_Stream.str();
    }

    std::string Describe(void*) {
        return "typed sample";
    }

    bool Contains(const std::string& p_Text, const std::string& p_Needle) {
        return p_Text.find(p_Needle) != std::string::npos;
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    Check(sizeof(FakeValue) == 0x30, "test value matches the size used by the game");

    wchar_t s_Temp[MAX_PATH] {};
    GetTempPathW(MAX_PATH, s_Temp);
    const std::wstring s_Path = std::wstring(s_Temp) + L"MirrorUiTraceTest.log";

    UiTrace::SetLogPath(s_Path);

    // A value moving across the screen should stand out; a constant one should not.
    for (int i = 0; i < 5; ++i) {
        const std::vector<FakeValue> s_Args = { Number(100.0 + i * 200.0), Number(540.0), Text("subdue"), Int(i) };
        UiTrace::NoteInvoke("setCustomPositions", reinterpret_cast<void*>(0x1234), s_Args.data(), s_Args.size());
    }

    const std::vector<FakeValue> s_Constant = { Number(1.0) };
    UiTrace::NoteInvoke("setFontSize", nullptr, s_Constant.data(), s_Constant.size());
    UiTrace::NoteInvoke(nullptr, nullptr, nullptr, 0);

    UiTrace::NoteViewport(1520.0f, 40.0f, 360.0f, 200.0f, reinterpret_cast<void*>(0x5678), false);

    UiTrace::Flush();
    std::string s_Log = ReadAll(s_Path);

    Check(Contains(s_Log, "invoke setCustomPositions(nnsi)"), "method name and argument types are recorded");
    Check(Contains(s_Log, "[POS?] [NAME] invoke setCustomPositions"), "a moving value is flagged as a position");
    Check(Contains(s_Log, "count=5"), "calls are counted");
    Check(Contains(s_Log, "arg0=100.00..900.00"), "number ranges are tracked");
    Check(Contains(s_Log, "\"subdue\""), "strings are decoded");
    Check(!Contains(s_Log, "[POS?] invoke setFontSize") && Contains(s_Log, "invoke setFontSize(n)"), "a constant value is not flagged");
    Check(Contains(s_Log, "invoke <self>()"), "calls without a method name are handled");
    Check(Contains(s_Log, "viewport x=1520 y=40 w=360 h=200"), "viewports are recorded");
    Check(s_Log.find("setCustomPositions") < s_Log.find("setFontSize"), "likely positions are listed first");

    // A display object learns its class from the call that created it, and passes it on to its container.
    {
        FakeValue s_Holder;
        s_Holder.m_Type = 10;
        s_Holder.m_Data = 0xAAA0;

        FakeValue s_Created;
        s_Created.m_Type = 10;
        s_Created.m_Data = 0xBBB0;

        FakeValue s_Inner;
        s_Inner.m_Type = 10;
        s_Inner.m_Data = 0xCCC0;

        const std::vector<FakeValue> s_AttachArgs = { s_Holder, Text("hud.ActionPrompt") };
        UiTrace::NoteAttach("attachInstance", nullptr, &s_Created, s_AttachArgs.data(), s_AttachArgs.size());
        UiTrace::NoteAttach("getContainer", &s_Created, &s_Inner, nullptr, 0);

        uint8_t s_Info[0xE8] {};
        const double s_X = -300.0;
        const uint16_t s_Flags = 0x3;
        memcpy(s_Info, &s_X, sizeof(s_X));
        memcpy(s_Info + 0xD4, &s_Flags, sizeof(s_Flags));
        const uint32_t s_Callers[2] = { 0x1234, 0x5678 };

        UiTrace::NoteDisplayInfo(reinterpret_cast<void*>(0xBBB0), s_Info, s_Callers, 2);
        UiTrace::NoteDisplayInfo(reinterpret_cast<void*>(0xCCC0), s_Info, s_Callers, 2);
        UiTrace::NoteDisplayInfo(reinterpret_cast<void*>(0xAAA0), s_Info, s_Callers, 1);

        const uint16_t s_OnlyAlpha = 0x20;
        memcpy(s_Info + 0xD4, &s_OnlyAlpha, sizeof(s_OnlyAlpha));
        UiTrace::NoteDisplayInfo(reinterpret_cast<void*>(0xDDD0), s_Info, s_Callers, 2);

        UiTrace::NoteProjection(0xB0F131, -640.5f, 12.f, 8);

        UiTrace::Flush();
        const std::string s_Display = ReadAll(s_Path);

        Check(Contains(s_Display, "display flags=0x3 class=hud.ActionPrompt path=0x1234<0x5678"), "a position write names its class and callers");
        Check(Contains(s_Display, "class=hud.ActionPrompt(container)"), "a container inherits the class");
        Check(Contains(s_Display, "class=hud.ActionPrompt(holder) path=0x1234"), "the holder of an instance is recognised");
        Check(Contains(s_Display, "arg0=-300.00..-300.00"), "the position range is recorded");
        Check(!Contains(s_Display, "flags=0x20"), "writes that cannot move anything are left out");
        Check(Contains(s_Display, "projection caller=0xb0f131 edges=8"), "projections are listed per caller and edge");
        Check(Contains(s_Display, "arg0=-640.50..-640.50"), "the projected position is recorded");
    }

    const FakeValue* s_Bad = reinterpret_cast<const FakeValue*>(0x10);
    UiTrace::NoteInvoke("badPointer", nullptr, s_Bad, 2);
    UiTrace::Flush();
    Check(Contains(ReadAll(s_Path), "invoke badPointer(!!)"), "unreadable arguments do not crash");

    const size_t s_Entries = UiTrace::GetEntryCount();
    UiTrace::NoteInvoke("setFontSize", nullptr, s_Constant.data(), s_Constant.size());
    Check(UiTrace::GetEntryCount() == s_Entries, "repeated calls share an entry");

    // Zeroes where the game's code would be: an unknown version must leave everything alone.
    std::vector<uint8_t> s_FakeModule(0x1500000);
    std::string s_Error;
    const bool s_Installed = UiTrace::Install(s_FakeModule.data(), Describe, &s_Error);
    printf("[INFO] Install refused with: %s\n", s_Error.c_str());
    Check(!s_Installed && Contains(s_Error, "does not match"), "an unknown game version is refused");

    DeleteFileW(s_Path.c_str());

    return Finish();
}
