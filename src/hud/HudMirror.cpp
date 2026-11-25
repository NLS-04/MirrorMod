// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hud/HudMirror.h"

#include "core/GameHooks.h"
#include "core/Log.h"
#include "diagnostics/UiTrace.h"
#include "hud/MapMirror.h"
#include "hud/Scaleform.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <mutex>

using namespace Scaleform;

namespace {
    constexpr const char* c_Log = "hud";

    // Edge flags reported by the projection: these two are the left and the right screen edge.
    constexpr uint32_t c_EdgeA = 2;
    constexpr uint32_t c_EdgeB = 8;

    // Where the engine returns to, which tells the elements apart inside the generic setters.
    constexpr uintptr_t c_TiltedPromptReturn = 0x1430CD5;
    constexpr uintptr_t c_FlatPromptReturn = 0xB450F1;
    constexpr uintptr_t c_AttentionLayoutReturn = 0xAAB75D;
    constexpr uintptr_t c_LayoutPositionReturn = 0x142CFC0;

    // The red markers showing where a hit came from. The engine puts them at (r sin a, -r cos a) around the screen center
    // and rotates them by a, so negating x and the rotation is exactly the marker for the mirrored direction.
    constexpr uintptr_t c_HitMarkerRotationReturn = 0xA0F3EF;
    constexpr uintptr_t c_HitMarkerOffsetReturn = 0xA0F421;

    // The attention wedges around 47. Their angle is set from this function.
    constexpr uintptr_t c_WedgeCodeBegin = 0xAAAB90;
    constexpr uintptr_t c_WedgeCodeEnd = 0xAAB560;

    // The UI stage the attention ring is placed on. Its coordinates start at the left edge, unlike the prompts'.
    constexpr float c_StageWidth = 1920.f;

    constexpr int c_RotationComponent = 2;
    constexpr int c_TracedCallers = 4;

    using Project_t = bool(__fastcall*)(void*, const float*, void*, float*, float*, float*, float*, uint32_t*, void*);
    using PipViewport_t = void(__fastcall*)(void*, const float*);

    Project_t g_Project = nullptr;
    PipViewport_t g_PipViewport = nullptr;
    SetDisplayInfo_t g_SetDisplayInfo = nullptr;
    SetVector_t g_SetPosition = nullptr;
    SetVector_t g_SetLayoutOffset = nullptr;
    SetVector_t g_SetRotation = nullptr;

    // Set while the attention ring goes through the layout code, which every UI control shares.
    thread_local bool t_AttentionLayout = false;

    std::atomic<bool> g_Active = false;
    std::atomic<uint32_t> g_ScreenWidth = 0;
    uintptr_t g_Base = 0;

    std::atomic<uint64_t> g_ProjectedPoints = 0;
    std::atomic<uint64_t> g_Wedges = 0;
    std::atomic<uint64_t> g_PipRects = 0;
    std::atomic<uint64_t> g_Prompts = 0;
    std::atomic<uint64_t> g_AttentionMoves = 0;
    std::atomic<uint64_t> g_HitMarkers = 0;

    Log::Budget g_PipRectLines(16);

    // The engine sets the picture-in-picture rectangle while a level loads and then never again, so turning the mirror
    // on or off has to send it once more.
    std::mutex g_PipMutex;
    void* g_PipOwner = nullptr;
    alignas(16) float g_PipGameRect[4] {};
    bool g_PipSentMirrored = false;

    bool IsActive() {
        return g_Active.load(std::memory_order_relaxed);
    }

    void Count(std::atomic<uint64_t>& p_Counter) {
        p_Counter.fetch_add(1, std::memory_order_relaxed);
    }

    uintptr_t ToRva(void* p_Address) {
        return reinterpret_cast<uintptr_t>(p_Address) - g_Base;
    }

    bool __fastcall ProjectDetour(
        void* p_Camera, const float* p_World, void* p_Settings, float* p_Xy, float* p_Distance, float* p_Scale,
        float* p_Alpha, uint32_t* p_Edges, void* p_Ninth
    ) {
        const bool s_Visible = g_Project(p_Camera, p_World, p_Settings, p_Xy, p_Distance, p_Scale, p_Alpha, p_Edges, p_Ninth);

        if (s_Visible && UiTrace::IsEnabled()) {
            UiTrace::NoteProjection(static_cast<uint32_t>(ToRva(_ReturnAddress())), p_Xy[0], p_Xy[1], p_Edges ? *p_Edges : 0);
        }

        if (s_Visible && IsActive()) {
            HudMirror::MirrorProjectedPoint(p_Xy, p_Edges);
            Count(g_ProjectedPoints);
        }

        return s_Visible;
    }

