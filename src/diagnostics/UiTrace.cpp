// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "diagnostics/UiTrace.h"

#include "core/GameHooks.h"
#include "core/GameMemory.h"
#include "hud/Scaleform.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <unordered_map>
#include <vector>

using namespace Scaleform;
using GameMemory::TryCopy;

namespace {
    // Everything that can put an element on the wrong side. Alpha, scale and visibility cannot.
    constexpr uint16_t c_DisplayPlacement =
        c_DisplayHasX | c_DisplayHasY | c_DisplayHasRotation | c_DisplayHasZ | c_DisplayHasProjection | c_DisplayHasView;

    constexpr int c_MaxArgs = 12;
    constexpr size_t c_MaxEntries = 1500;
    constexpr size_t c_MaxSamples = 4;
    constexpr uint64_t c_SampleIntervalMs = 1000;
    constexpr uint64_t c_FlushIntervalMs = 3000;

    using Invoke_t = uint64_t(__fastcall*)(void*, const char*, void*, void*, uint64_t);
    using Wrapper_t = uint64_t(__fastcall*)(void*, const char*, void*, void*);

    Invoke_t g_Invoke = nullptr;
    Wrapper_t g_ObjectWrapper = nullptr;
    Wrapper_t g_DataWrapper = nullptr;

    UiTrace::DescribeObject_t g_Describe = nullptr;
    std::atomic<bool> g_Enabled = false;

    // The kinds of calls recorded, in the order they appear in the log.
    enum class Section { Invoke, TypedCall, Display, Projection, Viewport, Count };

    const char* const c_SectionTitles[] = {
        "Scaleform calls. Signature: n number, i int, u uint, b bool, s string, O object, A array, D display object, _ undefined or null, ! unreadable.",
        "Calls receiving a typed engine object.",
        "Display property writes. arg0 x, arg1 y, arg2 rotation, arg3 z.",
        "World to HUD projections, before mirroring. arg0 x, arg1 y, relative to the screen center.",
        "Viewports smaller than the screen, on a screen sized target.",
    };

    struct NumberStat {
        bool m_Seen = false;
        double m_Min = 0.0;
        double m_Max = 0.0;
    };

    struct Entry {
        Section m_Section = Section::Invoke;
        bool m_NameIsPositional = false;
        uint64_t m_Count = 0;
        uint64_t m_LastSample = 0;
        NumberStat m_Numbers[c_MaxArgs];
        std::vector<std::string> m_Samples;
    };

    std::mutex g_Mutex;
    std::map<std::string, Entry> g_Entries;
    std::unordered_map<uintptr_t, std::string> g_Classes;
    std::wstring g_LogPath;
    uint64_t g_LastFlush = 0;

    std::string ReadText(const char* p_Text) {
        if (!p_Text) {
            return "<null>";
        }

        char s_Buffer[128] {};

        for (size_t i = 0; i + 1 < sizeof(s_Buffer); ++i) {
            if (!TryCopy(p_Text + i, &s_Buffer[i], 1)) {
                return "<unreadable>";
            }
            if (s_Buffer[i] == 0) {
                break;
            }
        }

        return s_Buffer;
    }

    struct Decoded {
        std::string m_Signature;
        std::string m_Text;
        double m_Numbers[c_MaxArgs] {};
        bool m_IsNumber[c_MaxArgs] {};
    };

