// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone check of the HUD corrections: the arithmetic and the refusal on unknown game versions.

#include "TestSupport.h"

#include "game/SceneEvents.h"
#include "game/ShoulderSwap.h"
#include "hud/HudMirror.h"
#include "hud/MapMirror.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
    bool Near(float p_A, float p_B) {
        return std::fabs(p_A - p_B) < 0.001f;
    }

    bool RectIs(const float* p_Rect, float p_0, float p_1, float p_2, float p_3) {
        return Near(p_Rect[0], p_0) && Near(p_Rect[1], p_1) && Near(p_Rect[2], p_2) && Near(p_Rect[3], p_3);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    {
        float s_Point[2] = { 300.f, -120.f };
        uint32_t s_Edges = 0;
        HudMirror::MirrorProjectedPoint(s_Point, &s_Edges);
        Check(Near(s_Point[0], -300.f) && Near(s_Point[1], -120.f), "a point moves to the other side of the center, height stays");
        Check(s_Edges == 0, "an on-screen point stays without edge");
    }

    {
        float s_Point[2] = { 900.f, 0.f };
        uint32_t s_Edges = 2;
        HudMirror::MirrorProjectedPoint(s_Point, &s_Edges);
        Check(s_Edges == 8, "the first side edge becomes the second");

        s_Edges = 8;
        HudMirror::MirrorProjectedPoint(s_Point, &s_Edges);
        Check(s_Edges == 2, "the second side edge becomes the first");

        s_Edges = 1 | 8;
        HudMirror::MirrorProjectedPoint(s_Point, &s_Edges);
        Check(s_Edges == (1 | 2), "the other edge bit survives a swap");

        s_Edges = 2 | 8;
        HudMirror::MirrorProjectedPoint(s_Point, &s_Edges);
        Check(s_Edges == (2 | 8), "both side edges stay both");

        float s_Twice[2] = { 123.f, 45.f };
        HudMirror::MirrorProjectedPoint(s_Twice, nullptr);
        HudMirror::MirrorProjectedPoint(s_Twice, nullptr);
        Check(Near(s_Twice[0], 123.f), "mirroring twice is the identity, without edge output");
    }

    {
        const float s_In[4] = { 1500.f, 40.f, 380.f, 214.f };
        float s_Out[4];
        HudMirror::MirrorPipRect(s_In, s_Out, 1920);
        Check(RectIs(s_Out, 40.f, 40.f, 380.f, 214.f), "a rectangle at the right moves to the left, keeping its size");

        float s_Back[4];
        HudMirror::MirrorPipRect(s_Out, s_Back, 1920);
        Check(RectIs(s_Back, 1500.f, 40.f, 380.f, 214.f), "and mirroring it again returns it");
    }

    {
        const float s_Corners[4] = { 1500.f, 40.f, 1880.f, 254.f };
        float s_Out[4];
        HudMirror::MirrorPipRect(s_Corners, s_Out, 1920);
        Check(RectIs(s_Out, 40.f, 40.f, 420.f, 254.f), "a rectangle given as corners stays ordered");
    }

    {
        const float s_Hidden[4] = { 0.f, 0.f, 0.f, 0.f };
        float s_Out[4];
        HudMirror::MirrorPipRect(s_Hidden, s_Out, 1920);
        Check(RectIs(s_Out, 0.f, 0.f, 0.f, 0.f), "an empty rectangle stays empty");

        const float s_In[4] = { 1500.f, 40.f, 380.f, 214.f };
        HudMirror::MirrorPipRect(s_In, s_Out, 0);
        Check(RectIs(s_Out, 1500.f, 40.f, 380.f, 214.f), "without a known width nothing moves");
    }

    {
        const auto s_Double = [](const uint8_t* p_Info, size_t p_Offset) {
            double s_Value = 0.0;
            memcpy(&s_Value, p_Info + p_Offset, sizeof(s_Value));
            return s_Value;
        };

        const auto s_Fill = [](uint8_t* p_Info, uint16_t p_Flags) {
            const double s_Values[9] = { 300.0, 120.0, 25.0, 1.0, 1.0, 1.0, 0.0, 10.0, 15.0 };
            memcpy(p_Info, s_Values, sizeof(s_Values));
            memcpy(p_Info + 0xD4, &p_Flags, sizeof(p_Flags));
        };

        uint8_t s_Info[0xE8] {};
        s_Fill(s_Info, 0x1B87);
        HudMirror::MirrorPromptDisplayInfo(s_Info);

        Check(s_Double(s_Info, 0x00) == -300.0, "a tilted prompt moves to the other side");
        Check(s_Double(s_Info, 0x08) == 120.0, "its height stays");
        Check(s_Double(s_Info, 0x10) == -25.0, "its rotation in the screen plane turns around");
        Check(s_Double(s_Info, 0x18) == 1.0 && s_Double(s_Info, 0x20) == 1.0, "its scale stays positive, so the text stays readable");
        Check(s_Double(s_Info, 0x38) == 10.0, "the tilt around the horizontal axis stays");
        Check(s_Double(s_Info, 0x40) == -15.0, "the tilt around the vertical axis turns around");

        HudMirror::MirrorPromptDisplayInfo(s_Info);
        Check(s_Double(s_Info, 0x00) == 300.0 && s_Double(s_Info, 0x10) == 25.0 && s_Double(s_Info, 0x40) == 15.0, "applying it twice restores the values");

        s_Fill(s_Info, 0x2);
        HudMirror::MirrorPromptDisplayInfo(s_Info);
        Check(s_Double(s_Info, 0x00) == 300.0 && s_Double(s_Info, 0x10) == 25.0, "fields without a set flag are left alone");
    }

    {
        // Map at 500, marker 120 to its right, popup 30 further: the mirrored marker is at 380, its popup at 410.
        Check(Near(MapMirror::MirrorHoverPosition(650.f, 500.f, 30.f), 410.f), "the hover popup follows the mirrored marker and keeps its side");
        Check(Near(MapMirror::MirrorHoverPosition(530.f, 500.f, 30.f), 530.f), "a marker at the map's origin keeps its popup in place");
    }

    {
        const int c_Text = 3;
        const int c_Stairs = 1;
        const int c_Npc = 5;
        const int c_Hero = 14;

        Check(MapMirror::TrackerTurnAxis(c_Text, false, 0) == 0, "text on a horizontally mirrored map is turned back horizontally");
        Check(MapMirror::TrackerTurnAxis(c_Text, false, 1) == 1, "and on a vertically mirrored map vertically");
        Check(
            MapMirror::TrackerTurnAxis(c_Npc, true, 0) == -1 && MapMirror::TrackerTurnAxis(c_Npc, true, 1) == -1,
            "characters turning with their entity stay mirrored with the map"
        );
        Check(MapMirror::TrackerTurnAxis(c_Hero, true, 1) == -1, "and so does the player");
        Check(MapMirror::TrackerTurnAxis(c_Hero, false, 1) == 1, "a fixed icon on the same layer is turned back");
        Check(MapMirror::TrackerTurnAxis(c_Stairs, false, 0) == -1, "staircases stay as they are on a horizontally mirrored map");
        Check(MapMirror::TrackerTurnAxis(c_Stairs, false, 1) == 0, "on a vertically mirrored map they use the other axis");
    }

    {
        std::vector<uint8_t> s_Hero(0x2000);
        std::vector<uint8_t> s_Main(0x400);
        std::vector<uint8_t> s_Camera(0x1000);

        const auto s_Put = [](std::vector<uint8_t>& p_Memory, size_t p_Offset, const auto& p_Value) {
            memcpy(p_Memory.data() + p_Offset, &p_Value, sizeof(p_Value));
        };
        const auto s_GetFloat = [](const std::vector<uint8_t>& p_Memory, size_t p_Offset) {
            float s_Value = 0.f;
            memcpy(&s_Value, p_Memory.data() + p_Offset, sizeof(s_Value));
            return s_Value;
        };

        ShoulderSwap::State s_ReadState;
        Check(!ShoulderSwap::Swap(nullptr), "without a player nothing is swapped");
        Check(!ShoulderSwap::GetState(s_Hero.data(), &s_ReadState), "without a camera there is no state");
        Check(!ShoulderSwap::Swap(s_Hero.data()), "without a camera nothing is swapped");

        s_Put(s_Hero, 0x13B0, s_Main.data());
        s_Put(s_Main, 0x2D8, s_Camera.data());
        s_Put(s_Camera, 0x828, 0.25f);
        s_Put(s_Camera, 0x8DC, 1.f);
        s_Camera[0x834] = 1;

        Check(ShoulderSwap::Swap(s_Hero.data()), "with a camera the shoulder is swapped");
        Check(s_Camera[0x834] == 0, "the side changes");
        Check(Near(s_GetFloat(s_Camera, 0x8DC), 0.f) && Near(s_GetFloat(s_Camera, 0x830), 0.f), "the camera is at the new side at once");
        Check(Near(s_GetFloat(s_Camera, 0x82C), 0.25f), "and the blend is over");
        Check((s_Hero[0xE29] & 1) == 1, "47's own flag changes like on a key press");

        ShoulderSwap::Swap(s_Hero.data());
        Check(s_Camera[0x834] == 1 && Near(s_GetFloat(s_Camera, 0x8DC), 1.f), "swapping again returns to the first shoulder");
        Check((s_Hero[0xE29] & 1) == 0, "and to the first flag");

        Check(ShoulderSwap::GetState(s_Hero.data(), &s_ReadState) && s_ReadState.m_Side && Near(s_ReadState.m_Position, 1.f), "the state can be read");

        Check(ShoulderSwap::WasReset(false, { true, 1.f }), "a jump to the default shoulder is a reset by the game");
        Check(!ShoulderSwap::WasReset(false, { true, 0.1f }), "a blend towards it is a key press, not a reset");
        Check(!ShoulderSwap::WasReset(true, { true, 1.f }), "staying on the default shoulder is no reset");
        Check(!ShoulderSwap::WasReset(false, { false, 0.f }), "staying on the other shoulder is no reset");
    }

    {
        const auto s_Is = [](ShoulderSwap::Plan p_Plan, bool p_Swap, bool p_Swapped, bool p_Wanted) {
            return p_Plan.m_Swap == p_Swap && p_Plan.m_Swapped == p_Swapped && p_Plan.m_Wanted == p_Wanted;
        };

        Check(s_Is(ShoulderSwap::PlanSwap(true, true, false, false), true, true, true), "a wanted shoulder is taken");
        Check(s_Is(ShoulderSwap::PlanSwap(false, false, true, false), true, false, false), "and left again when no longer wanted");
        Check(s_Is(ShoulderSwap::PlanSwap(false, true, true, false), false, true, true), "turning the mirror off with the option off changes nothing");
        Check(s_Is(ShoulderSwap::PlanSwap(true, true, true, false), false, true, true), "nothing to do when it is already there");
        Check(s_Is(ShoulderSwap::PlanSwap(true, true, true, true), true, true, true), "a reset by the game is undone while the mirror is on");
        Check(s_Is(ShoulderSwap::PlanSwap(false, true, true, true), false, false, false), "and accepted while it is off");
    }

    {
        std::vector<uint8_t> s_Hero(0x2000);
        std::vector<uint8_t> s_Main(0x400);
        std::vector<uint8_t> s_Camera(0x1000);
        const void* s_MainPointer = s_Main.data();
        const void* s_CameraPointer = s_Camera.data();
        memcpy(s_Hero.data() + 0x13B0, &s_MainPointer, sizeof(s_MainPointer));
        memcpy(s_Main.data() + 0x2D8, &s_CameraPointer, sizeof(s_CameraPointer));
        s_Camera[0x834] = 1;

        ShoulderSwap::Controller s_Controller;

        s_Controller.OnSceneLoading();
        s_Controller.Update(s_Hero.data(), false);
        Check(s_Camera[0x834] == 1, "a level started with the mirror off stays on the default shoulder");

        s_Controller.OnSceneLoading();
        s_Controller.Update(nullptr, true);
        Check(s_Camera[0x834] == 1, "without a camera yet nothing happens");
        s_Controller.Update(s_Hero.data(), true);
        Check(s_Camera[0x834] == 0, "a level started with the mirror on begins on the other shoulder");

        s_Controller.Update(s_Hero.data(), true);
        Check(s_Camera[0x834] == 0, "and stays there");

        s_Camera[0x834] = 1;
        const float s_Default = 1.f;
        memcpy(s_Camera.data() + 0x8DC, &s_Default, sizeof(s_Default));
        s_Controller.Update(s_Hero.data(), true);
        Check(s_Camera[0x834] == 0, "a reset by the game is undone");

        s_Controller.FollowMirror(false);
        s_Controller.Update(s_Hero.data(), false);
        Check(s_Camera[0x834] == 1, "switching the mod off goes back");

        s_Controller.FollowMirror(true);
        s_Controller.Update(s_Hero.data(), true);
        Check(s_Camera[0x834] == 0, "and switching it on again changes over");
    }

    {
        Check(!SceneEvents::IsGameVisible(false, 0), "right after the scene started the loading image is still up");
        Check(!SceneEvents::IsGameVisible(false, 999), "and shortly before the timeout too");
        Check(SceneEvents::IsGameVisible(true, 5), "the event ends the wait");
        Check(SceneEvents::IsGameVisible(false, 1000), "and without the event the timeout does");
    }

    {
        Check(Near(MapMirror::MirrorPanTarget(100.f, 130.f), 70.f), "a pan to the right becomes a pan to the left");
        Check(Near(MapMirror::MirrorPanTarget(100.f, 100.f), 100.f), "no pan stays no pan");
        Check(
            Near(MapMirror::MirrorPanTarget(100.f, MapMirror::MirrorPanTarget(100.f, 130.f)), 130.f),
            "reversing a pan twice restores it"
        );
    }

    {
        struct FakeAspect {
            uint8_t m_Bytes[0x90] {};

            FakeAspect(int32_t p_Layer, const void* p_Control) {
                memcpy(m_Bytes + 0x58, &p_Layer, sizeof(p_Layer));
                memcpy(m_Bytes + 0x88, &p_Control, sizeof(p_Control));
            }
        };

        struct FakeEntry {
            void* m_Entity;
            void* m_Aspect;
        };

        int s_ControlA = 0;
        int s_ControlB = 0;
        int s_Unknown = 0;
        FakeAspect s_Text(3, &s_ControlA);
        FakeAspect s_Stairs(1, &s_ControlB);
        FakeEntry s_Entries[3] = { { nullptr, &s_Text }, { nullptr, nullptr }, { nullptr, &s_Stairs } };

        // A heap array: begin, end, capacity without the inline bit.
        uint8_t s_Heap[0x40] {};
        const FakeEntry* s_Begin = s_Entries;
        const FakeEntry* s_End = s_Entries + 3;
        memcpy(s_Heap + 0x10, &s_Begin, sizeof(s_Begin));
        memcpy(s_Heap + 0x18, &s_End, sizeof(s_End));

        Check(MapMirror::FindTrackerAspect(s_Heap, &s_ControlA) == &s_Text, "a marker is found in a heap array");
        Check(MapMirror::FindTrackerAspect(s_Heap, &s_ControlB) == &s_Stairs, "and past an empty entry");
        Check(MapMirror::FindTrackerAspect(s_Heap, &s_Unknown) == nullptr, "an unknown control has no marker");

        // The inline form: the elements start where begin would be, the count is in the low byte of the capacity.
        uint8_t s_Inline[0x40] {};
        memcpy(s_Inline + 0x10, s_Entries, sizeof(FakeEntry));

        const uint64_t s_InlineCapacity = (uint64_t { 1 } << 62) | 1;
        memcpy(s_Inline + 0x20, &s_InlineCapacity, sizeof(s_InlineCapacity));

        Check(MapMirror::FindTrackerAspect(s_Inline, &s_ControlA) == &s_Text, "a marker is found in an inline array");
        Check(MapMirror::FindTrackerAspect(s_Inline, &s_ControlB) == nullptr, "an inline array ends at its count");
    }

    // Zeroes where the game's code would be: an unknown version must leave everything alone.
    std::vector<uint8_t> s_FakeModule(0x1700000);
    Check(!ShoulderSwap::IsCompatible(s_FakeModule.data()), "the shoulder swap refuses an unknown game version");

    std::vector<uint8_t> s_FakeEventModule(0x2000000);
    std::string s_EventError;
    Check(!SceneEvents::Prepare(s_FakeEventModule.data(), &s_EventError), "the load screen event refuses an unknown game version");
    std::string s_Error;
    const bool s_Installed = HudMirror::Install(s_FakeModule.data(), &s_Error);
    printf("[INFO] Install refused with: %s\n", s_Error.c_str());
    Check(!s_Installed && s_Error.find("does not match") != std::string::npos, "an unknown game version is refused");

    HudMirror::SetActive(true);
    Check(HudMirror::GetStats().m_ProjectedPoints == 0, "nothing was hooked");

    HudMirror::RefreshPip();
    Check(true, "without a known rectangle nothing is sent");

    return Finish();
}