    void __fastcall PipViewportDetour(void* p_This, const float* p_Rect) {
        // The original reads the vector with an aligned load.
        alignas(16) float s_Rect[4];
        const uint32_t s_Width = g_ScreenWidth.load(std::memory_order_relaxed);
        const bool s_Active = IsActive();

        if (s_Active) {
            HudMirror::MirrorPipRect(p_Rect, s_Rect, s_Width);
        }
        else {
            memcpy(s_Rect, p_Rect, sizeof(s_Rect));
        }

        if (s_Active && p_Rect[2] > 0.f && p_Rect[3] > 0.f) {
            Count(g_PipRects);

            if (Log::IsVerbose() && g_PipRectLines.Take()) {
                Log::Debug(
                    c_Log, "PiP rect ({}, {}, {}, {}) -> ({}, {}, {}, {}), screen width {}.", p_Rect[0], p_Rect[1], p_Rect[2], p_Rect[3], s_Rect[0],
                    s_Rect[1], s_Rect[2], s_Rect[3], s_Width
                );
            }
        }

        {
            std::scoped_lock s_Lock(g_PipMutex);
            g_PipOwner = p_This;
            memcpy(g_PipGameRect, p_Rect, sizeof(g_PipGameRect));

            // Without the screen width nothing was mirrored, so RefreshPip has to try again once it is known.
            g_PipSentMirrored = s_Active && s_Width != 0;
        }

        g_PipViewport(p_This, s_Rect);
    }

    // Must not be inlined, because the number of frames it skips counts on it being a call of its own.
    __declspec(noinline) void TraceDisplayInfo(void* p_Data, const uint8_t* p_Info) {
        void* s_Frames[c_TracedCallers] {};
        uint32_t s_Callers[c_TracedCallers] {};

        // Skips this function and the detour, so the first entry is the game code calling the setter.
        const int s_Count = RtlCaptureStackBackTrace(2, c_TracedCallers, s_Frames, nullptr);

        for (int i = 0; i < s_Count; ++i) {
            s_Callers[i] = static_cast<uint32_t>(ToRva(s_Frames[i]));
        }

        UiTrace::NoteDisplayInfo(p_Data, p_Info, s_Callers, s_Count);
    }

    bool SetDisplayInfoWithPromptMirrored(void* p_Interface, void* p_Data, uint8_t* p_Info) {
        HudMirror::MirrorPromptDisplayInfo(p_Info);
        const bool s_Result = g_SetDisplayInfo(p_Interface, p_Data, p_Info);
        HudMirror::MirrorPromptDisplayInfo(p_Info);
        Count(g_Prompts);
        return s_Result;
    }

    bool SetDisplayInfoWithRotationNegated(void* p_Interface, void* p_Data, uint8_t* p_Info) {
        const auto s_Rotation = GameMemory::Read<double>(p_Info, c_DisplayRotation);

        GameMemory::Write<double>(p_Info, c_DisplayRotation, -s_Rotation);
        const bool s_Result = g_SetDisplayInfo(p_Interface, p_Data, p_Info);
        GameMemory::Write<double>(p_Info, c_DisplayRotation, s_Rotation);

        Count(g_Wedges);
        return s_Result;
    }

    bool __fastcall SetDisplayInfoDetour(void* p_Interface, void* p_Data, uint8_t* p_Info) {
        if (UiTrace::IsEnabled()) {
            TraceDisplayInfo(p_Data, p_Info);
        }

        if (IsActive()) {
            const uintptr_t s_Caller = ToRva(_ReturnAddress());

            if (s_Caller == c_TiltedPromptReturn) {
                return SetDisplayInfoWithPromptMirrored(p_Interface, p_Data, p_Info);
            }

            const auto s_Flags = GameMemory::Read<uint16_t>(p_Info, c_DisplayFlags);

            if (s_Caller >= c_WedgeCodeBegin && s_Caller < c_WedgeCodeEnd && (s_Flags & c_DisplayHasRotation)) {
                return SetDisplayInfoWithRotationNegated(p_Interface, p_Data, p_Info);
            }
        }

        return g_SetDisplayInfo(p_Interface, p_Data, p_Info);
    }

