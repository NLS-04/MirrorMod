// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdio>
#include <string>

/** The little the standalone tests share: a named check that prints its result, and the summary at the end. */
inline int g_Failures = 0;

inline void Check(bool p_Ok, const std::string& p_Name) {
    printf("[%s] %s\n", p_Ok ? "PASS" : "FAIL", p_Name.c_str());

    if (!p_Ok) {
        ++g_Failures;
    }
}

inline int Finish() {
    printf("\n%s (%d failures)\n", g_Failures == 0 ? "ALL PASSED" : "FAILED", g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
