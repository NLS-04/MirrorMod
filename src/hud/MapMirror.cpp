// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hud/MapMirror.h"

#include "core/GameHooks.h"
#include "core/Log.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

using namespace Scaleform;
using GameMemory::Read;
using GameMemory::Write;

namespace {
    constexpr const char* c_Log = "map";

    // Callers of the layout setter that belong to the maps: the map layer, and the three kinds of markers on it.
    constexpr uintptr_t c_MapPanReturn = 0xA673DD;
    constexpr uintptr_t c_TrackerReturn = 0x876FCB;
    constexpr uintptr_t c_NorthTrackerReturn = 0x878373;
    constexpr uintptr_t c_PlayerTrackerReturn = 0xA18D20;

    // Callers of the rotation setter that turn the map layer.
    constexpr uintptr_t c_MapRotationReturn = 0xA672B7;
    constexpr uintptr_t c_MapRotationAltReturn = 0xA67373;

    // Callers of the scale setter: the map layer's base scale, and the zoom of the pause menu's map.
    constexpr uintptr_t c_MapScaleReturn = 0xA66EA6;
    constexpr uintptr_t c_FullMapZoomReturn = 0xA6912F;

    // Also callers of the scale setter: all markers after a zoom, and a single one that becomes visible.
    constexpr uintptr_t c_TrackerScaleReturn = 0xA0AE60;
    constexpr uintptr_t c_SingleTrackerScaleReturn = 0xA0A9A6;

    // Callers of the map update that move the pause menu's map by the player's input.
    constexpr uintptr_t c_MapMouseDragReturn = 0xA670FD;
    constexpr uintptr_t c_MapStickPanReturn = 0xA67F38;
    constexpr uintptr_t c_MapScrollPanReturn = 0xA67FAE;

    // Where the popup shown while hovering a marker on the pause menu's map is placed.
    constexpr uintptr_t c_HoverPlaceReturn = 0xA6943C;

    // The owner of a map update: ZUIMinimapNavigationEntity, or ZUIMapNavigationEntity for the pause menu.
    constexpr size_t c_MapOwnerControl = 0x58;
    constexpr size_t c_MapOwnerCenter = 0xA0;
    constexpr size_t c_MapOwnerHoverOffset = 0x190;
    constexpr size_t c_MapOwnerIsFullMapSlot = 0x110 / sizeof(void*);

    constexpr size_t c_InterfaceValidSlot = 0x28 / sizeof(void*);

    // The engine's only ZUIMapTrackerManager. It owns the markers and hands them to whichever map is shown.
    constexpr uintptr_t c_TrackerManagerRva = 0x315B260;
    constexpr size_t c_ManagerTrackers = 0x10;
    constexpr size_t c_ManagerView = 0x90;
    constexpr size_t c_ViewOwner = 0x60;

    // A TArray: begin, end, and a capacity whose top bits say whether the elements are stored inside the array.
    constexpr size_t c_ArrayEnd = 0x8;
    constexpr size_t c_ArrayCapacity = 0x10;
    constexpr int c_ArrayInlineBit = 62;
    constexpr uintptr_t c_ArrayInlineCountMask = 0xFF;

    // A TEntityRef in that array, and the fields of the ZUIMapTrackerAspect it refers to.
    constexpr size_t c_TrackerEntrySize = 0x10;
    constexpr size_t c_TrackerEntryAspect = 0x8;
    constexpr size_t c_AspectFixedRotation = 0x28;
    constexpr size_t c_AspectLayer = 0x58;
    constexpr size_t c_AspectRotationSource = 0x78;
    constexpr size_t c_AspectControl = 0x88;

    // UIMapLayer values with a rotation that means something, see MapMirror::TrackerTurnAxis.
    constexpr int32_t c_LayerStaircase = 1;
    constexpr int32_t c_LayerNpc = 5;
    constexpr int32_t c_LayerSecurityCamera = 7;
    constexpr int32_t c_LayerOpponent = 13;
    constexpr int32_t c_LayerHero = 14;
    constexpr int c_LayerCount = 16;

