// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <format>
#include <string>
#include <utility>

/**
 * The mod's log: one file next to the DLL, rewritten on every game start, one event per line.
 *
 *     14:03:27.512 WARN  render   Flip rehearsal failed, using the fallback: ...
 *
 * The columns are local time, level, the part of the mod reporting, and the message. Every line is flushed at once,
 * because the lines that matter most are the ones right before a crash.
 *
 * Warnings and errors also go to the SDK's console. Debug lines are only written while verbose logging is on and cost a
 * single atomic load otherwise. Anything that can repeat, especially inside a hook, has to go through a Budget.
 *
 * Lines written before Open are kept and written by it, so code running under the loader lock can log as well.
 */
namespace Log {
    enum class Level { Debug, Info, Warning, Error };

    /** Receives warnings and errors for the console, as "component: message". */
    using ConsoleSink_t = void (*)(Level p_Level, const std::string& p_Text);

    void Open(const std::wstring& p_Path, ConsoleSink_t p_Sink);
    void Close();

    void SetVerbose(bool p_Verbose);
    bool IsVerbose();

    void Write(Level p_Level, const char* p_Component, const std::string& p_Message);

    template <class... Args>
    void Debug(const char* p_Component, std::format_string<Args...> p_Format, Args&&... p_Args) {
        if (IsVerbose()) {
            Write(Level::Debug, p_Component, std::format(p_Format, std::forward<Args>(p_Args)...));
        }
    }

    template <class... Args>
    void Info(const char* p_Component, std::format_string<Args...> p_Format, Args&&... p_Args) {
        Write(Level::Info, p_Component, std::format(p_Format, std::forward<Args>(p_Args)...));
    }

    template <class... Args>
    void Warning(const char* p_Component, std::format_string<Args...> p_Format, Args&&... p_Args) {
        Write(Level::Warning, p_Component, std::format(p_Format, std::forward<Args>(p_Args)...));
    }

    template <class... Args>
    void Error(const char* p_Component, std::format_string<Args...> p_Format, Args&&... p_Args) {
        Write(Level::Error, p_Component, std::format(p_Format, std::forward<Args>(p_Args)...));
    }

    /** Lets a recurring message through a limited number of times, so that a problem in every frame cannot flood the log. */
    class Budget {
    public:
        explicit Budget(int p_Lines) : m_Left(p_Lines) {}

        bool Take() {
            return m_Left.load(std::memory_order_relaxed) > 0 && m_Left.fetch_sub(1, std::memory_order_relaxed) > 0;
        }

    private:
        std::atomic<int> m_Left;
    };
}
