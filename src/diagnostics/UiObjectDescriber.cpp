// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "diagnostics/UiObjectDescriber.h"

#include "core/GameMemory.h"

#include <IPluginInterface.h>

#include <Functions.h>
#include <Glacier/ZObject.h>
#include <Glacier/ZString.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>

namespace {
    constexpr size_t c_MaxJsonLength = 1500;
    constexpr uintptr_t c_LowestAddress = 0x10000;

    // ZString has a destructor, so the call that may fault needs a function of its own.
    bool CallDynamicToString(ZDynamicObject* p_Object, ZString* p_Out) {
        __try {
            Functions::ZDynamicObject_ToString->Call(p_Object, *p_Out);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool ReadRefInfo(void* p_Ref, char* p_Name, size_t p_NameSize, void** p_Data, uint8_t* p_Bytes, size_t p_ByteCount) {
        __try {
            const auto* s_Ref = static_cast<ZObjectRef*>(p_Ref);
            const STypeID* s_Id = s_Ref->GetTypeID();
            strncpy_s(p_Name, p_NameSize, s_Id->GetTypeInfo()->pszTypeName, _TRUNCATE);

            *p_Data = s_Ref->GetData();
            memset(p_Bytes, 0, p_ByteCount);
            memcpy(p_Bytes, *p_Data, p_ByteCount);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

std::string DescribeUiObject(void* p_Object) {
    char s_Name[160] {};
    void* s_Data = nullptr;
    uint8_t s_Bytes[64] {};

    if (!p_Object || !ReadRefInfo(p_Object, s_Name, sizeof(s_Name), &s_Data, s_Bytes, sizeof(s_Bytes))) {
        return "not a typed object";
    }

    std::string s_Text = s_Name;

    if (strcmp(s_Name, "TArray<SDynamicObjectKeyValuePair>") == 0 || strcmp(s_Name, "TArray<ZDynamicObject>") == 0) {
        ZString s_Json;

        if (CallDynamicToString(static_cast<ZDynamicObject*>(p_Object), &s_Json)) {
            s_Text += std::format(" {}", std::string_view(s_Json.c_str(), (std::min<size_t>)(s_Json.size(), c_MaxJsonLength)));
            return s_Text;
        }
    }

    // Arrays hold begin/end pointers. Follow them, because the interesting values are in the elements.
    if (strncmp(s_Name, "TArray<", 7) == 0) {
        const auto s_Begin = GameMemory::Read<uintptr_t>(s_Bytes, 0);
        s_Text += " elements:";

        if (s_Begin >= c_LowestAddress && !GameMemory::TryCopy(reinterpret_cast<const void*>(s_Begin), s_Bytes, sizeof(s_Bytes))) {
            return s_Text + " unreadable";
        }
    }

    for (size_t i = 0; i + sizeof(float) <= sizeof(s_Bytes); i += sizeof(float)) {
        s_Text += std::format(" {:.2f}", GameMemory::Read<float>(s_Bytes, i));
    }

    return s_Text;
}