    constexpr uintptr_t c_GfxLockRva = 0x4620F88;

    // Checked this often. Applying is idempotent, and a new map appears only with a new scene.
    constexpr uint64_t c_MinimapCheckInterval = 30;
    constexpr int c_MaxParents = 10;
    constexpr int c_MaxMinimapLayers = 4;

    using MapUpdate_t = void(__fastcall*)(void*, const float*, const void*);
    using IsFullMap_t = bool(__fastcall*)(void*);
    using IsValid_t = bool(__fastcall*)(void*);
    using HoverPlace_t = void(__fastcall*)(void*, bool, void*);
    using LayoutPosition_t = void*(__fastcall*)(void*, float*, bool);
    using GetDisplayInfo_t = bool(__fastcall*)(void*, void*, uint8_t*);
    using GetMember_t = void*(__fastcall*)(const void*, void*, const char*);
    using ReleaseValue_t = void(__fastcall*)(void*, void*, void*);

    MapUpdate_t g_MapUpdate = nullptr;
    HoverPlace_t g_HoverPlace = nullptr;
    SetVector_t g_SetScale = nullptr;
    SetDisplayInfo_t g_SetDisplayInfo = nullptr;
    GetDisplayInfo_t g_GetDisplayInfo = nullptr;
    GetMember_t g_GetMember = nullptr;
    ReleaseValue_t g_ReleaseValue = nullptr;
    LayoutPosition_t g_LayoutPosition = nullptr;
    CRITICAL_SECTION* g_GfxLock = nullptr;
    uintptr_t g_Base = 0;

    std::atomic<bool> g_Active = false;
    std::atomic<bool> g_MinimapEnabled = true;
    std::atomic<bool> g_FullMapEnabled = true;

    // Set after a fault in the engine's UI objects, which costs the map mirroring for this session but not the game.
    std::atomic<bool> g_Broken = false;

    // The HUD minimap's layers currently mirrored.
    std::atomic<uintptr_t> g_FlippedMaps[c_MaxMinimapLayers] {};

    // The pause menu's map layer while it is mirrored. The menu creates a new one whenever it wants to.
    std::atomic<uintptr_t> g_FlippedFullMap = 0;

    // The axis the pause menu's map should be mirrored on, and the one it currently is. 0 is horizontal, 1 vertical.
    std::atomic<int> g_FullMapAxis = 0;
    std::atomic<int> g_FlippedFullMapAxis = 0;

    std::atomic<uint64_t> g_FlipCount = 0;
    uint64_t g_MapChecks = 0;

    Log::Budget g_MapLines(48);
    Log::Budget g_MarkerFaultLines(4);

    std::atomic<bool> g_TrackerParentsWanted = true;
    std::mutex g_TrackerMutex;
    uintptr_t g_TrackerParents[c_MaxParents] {};
    int g_TrackerParentCount = 0;

    // Set while the map update runs for the pause menu's map, which has its own handling.
    thread_local bool t_FullMapUpdate = false;

    // The mirrored pause map whose hover popup is being placed right now, if any.
    thread_local void* t_HoverOwner = nullptr;

    struct GfxValue {
        alignas(16) uint8_t m_Bytes[c_ValueSize] {};
    };

    uintptr_t ToRva(void* p_Address) {
        return reinterpret_cast<uintptr_t>(p_Address) - g_Base;
    }

    const void* ControlValue(void* p_Interface) {
        return Read<void*>(p_Interface, c_InterfaceToValue);
    }

    bool IsActive() {
        return g_Active.load(std::memory_order_relaxed);
    }

    void ReleaseValue(GfxValue& p_Value) {
        EnterCriticalSection(g_GfxLock);

        if (Read<uint32_t>(p_Value.m_Bytes, c_ValueType) & c_ValueManagedBit) {
            g_ReleaseValue(Read<void*>(p_Value.m_Bytes, c_ValueInterface), p_Value.m_Bytes, Read<void*>(p_Value.m_Bytes, c_ValueData));
            Write<void*>(p_Value.m_Bytes, c_ValueInterface, nullptr);
        }

        LeaveCriticalSection(g_GfxLock);
    }

