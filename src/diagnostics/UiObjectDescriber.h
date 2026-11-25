// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

/** Text for a typed engine object (a ZObjectRef) passed to the UI, for UiTrace. Structured data becomes JSON, the rest a float dump. */
std::string DescribeUiObject(void* p_Object);