    Decoded DecodeArgs(const void* p_Args, uint64_t p_Count) {
        Decoded s_Out;
        const auto* s_Base = static_cast<const uint8_t*>(p_Args);
        const int s_Count = static_cast<int>((std::min<uint64_t>)(p_Count, c_MaxArgs));

        for (int i = 0; i < s_Count; ++i) {
            const uint8_t* s_Value = s_Base + static_cast<size_t>(i) * c_ValueSize;

            uint32_t s_Raw = 0;
            uint64_t s_Data = 0;

            if (!s_Base || !TryCopy(s_Value + c_ValueType, &s_Raw, sizeof(s_Raw)) ||
                !TryCopy(s_Value + c_ValueData, &s_Data, sizeof(s_Data))) {
                s_Out.m_Signature += '!';
                s_Out.m_Text += " <unreadable>";
                continue;
            }

            switch (s_Raw & c_ValueTypeMask) {
                case c_TypeBoolean:
                    s_Out.m_Signature += 'b';
                    s_Out.m_Text += std::format(" {}", (s_Data & 0xFF) != 0);
                    break;
                case c_TypeInt:
                    s_Out.m_Signature += 'i';
                    s_Out.m_IsNumber[i] = true;
                    s_Out.m_Numbers[i] = static_cast<int32_t>(s_Data);
                    s_Out.m_Text += std::format(" {}", static_cast<int32_t>(s_Data));
                    break;
                case c_TypeUInt:
                    s_Out.m_Signature += 'u';
                    s_Out.m_IsNumber[i] = true;
                    s_Out.m_Numbers[i] = static_cast<uint32_t>(s_Data);
                    s_Out.m_Text += std::format(" {}", static_cast<uint32_t>(s_Data));
                    break;
                case c_TypeNumber: {
                    double s_Number = 0.0;
                    memcpy(&s_Number, &s_Data, sizeof(s_Number));
                    s_Out.m_Signature += 'n';
                    s_Out.m_IsNumber[i] = true;
                    s_Out.m_Numbers[i] = s_Number;
                    s_Out.m_Text += std::format(" {:.3f}", s_Number);
                    break;
                }
                case c_TypeString: {
                    const char* s_Text = reinterpret_cast<const char*>(s_Data);

                    if (s_Raw & c_ValueManagedBit) {
                        const char* s_Inner = nullptr;
                        s_Text = TryCopy(s_Text, &s_Inner, sizeof(s_Inner)) ? s_Inner : nullptr;
                    }

                    s_Out.m_Signature += 's';
                    s_Out.m_Text += std::format(" \"{}\"", ReadText(s_Text));
                    break;
                }
                case c_TypeUndefined:
                    s_Out.m_Signature += '_';
                    s_Out.m_Text += " undefined";
                    break;
                case c_TypeNull:
                    s_Out.m_Signature += '_';
                    s_Out.m_Text += " null";
                    break;
                case c_TypeObject:
                    s_Out.m_Signature += 'O';
                    s_Out.m_Text += " <object>";
                    break;
                case c_TypeArray:
                    s_Out.m_Signature += 'A';
                    s_Out.m_Text += " <array>";
                    break;
                case c_TypeDisplayObject:
                    s_Out.m_Signature += 'D';
                    s_Out.m_Text += " <displayobject>";
                    break;
                default:
                    s_Out.m_Signature += '?';
                    s_Out.m_Text += std::format(" <type {}>", s_Raw & c_ValueTypeMask);
                    break;
            }
        }

        return s_Out;
    }

    bool NameLooksPositional(const std::string& p_Key) {
        static const std::regex s_Pattern(
            "pos|screen|anchor|overhead|mark|prompt|indicator|waypoint|triangle|damage|pip|viewport|attention|target|icon|world|custom|arrow|tracker|interact",
            std::regex::icase
        );
        return std::regex_search(p_Key, s_Pattern);
    }

    bool HasScreenRange(const Entry& p_Entry) {
        for (const auto& s_Number : p_Entry.m_Numbers) {
            if (s_Number.m_Seen && s_Number.m_Max - s_Number.m_Min > 1.0 &&
                s_Number.m_Min > -500.0 && s_Number.m_Max < 8000.0) {
                return true;
            }
        }
        return false;
    }

    using EntryItem = std::pair<const std::string, Entry>;

    int Rank(const EntryItem& p_Item) {
        return (HasScreenRange(p_Item.second) ? 0 : 2) + (p_Item.second.m_NameIsPositional ? 0 : 1);
    }