    struct TrackerEntries {
        const uint8_t* m_Begin;
        size_t m_Count;
    };

    TrackerEntries GetTrackerEntries(const void* p_Manager) {
        const auto* s_Array = static_cast<const uint8_t*>(p_Manager) + c_ManagerTrackers;
        const auto s_Capacity = Read<uintptr_t>(s_Array, c_ArrayCapacity);

        if ((s_Capacity >> c_ArrayInlineBit) & 1) {
            return { s_Array, s_Capacity & c_ArrayInlineCountMask };
        }

        const auto* s_Begin = Read<const uint8_t*>(s_Array, 0);
        const auto* s_End = Read<const uint8_t*>(s_Array, c_ArrayEnd);
        return { s_Begin, static_cast<size_t>(s_End - s_Begin) / c_TrackerEntrySize };
    }

    const void* TrackerAspect(const TrackerEntries& p_Entries, size_t p_Index) {
        return Read<const void*>(p_Entries.m_Begin + p_Index * c_TrackerEntrySize, c_TrackerEntryAspect);
    }

    const uint8_t* TrackerManager() {
        return reinterpret_cast<const uint8_t*>(g_Base + c_TrackerManagerRva);
    }

    /** The same test the engine makes before it applies the rotation of the entity a marker follows. */
    bool AspectRotatesWithEntity(const void* p_Aspect) {
        return Read<uint8_t>(p_Aspect, c_AspectFixedRotation) == 0 && Read<int32_t>(p_Aspect, c_AspectRotationSource) == 0;
    }

    int AspectTurnAxis(const void* p_Aspect, int p_MapAxis) {
        return MapMirror::TrackerTurnAxis(Read<int32_t>(p_Aspect, c_AspectLayer), AspectRotatesWithEntity(p_Aspect), p_MapAxis);
    }

    /** Fills p_Parents with the ancestors of p_Value, nearest first. Every entry has to be released. */
    int CollectParents(const void* p_Value, GfxValue* p_Parents) {
        const void* s_Current = p_Value;
        int s_Count = 0;

        while (s_Count < c_MaxParents) {
            g_GetMember(s_Current, p_Parents[s_Count].m_Bytes, "parent");

            if (!ValueObject(p_Parents[s_Count].m_Bytes)) {
                ReleaseValue(p_Parents[s_Count]);
                break;
            }

            s_Current = p_Parents[s_Count].m_Bytes;
            ++s_Count;
        }

        return s_Count;
    }

    bool IsMapFlipped(uintptr_t p_Object) {
        if (p_Object == 0) {
            return false;
        }

        if (g_FlippedFullMap.load(std::memory_order_relaxed) == p_Object) {
            return true;
        }

        for (const auto& s_Slot : g_FlippedMaps) {
            if (s_Slot.load(std::memory_order_relaxed) == p_Object) {
                return true;
            }
        }

        return false;
    }

    void RememberMapFlip(uintptr_t p_Object, bool p_Flipped) {
        for (auto& s_Slot : g_FlippedMaps) {
            if (p_Flipped && s_Slot == 0) {
                s_Slot = p_Object;
                return;
            }

            if (!p_Flipped && s_Slot == p_Object) {
                s_Slot = 0;
                return;
            }
        }
    }

