// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "game/ShoulderSwap.h"

#include "core/GameHooks.h"
#include "core/GameMemory.h"
#include "core/Log.h"

#include <cstdint>

using GameMemory::Read;
using GameMemory::Write;

namespace {
    constexpr const char* c_Log = "shoulder";

    // ZHitman5.
    constexpr size_t c_HeroFlags = 0xE28;
    constexpr size_t c_HeroMainCamera = 0x13B0;
    constexpr uint64_t c_HeroFlagOtherShoulder = 0x100;

    // ZHM5MainCamera.
    constexpr size_t c_MainCameraThirdPerson = 0x2D8;

    // ZHM53rdPersonCamera. The side is a byte, its current position a float from 0 to 1 that blends towards the side.
    constexpr size_t c_CameraBlendPending = 0x771;
    constexpr size_t c_CameraBlendDuration = 0x828;
    constexpr size_t c_CameraBlendElapsed = 0x82C;
    constexpr size_t c_CameraBlendStart = 0x830;
    constexpr size_t c_CameraSide = 0x834;
    constexpr size_t c_CameraSidePosition = 0x8DC;

    // The end of the game's own swap handler, which contains every offset of the camera used here.
    constexpr uintptr_t c_SwapCodeRva = 0x2FE8D3;
    const uint8_t c_SwapCode[] = {
        0x48, 0x8B, 0x83, 0xB0, 0x13, 0x00, 0x00, 0x48, 0x8B, 0x88, 0xD8, 0x02, 0x00, 0x00, 0x0F, 0xB6, 0x81, 0x34, 0x08, 0x00, 0x00,
        0x84, 0xC0, 0xC6, 0x81, 0x71, 0x07, 0x00, 0x00, 0x00, 0x41, 0x0F, 0x94, 0xC0, 0x41, 0x3A, 0xC0, 0x74, 0x40, 0x48, 0x8B, 0x41,
        0x10, 0xC7, 0x81, 0x28, 0x08, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x48, 0x8B, 0x90, 0x80, 0x02, 0x00, 0x00, 0x48, 0x85, 0xD2,
        0x74, 0x09, 0x8B, 0x42, 0x28, 0x89, 0x81, 0x28, 0x08, 0x00, 0x00, 0x8B, 0x81, 0xDC, 0x08, 0x00, 0x00, 0x89, 0x81, 0x30, 0x08,
        0x00, 0x00, 0x44, 0x88, 0x81, 0x34, 0x08, 0x00, 0x00, 0xC7, 0x81, 0x2C, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    Log::Budget g_SwapLines(24);

    const uint8_t* ThirdPersonCamera(const void* p_Hero) {
        const auto* s_MainCamera = p_Hero ? Read<const uint8_t*>(p_Hero, c_HeroMainCamera) : nullptr;
        return s_MainCamera ? Read<const uint8_t*>(s_MainCamera, c_MainCameraThirdPerson) : nullptr;
    }
}

bool ShoulderSwap::IsCompatible(const void* p_Module) {
    return p_Module != nullptr && GameHooks::CodeMatches(static_cast<const uint8_t*>(p_Module) + c_SwapCodeRva, c_SwapCode, sizeof(c_SwapCode));
}

bool ShoulderSwap::GetState(const void* p_Hero, State* p_State) {
    const uint8_t* s_Camera = ThirdPersonCamera(p_Hero);

    if (s_Camera == nullptr) {
        return false;
    }

    p_State->m_Side = Read<uint8_t>(s_Camera, c_CameraSide) != 0;
    p_State->m_Position = Read<float>(s_Camera, c_CameraSidePosition);
    return true;
}

bool ShoulderSwap::WasReset(bool p_ExpectedSide, const State& p_State) {
    return !p_ExpectedSide && p_State.m_Side && p_State.m_Position == 1.f;
}

ShoulderSwap::Plan ShoulderSwap::PlanSwap(bool p_ModEnabled, bool p_Wanted, bool p_Swapped, bool p_WasReset) {
    const bool s_Swapped = p_Swapped && !p_WasReset;
    const bool s_Wanted = p_Wanted && (p_ModEnabled || !p_WasReset);

    return { s_Wanted != s_Swapped, s_Wanted, s_Wanted };
}

bool ShoulderSwap::Swap(void* p_Hero) {
    auto* s_Camera = const_cast<uint8_t*>(ThirdPersonCamera(p_Hero));

    if (s_Camera == nullptr) {
        return false;
    }

    const bool s_NewSide = Read<uint8_t>(s_Camera, c_CameraSide) == 0;
    const float s_Target = s_NewSide ? 1.f : 0.f;

    Write<uint64_t>(p_Hero, c_HeroFlags, Read<uint64_t>(p_Hero, c_HeroFlags) ^ c_HeroFlagOtherShoulder);

    // What the game does on a key press, except that the blend starts where it would end.
    Write<uint8_t>(s_Camera, c_CameraBlendPending, 0);
    Write<float>(s_Camera, c_CameraBlendElapsed, Read<float>(s_Camera, c_CameraBlendDuration));
    Write<float>(s_Camera, c_CameraBlendStart, s_Target);
    Write<uint8_t>(s_Camera, c_CameraSide, s_NewSide ? 1 : 0);
    Write<float>(s_Camera, c_CameraSidePosition, s_Target);

    return true;
}

void ShoulderSwap::Controller::FollowMirror(bool p_Enabled) {
    m_Wanted = p_Enabled;
}

void ShoulderSwap::Controller::OnSceneLoading() {
    m_Swapped = false;
    m_LevelStarting = true;
}

void ShoulderSwap::Controller::Update(void* p_Hero, bool p_ModEnabled) {
    State s_State;

    // The camera may not exist yet in the first frames of a level, so this is simply tried again next frame.
    if (!GetState(p_Hero, &s_State)) {
        return;
    }

    if (m_LevelStarting.exchange(false)) {
        m_Wanted = p_ModEnabled;
    }

    // The game puts its camera back on the default shoulder at some points, most of all when a level's intro ends.
    const bool s_WasReset = m_Swapped && WasReset(m_ExpectedSide, s_State);
    const Plan s_Plan = PlanSwap(p_ModEnabled, m_Wanted, m_Swapped, s_WasReset);

    m_Wanted = s_Plan.m_Wanted;
    m_Swapped = s_Plan.m_Swapped;

    if (s_Plan.m_Swap) {
        Swap(p_Hero);
        GetState(p_Hero, &s_State);

        if (Log::IsVerbose() && g_SwapLines.Take()) {
            Log::Debug(c_Log, "Camera moved to side {} ({}).", s_State.m_Side, s_WasReset ? "the game had reset it" : "mirror state changed");
        }
    }

    m_ExpectedSide = s_State.m_Side;
}