    void WriteEntry(FILE* p_File, const EntryItem& p_Item) {
        const Entry& s_Entry = p_Item.second;
        std::string s_Ranges;

        for (int i = 0; i < c_MaxArgs; ++i) {
            if (s_Entry.m_Numbers[i].m_Seen) {
                s_Ranges += std::format(" arg{}={:.2f}..{:.2f}", i, s_Entry.m_Numbers[i].m_Min, s_Entry.m_Numbers[i].m_Max);
            }
        }

        fprintf(
            p_File, "%s%s%s  count=%llu%s\n", HasScreenRange(s_Entry) ? "[POS?] " : "", s_Entry.m_NameIsPositional ? "[NAME] " : "",
            p_Item.first.c_str(), s_Entry.m_Count, s_Ranges.c_str()
        );

        for (const auto& s_Sample : s_Entry.m_Samples) {
            fprintf(p_File, "    %s\n", s_Sample.c_str());
        }
    }

    void WriteLogLocked() {
        if (g_LogPath.empty()) {
            return;
        }

        FILE* s_File = _wfopen(g_LogPath.c_str(), L"w");

        if (!s_File) {
            return;
        }

        fprintf(s_File, "# MirrorMod UI trace. Likely positions come first in each section.\n");
        fprintf(s_File, "# [POS?] numbers that moved in a screen-like range. [NAME] a name suggesting a position.\n");

        for (int s_Section = 0; s_Section < static_cast<int>(Section::Count); ++s_Section) {
            std::vector<const EntryItem*> s_Items;

            for (const EntryItem& s_Item : g_Entries) {
                if (static_cast<int>(s_Item.second.m_Section) == s_Section) {
                    s_Items.push_back(&s_Item);
                }
            }

            if (s_Items.empty()) {
                continue;
            }

            std::stable_sort(s_Items.begin(), s_Items.end(), [](const EntryItem* p_A, const EntryItem* p_B) { return Rank(*p_A) < Rank(*p_B); });

            fprintf(s_File, "\n## %s\n", c_SectionTitles[s_Section]);

            for (const EntryItem* s_Item : s_Items) {
                WriteEntry(s_File, *s_Item);
            }
        }

        fclose(s_File);
    }

    // Keeps the first samples of an entry, then always the most recent one.
    void AddSample(Entry& p_Entry, std::string p_Sample) {
        if (p_Entry.m_Samples.size() < c_MaxSamples) {
            p_Entry.m_Samples.push_back(std::move(p_Sample));
        }
        else {
            p_Entry.m_Samples.back() = std::move(p_Sample);
        }
    }

    Entry* FindEntryLocked(Section p_Section, const std::string& p_Key) {
        const auto s_It = g_Entries.find(p_Key);

        if (s_It != g_Entries.end()) {
            return &s_It->second;
        }

        if (g_Entries.size() >= c_MaxEntries) {
            return nullptr;
        }

        // Decided once, because matching the name is far too slow to repeat every time the log is sorted.
        Entry& s_Entry = g_Entries[p_Key];
        s_Entry.m_Section = p_Section;
        s_Entry.m_NameIsPositional = NameLooksPositional(p_Key);
        return &s_Entry;
    }

    void FlushIfDueLocked(uint64_t p_Now) {
        if (p_Now - g_LastFlush >= c_FlushIntervalMs) {
            g_LastFlush = p_Now;
            WriteLogLocked();
        }
    }

    bool SampleDue(const Entry& p_Entry, uint64_t p_Now) {
        return p_Entry.m_Samples.size() < c_MaxSamples || p_Now - p_Entry.m_LastSample >= c_SampleIntervalMs;
    }

    void AddNumber(NumberStat& p_Stat, double p_Value) {
        p_Stat.m_Min = p_Stat.m_Seen ? (std::min)(p_Stat.m_Min, p_Value) : p_Value;
        p_Stat.m_Max = p_Stat.m_Seen ? (std::max)(p_Stat.m_Max, p_Value) : p_Value;
        p_Stat.m_Seen = true;
    }