    void __fastcall SetPositionDetour(void* p_This, const float* p_Position) {
        if (IsActive()) {
            const uintptr_t s_Caller = ToRva(_ReturnAddress());
            const bool s_FlatPrompt = s_Caller == c_FlatPromptReturn;
            const bool s_Attention = t_AttentionLayout && s_Caller == c_LayoutPositionReturn;

            if (s_FlatPrompt || s_Attention) {
                float s_Mirrored[3];
                memcpy(s_Mirrored, p_Position, sizeof(s_Mirrored));

                // Prompts are placed relative to the screen center, the ring in stage coordinates.
                s_Mirrored[0] = s_FlatPrompt ? -s_Mirrored[0] : c_StageWidth - s_Mirrored[0];
                Count(s_FlatPrompt ? g_Prompts : g_AttentionMoves);

                g_SetPosition(p_This, s_Mirrored);
                return;
            }
        }

        g_SetPosition(p_This, p_Position);
    }

    void __fastcall SetRotationDetour(void* p_This, const float* p_Rotation) {
        const uintptr_t s_Caller = ToRva(_ReturnAddress());
        const bool s_Negate = MapMirror::IsRotationMirrored(s_Caller, p_This) || (s_Caller == c_HitMarkerRotationReturn && IsActive());

        CallWithNegatedAxis(g_SetRotation, p_This, p_Rotation, s_Negate ? c_RotationComponent : -1);
    }

    void __fastcall SetLayoutOffsetDetour(void* p_This, const float* p_Offset) {
        const uintptr_t s_Caller = ToRva(_ReturnAddress());
        float s_Adjusted[3];

        if (MapMirror::AdjustLayoutOffset(s_Caller, p_This, p_Offset, s_Adjusted)) {
            g_SetLayoutOffset(p_This, s_Adjusted);
            return;
        }

        if (s_Caller == c_HitMarkerOffsetReturn && IsActive()) {
            Count(g_HitMarkers);
            CallWithNegatedAxis(g_SetLayoutOffset, p_This, p_Offset, 0);
            return;
        }

        t_AttentionLayout = s_Caller == c_AttentionLayoutReturn && IsActive();
        g_SetLayoutOffset(p_This, p_Offset);
        t_AttentionLayout = false;
    }

    const GameHooks::Site c_Sites[] = {
        {
            "world to HUD projection", 0xA15970,
            { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48 },
            reinterpret_cast<void*>(&ProjectDetour), reinterpret_cast<void**>(&g_Project)
        },
        {
            "picture-in-picture viewport", 0xA2D760,
            { 0x48, 0x83, 0xEC, 0x38, 0x0F, 0x10, 0x89, 0xB8, 0x00, 0x00, 0x00, 0x0F, 0x28, 0x02, 0x0F, 0x2E },
            reinterpret_cast<void*>(&PipViewportDetour), reinterpret_cast<void**>(&g_PipViewport)
        },
        {
            "display info setter", 0x169F920,
            { 0x4C, 0x8B, 0xDC, 0x55, 0x57, 0x49, 0x8D, 0x6B, 0xC8, 0x48, 0x81, 0xEC, 0x28, 0x01, 0x00, 0x00 },
            reinterpret_cast<void*>(&SetDisplayInfoDetour), reinterpret_cast<void**>(&g_SetDisplayInfo)
        },
        {
            "UI control position", 0x14306B0,
            { 0x48, 0x81, 0xEC, 0x18, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x81, 0xD0, 0x01, 0x00, 0x00, 0x0F },
            reinterpret_cast<void*>(&SetPositionDetour), reinterpret_cast<void**>(&g_SetPosition)
        },
        {
            "UI control rotation", 0x1430810,
            { 0x48, 0x81, 0xEC, 0x18, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x81, 0xDC, 0x01, 0x00, 0x00, 0x0F },
            reinterpret_cast<void*>(&SetRotationDetour), reinterpret_cast<void**>(&g_SetRotation)
        },
        {
            "UI control layout offset", 0x1430080,
            { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xF2, 0x0F, 0x10, 0x02, 0x48, 0x8B, 0xD9, 0xF2, 0x0F, 0x11 },
            reinterpret_cast<void*>(&SetLayoutOffsetDetour), reinterpret_cast<void**>(&g_SetLayoutOffset)
        },
    };

    GameHooks::HookSet g_Hooks(c_Sites);
}