    /** Gives the object's scale on one axis the wanted sign. Rotation and position follow from the engine's next writes. */
    bool SetScaleSign(const void* p_Value, int p_Axis, bool p_Negative, double* p_Scale) {
        const size_t s_Field = p_Axis == 0 ? c_DisplayScaleX : c_DisplayScaleY;
        alignas(16) uint8_t s_Current[c_DisplayInfoSize] {};
        alignas(16) uint8_t s_New[c_DisplayInfoSize] {};

        auto* s_Interface = Read<void*>(p_Value, c_ValueInterface);
        auto* s_Data = Read<void*>(p_Value, c_ValueData);
        bool s_Applied = false;

        EnterCriticalSection(g_GfxLock);

        __try {
            g_GetDisplayInfo(s_Interface, s_Data, s_Current);
            *p_Scale = Read<double>(s_Current, s_Field);

            if (*p_Scale != 0.0) {
                const double s_Magnitude = fabs(*p_Scale);
                Write<double>(s_New, s_Field, p_Negative ? -s_Magnitude : s_Magnitude);
                Write<uint16_t>(s_New, c_DisplayFlags, p_Axis == 0 ? c_DisplayHasScaleX : c_DisplayHasScaleY);
                g_SetDisplayInfo(s_Interface, s_Data, s_New);
                s_Applied = true;
            }
        }
        __finally {
            LeaveCriticalSection(g_GfxLock);
        }

        return s_Applied;
    }

    bool IsInterfaceValid(void* p_Interface) {
        return reinterpret_cast<IsValid_t>(Read<void**>(p_Interface, 0)[c_InterfaceValidSlot])(p_Interface);
    }

    bool IsFullMapOwner(void* p_Owner) {
        return reinterpret_cast<IsFullMap_t>(Read<void**>(p_Owner, 0)[c_MapOwnerIsFullMapSlot])(p_Owner);
    }

    /** Covers the markers that exist now. SetScaleDetour keeps it up for the scale writes that come later. */
    int ApplyTrackerFlips(int p_MapAxis, bool p_Mirrored) {
        const TrackerEntries s_Entries = GetTrackerEntries(TrackerManager());
        int s_Turned = 0;

        for (size_t i = 0; i < s_Entries.m_Count; ++i) {
            const void* s_Aspect = TrackerAspect(s_Entries, i);

            if (s_Aspect == nullptr) {
                continue;
            }

            const int s_TurnAxis = AspectTurnAxis(s_Aspect, p_MapAxis);

            if (s_TurnAxis < 0) {
                continue;
            }

            auto* s_Control = Read<uint8_t*>(s_Aspect, c_AspectControl);

            if (s_Control == nullptr || !IsInterfaceValid(s_Control + c_ControlToInterface)) {
                continue;
            }

            double s_Scale = 0.0;

            if (SetScaleSign(ControlValue(s_Control + c_ControlToInterface), s_TurnAxis, p_Mirrored, &s_Scale)) {
                ++s_Turned;
            }
        }

        return s_Turned;
    }

    /** Whether p_Map is a container that really holds the markers. Otherwise map and markers would disagree. */
    bool HoldsMarkers(uintptr_t p_Map) {
        std::scoped_lock s_Lock(g_TrackerMutex);

        for (int i = 0; i < g_TrackerParentCount; ++i) {
            if (g_TrackerParents[i] == p_Map) {
                return true;
            }
        }

        if (Log::IsVerbose() && g_MapLines.Take()) {
            Log::Debug(c_Log, "Minimap: the map layer is none of the {} ancestors of a marker, left alone.", g_TrackerParentCount);
        }

        return false;
    }

    void ApplyMinimapFlip(const void* p_MapValue, bool p_Mirrored) {
        const uintptr_t s_Map = ValueObject(p_MapValue);

        if (s_Map == 0 || IsMapFlipped(s_Map) == p_Mirrored || (p_Mirrored && !HoldsMarkers(s_Map))) {
            return;
        }

        double s_Scale = 0.0;

        if (SetScaleSign(p_MapValue, 0, p_Mirrored, &s_Scale)) {
            RememberMapFlip(s_Map, p_Mirrored);
            const int s_Turned = ApplyTrackerFlips(0, p_Mirrored);
            g_FlipCount.fetch_add(1, std::memory_order_relaxed);

            if (Log::IsVerbose() && g_MapLines.Take()) {
                Log::Debug(c_Log, "Minimap {}, scale was {}, {} markers turned.", p_Mirrored ? "mirrored" : "restored", s_Scale, s_Turned);
            }
        }
    }