    /** The address identifying a Scaleform object value, or 0 if the value holds no object. */
    uintptr_t ObjectData(const void* p_Value) {
        if (!p_Value) {
            return 0;
        }

        const auto* s_Value = static_cast<const uint8_t*>(p_Value);
        uint32_t s_Raw = 0;
        uintptr_t s_Data = 0;

        if (!TryCopy(s_Value + c_ValueType, &s_Raw, sizeof(s_Raw)) || !TryCopy(s_Value + c_ValueData, &s_Data, sizeof(s_Data))) {
            return 0;
        }

        return IsObjectType(s_Raw) ? s_Data : 0;
    }

    /** The distinct keys of a JSON description as ` [a,b,c]`, or nothing for other descriptions. */
    std::string JsonKeys(const std::string& p_Description) {
        std::set<std::string> s_Keys;

        for (size_t s_End = p_Description.find("\":"); s_End != std::string::npos; s_End = p_Description.find("\":", s_End + 2)) {
            const size_t s_Begin = p_Description.rfind('"', s_End - 1);

            if (s_Begin != std::string::npos) {
                s_Keys.insert(p_Description.substr(s_Begin + 1, s_End - s_Begin - 1));
            }
        }

        if (s_Keys.empty()) {
            return {};
        }

        std::string s_Text;

        for (const auto& s_Key : s_Keys) {
            s_Text += (s_Text.empty() ? "" : ",") + s_Key;
        }

        return std::format(" [{}]", s_Text.substr(0, 200));
    }

    uint64_t __fastcall InvokeDetour(void* p_This, const char* p_Name, void* p_Result, void* p_Args, uint64_t p_Count) {
        const bool s_Enabled = g_Enabled.load(std::memory_order_relaxed);

        if (s_Enabled) {
            UiTrace::NoteInvoke(p_Name, p_This, p_Args, p_Count);
        }

        const uint64_t s_Returned = g_Invoke(p_This, p_Name, p_Result, p_Args, p_Count);

        if (s_Enabled) {
            UiTrace::NoteAttach(p_Name, p_This, p_Result, p_Args, p_Count);
        }

        return s_Returned;
    }

    uint64_t __fastcall ObjectWrapperDetour(void* p_This, const char* p_Name, void* p_Object, void* p_Fourth) {
        if (g_Enabled.load(std::memory_order_relaxed)) {
            UiTrace::NoteTypedCall("object", p_Name, p_Object);
        }

        return g_ObjectWrapper(p_This, p_Name, p_Object, p_Fourth);
    }

    uint64_t __fastcall DataWrapperDetour(void* p_This, const char* p_Name, void* p_Object, void* p_Fourth) {
        if (g_Enabled.load(std::memory_order_relaxed)) {
            UiTrace::NoteTypedCall("data", p_Name, p_Object);
        }

        return g_DataWrapper(p_This, p_Name, p_Object, p_Fourth);
    }

    const GameHooks::Site c_Sites[] = {
        {
            "generic UI invoke", 0x12E060,
            { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55 },
            reinterpret_cast<void*>(&InvokeDetour), reinterpret_cast<void**>(&g_Invoke)
        },
        {
            "typed object wrapper", 0x142F430,
            { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48 },
            reinterpret_cast<void*>(&ObjectWrapperDetour), reinterpret_cast<void**>(&g_ObjectWrapper)
        },
        {
            "data wrapper", 0x1437950,
            { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00 },
            reinterpret_cast<void*>(&DataWrapperDetour), reinterpret_cast<void**>(&g_DataWrapper)
        },
    };

    GameHooks::HookSet g_Hooks(c_Sites);
}

