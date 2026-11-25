// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/Log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <vector>

namespace {
    constexpr size_t c_MaxPendingLines = 256;

    struct PendingLine {
        Log::Level m_Level;
        std::string m_FileLine;
        std::string m_ConsoleText;
    };

    std::mutex g_Mutex;
    FILE* g_File = nullptr;
    Log::ConsoleSink_t g_Sink = nullptr;
    bool g_Opened = false;
    std::vector<PendingLine> g_Pending;
    std::atomic<bool> g_Verbose = false;

    const char* LevelName(Log::Level p_Level) {
        switch (p_Level) {
            case Log::Level::Debug: return "DEBUG";
            case Log::Level::Info: return "INFO";
            case Log::Level::Warning: return "WARN";
            default: return "ERROR";
        }
    }

    std::string FormatFileLine(Log::Level p_Level, const char* p_Component, const std::string& p_Message) {
        SYSTEMTIME s_Time {};
        GetLocalTime(&s_Time);

        return std::format(
            "{:02}:{:02}:{:02}.{:03} {:<5} {:<8} {}\n", s_Time.wHour, s_Time.wMinute, s_Time.wSecond, s_Time.wMilliseconds,
            LevelName(p_Level), p_Component, p_Message
        );
    }

    void WriteToFile(const std::string& p_Line) {
        if (g_File) {
            fputs(p_Line.c_str(), g_File);
            fflush(g_File);
        }
    }

    bool GoesToConsole(Log::Level p_Level) {
        return p_Level >= Log::Level::Warning;
    }
}

void Log::Open(const std::wstring& p_Path, ConsoleSink_t p_Sink) {
    std::vector<PendingLine> s_Pending;

    {
        std::scoped_lock s_Lock(g_Mutex);

        if (g_Opened) {
            return;
        }

        g_File = _wfopen(p_Path.c_str(), L"w");
        g_Sink = p_Sink;
        g_Opened = true;

        for (const PendingLine& s_Line : g_Pending) {
            WriteToFile(s_Line.m_FileLine);
        }

        s_Pending = std::move(g_Pending);
        g_Pending.clear();
    }

    if (p_Sink) {
        for (const PendingLine& s_Line : s_Pending) {
            if (GoesToConsole(s_Line.m_Level)) {
                p_Sink(s_Line.m_Level, s_Line.m_ConsoleText);
            }
        }
    }
}

void Log::Close() {
    std::scoped_lock s_Lock(g_Mutex);

    if (g_File) {
        fclose(g_File);
        g_File = nullptr;
    }

    g_Sink = nullptr;
}

void Log::SetVerbose(bool p_Verbose) {
    g_Verbose.store(p_Verbose, std::memory_order_relaxed);
}

bool Log::IsVerbose() {
    return g_Verbose.load(std::memory_order_relaxed);
}

void Log::Write(Level p_Level, const char* p_Component, const std::string& p_Message) {
    std::string s_FileLine = FormatFileLine(p_Level, p_Component, p_Message);
    std::string s_ConsoleText = GoesToConsole(p_Level) ? std::format("{}: {}", p_Component, p_Message) : std::string();
    ConsoleSink_t s_Sink = nullptr;

    {
        std::scoped_lock s_Lock(g_Mutex);

        if (!g_Opened) {
            if (g_Pending.size() < c_MaxPendingLines) {
                g_Pending.push_back({ p_Level, std::move(s_FileLine), std::move(s_ConsoleText) });
            }

            return;
        }

        WriteToFile(s_FileLine);
        s_Sink = g_Sink;
    }

    if (s_Sink && GoesToConsole(p_Level)) {
        s_Sink(p_Level, s_ConsoleText);
    }
}