    void StoreTrackerParents(const void* p_TrackerValue) {
        GfxValue s_Parents[c_MaxParents];
        const int s_Count = CollectParents(p_TrackerValue, s_Parents);

        {
            std::scoped_lock s_Lock(g_TrackerMutex);
            g_TrackerParentCount = s_Count;

            for (int i = 0; i < s_Count; ++i) {
                g_TrackerParents[i] = ValueObject(s_Parents[i].m_Bytes);
            }
        }

        for (int i = 0; i < s_Count; ++i) {
            ReleaseValue(s_Parents[i]);
        }
    }

    /** Which kinds of markers exist, as "layer:fixed/following", for telling from the log how each kind is treated. */
    std::string DescribeTrackers() {
        const TrackerEntries s_Entries = GetTrackerEntries(TrackerManager());
        int s_Counts[c_LayerCount][2] {};

        for (size_t i = 0; i < s_Entries.m_Count; ++i) {
            const void* s_Aspect = TrackerAspect(s_Entries, i);
            const int32_t s_Layer = s_Aspect ? Read<int32_t>(s_Aspect, c_AspectLayer) : -1;

            if (s_Layer >= 0 && s_Layer < c_LayerCount) {
                ++s_Counts[s_Layer][AspectRotatesWithEntity(s_Aspect) ? 1 : 0];
            }
        }

        std::string s_Text;

        for (int i = 0; i < c_LayerCount; ++i) {
            if (s_Counts[i][0] + s_Counts[i][1] > 0) {
                s_Text += std::format(" {}:{}/{}", i, s_Counts[i][0], s_Counts[i][1]);
            }
        }

        return s_Text;
    }

    void ApplyFullMapFlip(const void* p_Interface, bool p_Mirrored) {
        auto* s_Interface = const_cast<void*>(p_Interface);

        if (!IsInterfaceValid(s_Interface)) {
            return;
        }

        const void* s_MapValue = ControlValue(s_Interface);
        const uintptr_t s_Map = ValueObject(s_MapValue);

        if (s_Map == 0) {
            return;
        }

        const bool s_Flipped = g_FlippedFullMap.load(std::memory_order_relaxed) == s_Map;
        const int s_OldAxis = g_FlippedFullMapAxis.load(std::memory_order_relaxed);
        const int s_NewAxis = g_FullMapAxis.load(std::memory_order_relaxed);

        if (s_Flipped == p_Mirrored && (!p_Mirrored || s_OldAxis == s_NewAxis)) {
            return;
        }

        double s_Scale = 0.0;
        int s_Turned = 0;

        if (s_Flipped) {
            SetScaleSign(s_MapValue, s_OldAxis, false, &s_Scale);
            s_Turned = ApplyTrackerFlips(s_OldAxis, false);
            g_FlippedFullMap = 0;
        }

        if (p_Mirrored && SetScaleSign(s_MapValue, s_NewAxis, true, &s_Scale)) {
            g_FlippedFullMapAxis = s_NewAxis;
            g_FlippedFullMap = s_Map;
            s_Turned = ApplyTrackerFlips(s_NewAxis, true);
        }

        g_FlipCount.fetch_add(1, std::memory_order_relaxed);

        if (Log::IsVerbose() && g_MapLines.Take()) {
            Log::Debug(
                c_Log, "Pause map {} on axis {}, scale was {}, {} markers turned. Layers:{}", p_Mirrored ? "mirrored" : "restored", s_NewAxis, s_Scale,
                s_Turned, DescribeTrackers()
            );
        }
    }