void UiTrace::NoteInvoke(const char* p_Method, const void* p_Receiver, const void* p_Args, uint64_t p_Count) {
    const Decoded s_Args = DecodeArgs(p_Args, p_Count);
    const std::string s_Key = std::format("invoke {}({})", p_Method ? ReadText(p_Method) : std::string("<self>"), s_Args.m_Signature);
    const uint64_t s_Now = GetTickCount64();

    std::scoped_lock s_Lock(g_Mutex);
    Entry* s_Entry = FindEntryLocked(Section::Invoke, s_Key);

    if (s_Entry) {
        ++s_Entry->m_Count;

        for (int i = 0; i < c_MaxArgs; ++i) {
            if (s_Args.m_IsNumber[i]) {
                AddNumber(s_Entry->m_Numbers[i], s_Args.m_Numbers[i]);
            }
        }

        if (SampleDue(*s_Entry, s_Now)) {
            s_Entry->m_LastSample = s_Now;
            AddSample(*s_Entry, std::format("on {}:{}", p_Receiver, s_Args.m_Text));
        }
    }

    FlushIfDueLocked(s_Now);
}

void UiTrace::NoteTypedCall(const char* p_Wrapper, const char* p_Method, void* p_Object) {
    // Outside the lock, because describing can run engine code.
    std::string s_Description = g_Describe ? g_Describe(p_Object) : std::string();

    // Listed per set of keys, otherwise the few samples of a method only ever show its most frequent data.
    const std::string s_Key = std::format(
        "typed/{} {}{}", p_Wrapper, p_Method ? ReadText(p_Method) : std::string("<self>"), JsonKeys(s_Description)
    );
    const uint64_t s_Now = GetTickCount64();

    std::scoped_lock s_Lock(g_Mutex);
    Entry* s_Entry = FindEntryLocked(Section::TypedCall, s_Key);

    if (s_Entry) {
        ++s_Entry->m_Count;

        if (SampleDue(*s_Entry, s_Now)) {
            s_Entry->m_LastSample = s_Now;
            AddSample(*s_Entry, std::move(s_Description));
        }
    }

    FlushIfDueLocked(s_Now);
}

void UiTrace::NoteAttach(const char* p_Method, const void* p_Receiver, const void* p_Result, const void* p_Args, uint64_t p_Count) {
    if (!p_Method) {
        return;
    }

    const std::string s_Method = ReadText(p_Method);
    const bool s_Attach = s_Method == "attachInstance" && p_Count >= 2;
    const bool s_Container = s_Method == "getContainer";

    if (!s_Attach && !s_Container) {
        return;
    }

    const uintptr_t s_Result = ObjectData(p_Result);

    if (s_Attach) {
        const auto* s_Args = static_cast<const uint8_t*>(p_Args);
        const Decoded s_Decoded = DecodeArgs(s_Args + c_ValueSize, 1);
        std::string s_Class = s_Decoded.m_Text;

        // The text of a string argument is ` "name"`.
        s_Class.erase(std::remove(s_Class.begin(), s_Class.end(), '"'), s_Class.end());
        s_Class.erase(0, s_Class.find_first_not_of(' '));

        const uintptr_t s_Holder = ObjectData(s_Args);

        std::scoped_lock s_Lock(g_Mutex);

        if (s_Holder) {
            g_Classes[s_Holder] = s_Class + "(holder)";
        }

        if (s_Result) {
            g_Classes[s_Result] = s_Class;
        }

        return;
    }

    const uintptr_t s_Receiver = ObjectData(p_Receiver);

    std::scoped_lock s_Lock(g_Mutex);
    const auto s_It = g_Classes.find(s_Receiver);

    if (s_Result && s_It != g_Classes.end()) {
        g_Classes[s_Result] = s_It->second + "(container)";
    }
}