void HudMirror::MirrorProjectedPoint(float* p_Xy, uint32_t* p_Edges) {
    p_Xy[0] = -p_Xy[0];

    if (!p_Edges) {
        return;
    }

    const uint32_t s_Edges = *p_Edges;
    const uint32_t s_Others = s_Edges & ~(c_EdgeA | c_EdgeB);

    *p_Edges = s_Others | ((s_Edges & c_EdgeA) ? c_EdgeB : 0) | ((s_Edges & c_EdgeB) ? c_EdgeA : 0);
}

void HudMirror::MirrorPipRect(const float* p_In, float* p_Out, uint32_t p_ScreenWidth) {
    memcpy(p_Out, p_In, sizeof(float) * 4);

    if (p_ScreenWidth == 0 || p_In[2] <= 0.f || p_In[3] <= 0.f) {
        return;
    }

    const float s_Width = static_cast<float>(p_ScreenWidth);

    // Read as x, y, width, height unless that would leave the screen, which only corners can explain.
    if (p_In[0] + p_In[2] > s_Width + 1.f) {
        p_Out[0] = s_Width - p_In[2];
        p_Out[2] = s_Width - p_In[0];
    }
    else {
        p_Out[0] = s_Width - p_In[0] - p_In[2];
    }
}

void HudMirror::MirrorPromptDisplayInfo(uint8_t* p_Info) {
    const auto s_Flags = GameMemory::Read<uint16_t>(p_Info, c_DisplayFlags);

    const auto s_Negate = [&](uint16_t p_Flag, size_t p_Offset) {
        if (s_Flags & p_Flag) {
            GameMemory::Write<double>(p_Info, p_Offset, -GameMemory::Read<double>(p_Info, p_Offset));
        }
    };

    s_Negate(c_DisplayHasX, c_DisplayX);
    s_Negate(c_DisplayHasRotation, c_DisplayRotation);
    s_Negate(c_DisplayHasRotationY, c_DisplayRotationY);
}

void HudMirror::RefreshPip() {
    void* s_Owner = nullptr;
    alignas(16) float s_Rect[4];

    {
        std::scoped_lock s_Lock(g_PipMutex);

        if (g_PipOwner == nullptr || g_PipSentMirrored == IsActive()) {
            return;
        }

        s_Owner = g_PipOwner;
        memcpy(s_Rect, g_PipGameRect, sizeof(s_Rect));
    }

    PipViewportDetour(s_Owner, s_Rect);
}

void HudMirror::ResetScene() {
    {
        std::scoped_lock s_Lock(g_PipMutex);
        g_PipOwner = nullptr;
    }

    MapMirror::ResetScene();
}

bool HudMirror::Prepare(void* p_Module, std::string* p_Error) {
    if (g_Hooks.IsPrepared()) {
        return true;
    }

    if (!g_Hooks.Prepare(p_Module, p_Error)) {
        return false;
    }

    if (!MapMirror::Prepare(p_Module, g_SetDisplayInfo, p_Error)) {
        g_Hooks.Remove();
        return false;
    }

    g_Base = reinterpret_cast<uintptr_t>(p_Module);
    return true;
}

bool HudMirror::Install(void* p_Module, std::string* p_Error) {
    return Prepare(p_Module, p_Error) && g_Hooks.Enable(p_Error) && MapMirror::Enable(p_Error);
}

void HudMirror::Uninstall() {
    if (!g_Hooks.IsPrepared()) {
        return;
    }

    SetActive(false);

    {
        std::scoped_lock s_Lock(g_PipMutex);
        g_PipOwner = nullptr;
    }

    MapMirror::Remove();
    g_Hooks.Remove();
}

void HudMirror::SetActive(bool p_Active) {
    const bool s_Active = p_Active && g_Hooks.IsEnabled();

    g_Active = s_Active;
    MapMirror::SetActive(s_Active);
}

void HudMirror::SetScreenWidth(uint32_t p_Width) {
    g_ScreenWidth = p_Width;
}

HudMirror::Stats HudMirror::GetStats() {
    Stats s_Stats;
    s_Stats.m_ProjectedPoints = g_ProjectedPoints.load(std::memory_order_relaxed);
    s_Stats.m_Wedges = g_Wedges.load(std::memory_order_relaxed);
    s_Stats.m_PipRects = g_PipRects.load(std::memory_order_relaxed);
    s_Stats.m_Prompts = g_Prompts.load(std::memory_order_relaxed);
    s_Stats.m_AttentionMoves = g_AttentionMoves.load(std::memory_order_relaxed);
    s_Stats.m_HitMarkers = g_HitMarkers.load(std::memory_order_relaxed);
    s_Stats.m_MapFlips = MapMirror::GetFlipCount();
    return s_Stats;
}