    /** A fault in the UI objects must cost the map mirroring, not the game. Returns false if there was one. */
    bool GuardedStep(void (*p_Step)(const void*, bool), const void* p_Value, bool p_Mirrored) {
        __try {
            p_Step(p_Value, p_Mirrored);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void RunMapStep(void (*p_Step)(const void*, bool), const void* p_Value, bool p_Mirrored) {
        if (!GuardedStep(p_Step, p_Value, p_Mirrored) && !g_Broken.exchange(true)) {
            Log::Error(c_Log, "A fault occurred in the engine's UI objects. Map mirroring is off for this session.");
        }
    }

    void OnMinimapMoved(void* p_Interface) {
        if (g_Broken || g_MapChecks++ % c_MinimapCheckInterval != 0) {
            return;
        }

        const bool s_Mirrored = IsActive() && g_MinimapEnabled.load(std::memory_order_relaxed);

        g_TrackerParentsWanted = true;
        RunMapStep(&ApplyMinimapFlip, ControlValue(p_Interface), s_Mirrored);
    }

    void OnTrackerMoved(void* p_Interface) {
        if (g_Broken || !g_TrackerParentsWanted.exchange(false)) {
            return;
        }

        RunMapStep([](const void* p_Value, bool) { StoreTrackerParents(p_Value); }, ControlValue(p_Interface), false);
    }

    void UpdateFullMap(void* p_Owner) {
        if (g_Broken) {
            return;
        }

        const bool s_Mirrored = IsActive() && g_FullMapEnabled.load(std::memory_order_relaxed);

        RunMapStep(&ApplyFullMapFlip, Read<uint8_t*>(p_Owner, c_MapOwnerControl) + c_ControlToInterface, s_Mirrored);
    }

    /** The axis this map layer is mirrored on, or -1 if it is not. */
    int FlippedMapAxis(void* p_Interface) {
        const void* s_Value = ControlValue(p_Interface);
        const uintptr_t s_Object = s_Value ? ValueObject(s_Value) : 0;

        if (s_Object != 0 && g_FlippedFullMap.load(std::memory_order_relaxed) == s_Object) {
            return g_FlippedFullMapAxis.load(std::memory_order_relaxed);
        }

        return IsMapFlipped(s_Object) ? 0 : -1;
    }

    int TrackerTurnAxisGuarded(void* p_Interface, bool* p_Faulted) {
        __try {
            const auto* s_Manager = TrackerManager();
            auto* s_View = Read<void*>(s_Manager, c_ManagerView);
            auto* s_Owner = s_View ? Read<void*>(s_View, c_ViewOwner) : nullptr;
            auto* s_Control = s_Owner ? Read<uint8_t*>(s_Owner, c_MapOwnerControl) : nullptr;

            if (s_Control == nullptr) {
                return -1;
            }

            const int s_MapAxis = FlippedMapAxis(s_Control + c_ControlToInterface);

            if (s_MapAxis < 0) {
                return -1;
            }

            const void* s_Aspect = MapMirror::FindTrackerAspect(s_Manager, static_cast<uint8_t*>(p_Interface) - c_ControlToInterface);
            return s_Aspect ? AspectTurnAxis(s_Aspect, s_MapAxis) : -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            *p_Faulted = true;
            return -1;
        }
    }

    /** The axis to negate for the marker behind this interface, given the map it currently lies on, or -1. */
    int CurrentTrackerTurnAxis(void* p_Interface) {
        bool s_Faulted = false;
        const int s_Axis = TrackerTurnAxisGuarded(p_Interface, &s_Faulted);

        if (s_Faulted && g_MarkerFaultLines.Take()) {
            Log::Warning(c_Log, "A fault occurred while looking up a map marker. It keeps its original orientation.");
        }

        return s_Axis;
    }

    void __fastcall MapUpdateDetour(void* p_Owner, const float* p_Position, const void* p_Rotation) {
        const uintptr_t s_Caller = ToRva(_ReturnAddress());
        const bool s_FullMap = IsFullMapOwner(p_Owner);

        if (s_FullMap) {
            UpdateFullMap(p_Owner);
        }

        // Only the player's input is reversed, so that the map keeps following the mouse. Everything else, such as
        // centering on 47 or the bounds check, deals in world positions, which the mirror does not touch.
        const bool s_PlayerInput = s_Caller == c_MapMouseDragReturn || s_Caller == c_MapStickPanReturn || s_Caller == c_MapScrollPanReturn;

        alignas(16) float s_Position[4];
        memcpy(s_Position, p_Position, sizeof(s_Position));

        if (s_FullMap && s_PlayerInput && g_FlippedFullMap.load(std::memory_order_relaxed) != 0) {
            const int s_Axis = g_FlippedFullMapAxis.load(std::memory_order_relaxed);
            s_Position[s_Axis] = MapMirror::MirrorPanTarget(Read<float>(p_Owner, c_MapOwnerCenter + s_Axis * sizeof(float)), s_Position[s_Axis]);
        }

        t_FullMapUpdate = s_FullMap;
        g_MapUpdate(p_Owner, s_Position, p_Rotation);
        t_FullMapUpdate = false;
    }

    void __fastcall HoverPlaceDetour(void* p_Owner, bool p_Show, void* p_Data) {
        t_HoverOwner = p_Show && g_FlippedFullMap.load(std::memory_order_relaxed) != 0 ? p_Owner : nullptr;
        g_HoverPlace(p_Owner, p_Show, p_Data);
        t_HoverOwner = nullptr;
    }

    void __fastcall SetScaleDetour(void* p_This, const float* p_Scale) {
        const uintptr_t s_Caller = ToRva(_ReturnAddress());
        int s_Axis = -1;

        if (s_Caller == c_MapScaleReturn || s_Caller == c_FullMapZoomReturn) {
            s_Axis = FlippedMapAxis(p_This);
        }
        else if (s_Caller == c_TrackerScaleReturn || s_Caller == c_SingleTrackerScaleReturn) {
            s_Axis = CurrentTrackerTurnAxis(p_This);
        }

        CallWithNegatedAxis(g_SetScale, p_This, p_Scale, s_Axis);
    }

    const GameHooks::Site c_Sites[] = {
        {
            "map update", 0xA67120,
            { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55 },
            reinterpret_cast<void*>(&MapUpdateDetour), reinterpret_cast<void**>(&g_MapUpdate)
        },
        {
            "map hover popup", 0xA692C0,
            { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81, 0xEC, 0xA0, 0x00 },
            reinterpret_cast<void*>(&HoverPlaceDetour), reinterpret_cast<void**>(&g_HoverPlace)
        },
        {
            "UI control scale", 0x1431290,
            { 0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x4A, 0x04 },
            reinterpret_cast<void*>(&SetScaleDetour), reinterpret_cast<void**>(&g_SetScale)
        },
    };

    const GameHooks::Import c_Imports[] = {
        {
            "display info getter", 0x16869D0,
            { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x10, 0x49, 0x89, 0x7B, 0x18, 0x55, 0x49, 0x8D, 0x6B, 0x88 },
            reinterpret_cast<void**>(&g_GetDisplayInfo)
        },
        {
            "member getter", 0x1424900,
            { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 },
            reinterpret_cast<void**>(&g_GetMember)
        },
        {
            "value release", 0x16919D0,
            { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x42, 0x18, 0x48, 0x8B, 0xDA, 0x25, 0x8F, 0x00, 0x00 },
            reinterpret_cast<void**>(&g_ReleaseValue)
        },
        {
            "layout position", 0x142DA20,
            { 0x40, 0x55, 0x56, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00 },
            reinterpret_cast<void**>(&g_LayoutPosition)
        },
    };

    GameHooks::HookSet g_Hooks(c_Sites);
}

bool MapMirror::Prepare(void* p_Module, SetDisplayInfo_t p_SetDisplayInfo, std::string* p_Error) {
    if (g_Hooks.IsPrepared()) {
        return true;
    }

    if (!GameHooks::ResolveImports(p_Module, c_Imports, p_Error) || !g_Hooks.Prepare(p_Module, p_Error)) {
        return false;
    }

    g_Base = reinterpret_cast<uintptr_t>(p_Module);
    g_GfxLock = reinterpret_cast<CRITICAL_SECTION*>(g_Base + c_GfxLockRva);
    g_SetDisplayInfo = p_SetDisplayInfo;
    return true;
}

bool MapMirror::Enable(std::string* p_Error) {
    return g_Hooks.Enable(p_Error);
}

void MapMirror::Remove() {
    g_Active = false;
    g_Hooks.Remove();
}

void MapMirror::SetActive(bool p_Active) {
    g_Active = p_Active;
}

void MapMirror::SetMinimapEnabled(bool p_Enabled) {
    g_MinimapEnabled = p_Enabled;
}

void MapMirror::SetFullMapEnabled(bool p_Enabled) {
    g_FullMapEnabled = p_Enabled;
}

void MapMirror::SetFullMapAxis(int p_Axis) {
    g_FullMapAxis = p_Axis == 1 ? 1 : 0;
}

void MapMirror::ResetScene() {
    std::scoped_lock s_Lock(g_TrackerMutex);
    g_TrackerParentCount = 0;
    g_TrackerParentsWanted = true;

    for (auto& s_Slot : g_FlippedMaps) {
        s_Slot = 0;
    }

    g_FlippedFullMap = 0;
    g_MapChecks = 0;
}

uint64_t MapMirror::GetFlipCount() {
    return g_FlipCount.load(std::memory_order_relaxed);
}

bool MapMirror::IsRotationMirrored(uintptr_t p_Caller, void* p_Control) {
    return (p_Caller == c_MapRotationReturn || p_Caller == c_MapRotationAltReturn) && FlippedMapAxis(p_Control) >= 0;
}

bool MapMirror::AdjustLayoutOffset(uintptr_t p_Caller, void* p_Control, const float* p_Offset, float* p_Adjusted) {
    if (p_Caller == c_MapPanReturn) {
        if (!t_FullMapUpdate) {
            OnMinimapMoved(p_Control);
        }

        const int s_Axis = FlippedMapAxis(p_Control);

        if (s_Axis < 0) {
            return false;
        }

        memcpy(p_Adjusted, p_Offset, sizeof(float) * 3);
        p_Adjusted[s_Axis] = -p_Adjusted[s_Axis];
        return true;
    }

    if (p_Caller == c_TrackerReturn || p_Caller == c_NorthTrackerReturn || p_Caller == c_PlayerTrackerReturn) {
        OnTrackerMoved(p_Control);
        return false;
    }

    if (p_Caller == c_HoverPlaceReturn && t_HoverOwner != nullptr) {
        alignas(16) float s_MapPosition[4] {};
        g_LayoutPosition(Read<void*>(t_HoverOwner, c_MapOwnerControl), s_MapPosition, false);

        const int s_Axis = g_FlippedFullMapAxis.load(std::memory_order_relaxed);

        memcpy(p_Adjusted, p_Offset, sizeof(float) * 3);
        p_Adjusted[s_Axis] = MirrorHoverPosition(
            p_Adjusted[s_Axis], s_MapPosition[s_Axis], Read<float>(t_HoverOwner, c_MapOwnerHoverOffset + s_Axis * sizeof(float))
        );
        return true;
    }

    return false;
}

float MapMirror::MirrorHoverPosition(float p_GamePosition, float p_MapPosition, float p_PopupOffset) {
    return 2.f * p_MapPosition - p_GamePosition + 2.f * p_PopupOffset;
}

int MapMirror::TrackerTurnAxis(int32_t p_Layer, bool p_RotatesWithEntity, int p_MapAxis) {
    switch (p_Layer) {
        case c_LayerNpc:
        case c_LayerSecurityCamera:
        case c_LayerOpponent:
        case c_LayerHero:
            return p_RotatesWithEntity ? -1 : p_MapAxis;

        case c_LayerStaircase:
            return p_MapAxis == 1 ? 0 : -1;

        default:
            return p_MapAxis;
    }
}

float MapMirror::MirrorPanTarget(float p_Current, float p_Target) {
    return 2.f * p_Current - p_Target;
}

const void* MapMirror::FindTrackerAspect(const void* p_Manager, const void* p_Control) {
    const TrackerEntries s_Entries = GetTrackerEntries(p_Manager);

    for (size_t i = 0; i < s_Entries.m_Count; ++i) {
        const void* s_Aspect = TrackerAspect(s_Entries, i);

        if (s_Aspect != nullptr && Read<const void*>(s_Aspect, c_AspectControl) == p_Control) {
            return s_Aspect;
        }
    }

    return nullptr;
}