void UiTrace::NoteDisplayInfo(const void* p_Data, const uint8_t* p_Info, const uint32_t* p_Callers, int p_CallerCount) {
    uint16_t s_Flags = 0;
    double s_Values[4] {};

    if (!TryCopy(p_Info + c_DisplayFlags, &s_Flags, sizeof(s_Flags)) ||
        !TryCopy(p_Info + c_DisplayX, &s_Values[0], sizeof(double)) || !TryCopy(p_Info + c_DisplayY, &s_Values[1], sizeof(double)) ||
        !TryCopy(p_Info + c_DisplayRotation, &s_Values[2], sizeof(double)) || !TryCopy(p_Info + c_DisplayZ, &s_Values[3], sizeof(double))) {
        return;
    }

    if ((s_Flags & c_DisplayPlacement) == 0) {
        return;
    }

    std::string s_Path;

    for (int i = 0; i < p_CallerCount; ++i) {
        s_Path += std::format("{}{:#x}", i ? "<" : "", p_Callers[i]);
    }

    const uint64_t s_Now = GetTickCount64();

    std::scoped_lock s_Lock(g_Mutex);

    const auto s_Class = g_Classes.find(reinterpret_cast<uintptr_t>(p_Data));
    const std::string s_Key = std::format(
        "display flags={:#x} class={} path={}", s_Flags, s_Class != g_Classes.end() ? s_Class->second : std::string("?"), s_Path
    );

    Entry* s_Entry = FindEntryLocked(Section::Display, s_Key);

    if (s_Entry) {
        ++s_Entry->m_Count;

        const uint16_t s_Masks[4] = { c_DisplayHasX, c_DisplayHasY, c_DisplayHasRotation, c_DisplayHasZ };

        for (int i = 0; i < 4; ++i) {
            if (s_Flags & s_Masks[i]) {
                AddNumber(s_Entry->m_Numbers[i], s_Values[i]);
            }
        }
    }

    FlushIfDueLocked(s_Now);
}

void UiTrace::NoteProjection(uint32_t p_Caller, float p_X, float p_Y, uint32_t p_Edges) {
    const std::string s_Key = std::format("projection caller={:#x} edges={}", p_Caller, p_Edges);

    std::scoped_lock s_Lock(g_Mutex);
    Entry* s_Entry = FindEntryLocked(Section::Projection, s_Key);

    if (s_Entry) {
        ++s_Entry->m_Count;
        AddNumber(s_Entry->m_Numbers[0], p_X);
        AddNumber(s_Entry->m_Numbers[1], p_Y);
    }
}

void UiTrace::NoteViewport(float p_X, float p_Y, float p_Width, float p_Height, const void* p_Target, bool p_AfterFlip) {
    const std::string s_Key = std::format(
        "viewport x={:.0f} y={:.0f} w={:.0f} h={:.0f} target {} {}", p_X, p_Y, p_Width, p_Height, p_Target,
        p_AfterFlip ? "after flip" : "before flip"
    );

    std::scoped_lock s_Lock(g_Mutex);
    Entry* s_Entry = FindEntryLocked(Section::Viewport, s_Key);

    if (s_Entry) {
        ++s_Entry->m_Count;
    }
}

bool UiTrace::Prepare(void* p_Module, std::string* p_Error) {
    return g_Hooks.Prepare(p_Module, p_Error);
}

bool UiTrace::Install(void* p_Module, DescribeObject_t p_Describe, std::string* p_Error) {
    g_Describe = p_Describe;
    return g_Hooks.Prepare(p_Module, p_Error) && g_Hooks.Enable(p_Error);
}

void UiTrace::Uninstall() {
    if (!g_Hooks.IsPrepared()) {
        return;
    }

    SetEnabled(false);
    g_Hooks.Remove();
}

void UiTrace::SetLogPath(const std::wstring& p_Path) {
    std::scoped_lock s_Lock(g_Mutex);
    g_LogPath = p_Path;
}

void UiTrace::SetEnabled(bool p_Enabled) {
    const bool s_Was = g_Enabled.exchange(p_Enabled);

    if (s_Was && !p_Enabled) {
        Flush();
    }
}

bool UiTrace::IsEnabled() {
    return g_Enabled.load(std::memory_order_relaxed);
}

void UiTrace::Flush() {
    std::scoped_lock s_Lock(g_Mutex);
    WriteLogLocked();
}

size_t UiTrace::GetEntryCount() {
    std::scoped_lock s_Lock(g_Mutex);
    return g_Entries.size();
}
