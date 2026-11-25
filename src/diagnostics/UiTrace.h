// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

/**
 * Records what the engine tells the Scaleform UI, to find out which calls carry screen positions.
 * Purely observing: arguments are read and passed on unchanged.
 *
 * The result is one file with a section per kind of call. It is rewritten every few seconds while recording.
 * The hook sites are specific to game version 3.290.1.0, see GameHooks.
 */
namespace UiTrace {
    /** Describes a typed engine object (a ZObjectRef) that a UI call received. Never throws. */
    using DescribeObject_t = std::string (*)(void* p_Object);

    /** Creates the hooks without enabling them, which is safe under the loader lock. */
    bool Prepare(void* p_Module, std::string* p_Error);

    /** Enables the hooks. Must not run under the loader lock. */
    bool Install(void* p_Module, DescribeObject_t p_Describe, std::string* p_Error);
    void Uninstall();

    void SetLogPath(const std::wstring& p_Path);

    /** Starts or pauses recording. Pausing writes the log. */
    void SetEnabled(bool p_Enabled);
    bool IsEnabled();

    /** Writes the log now. It is also rewritten every few seconds while recording. */
    void Flush();

    size_t GetEntryCount();

    /** One call on a UI element. p_Args points to p_Count Scaleform values. */
    void NoteInvoke(const char* p_Method, const void* p_Receiver, const void* p_Args, uint64_t p_Count);

    /** A UI call that received a typed engine object, recorded per wrapper and method. */
    void NoteTypedCall(const char* p_Wrapper, const char* p_Method, void* p_Object);

    /** Learns which ActionScript class a display object belongs to, from the call and its result. */
    void NoteAttach(const char* p_Method, const void* p_Receiver, const void* p_Result, const void* p_Args, uint64_t p_Count);

    /** A write of display properties. p_Callers are game code addresses, nearest caller first. */
    void NoteDisplayInfo(const void* p_Data, const uint8_t* p_Info, const uint32_t* p_Callers, int p_CallerCount);

    /** One result of the engine's world to HUD projection, before mirroring. */
    void NoteProjection(uint32_t p_Caller, float p_X, float p_Y, uint32_t p_Edges);

    /** A viewport smaller than the screen set while rendering to a screen sized target. */
    void NoteViewport(float p_X, float p_Y, float p_Width, float p_Height, const void* p_Target, bool p_AfterFlip);
}
