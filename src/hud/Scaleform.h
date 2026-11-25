// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/GameMemory.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

/** The layouts of Scaleform's types and of the game's UI controls, as used by game version 3.290.1.0. */
namespace Scaleform {
    // GFx::Value.
    constexpr size_t c_ValueSize = 0x30;
    constexpr size_t c_ValueInterface = 0x10;
    constexpr size_t c_ValueType = 0x18;
    constexpr size_t c_ValueData = 0x20;
    constexpr uint32_t c_ValueTypeMask = 0x8F;
    constexpr uint32_t c_ValueManagedBit = 0x40;

    enum ValueType : uint32_t {
        c_TypeUndefined = 0,
        c_TypeNull = 1,
        c_TypeBoolean = 2,
        c_TypeInt = 3,
        c_TypeUInt = 4,
        c_TypeNumber = 5,
        c_TypeString = 6,
        c_TypeObject = 8,
        c_TypeArray = 9,
        c_TypeDisplayObject = 10,
    };

    // GFx::Value::DisplayInfo. The fields are doubles, and the flags say which of them are set.
    constexpr size_t c_DisplayX = 0x00;
    constexpr size_t c_DisplayY = 0x08;
    constexpr size_t c_DisplayRotation = 0x10;
    constexpr size_t c_DisplayScaleX = 0x18;
    constexpr size_t c_DisplayScaleY = 0x20;
    constexpr size_t c_DisplayZ = 0x30;
    constexpr size_t c_DisplayRotationY = 0x40;
    constexpr size_t c_DisplayFlags = 0xD4;
    constexpr size_t c_DisplayInfoSize = 0xF0;

    constexpr uint16_t c_DisplayHasX = 0x1;
    constexpr uint16_t c_DisplayHasY = 0x2;
    constexpr uint16_t c_DisplayHasRotation = 0x4;
    constexpr uint16_t c_DisplayHasScaleX = 0x8;
    constexpr uint16_t c_DisplayHasScaleY = 0x10;
    constexpr uint16_t c_DisplayHasZ = 0x80;
    constexpr uint16_t c_DisplayHasRotationY = 0x200;
    constexpr uint16_t c_DisplayHasProjection = 0x1000;
    constexpr uint16_t c_DisplayHasView = 0x2000;

    // A UI control (ZUIControlEntity) and the interface its setters run on.
    constexpr size_t c_ControlToInterface = 0x60;
    constexpr size_t c_InterfaceToValue = 0x1B8;

    using SetVector_t = void(__fastcall*)(void*, const float*);
    using SetDisplayInfo_t = bool(__fastcall*)(void*, void*, uint8_t*);

    inline bool IsObjectType(uint32_t p_RawType) {
        const uint32_t s_Type = p_RawType & c_ValueTypeMask;
        return s_Type == c_TypeObject || s_Type == c_TypeArray || s_Type == c_TypeDisplayObject;
    }

    /** The identity of the object a value holds, or 0 if it holds none. */
    inline uintptr_t ValueObject(const void* p_Value) {
        return IsObjectType(GameMemory::Read<uint32_t>(p_Value, c_ValueType)) ? GameMemory::Read<uintptr_t>(p_Value, c_ValueData) : 0;
    }

    /** Calls a UI control's setter with one component of its vector negated, or unchanged if p_Axis is negative. */
    inline void CallWithNegatedAxis(SetVector_t p_Setter, void* p_Control, const float* p_Vector, int p_Axis) {
        if (p_Axis < 0) {
            p_Setter(p_Control, p_Vector);
            return;
        }

        float s_Mirrored[3];
        memcpy(s_Mirrored, p_Vector, sizeof(s_Mirrored));
        s_Mirrored[p_Axis] = -s_Mirrored[p_Axis];
        p_Setter(p_Control, s_Mirrored);
    }
}
