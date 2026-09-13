// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Android/Yuzu compatibility shim: keep legacy includes of "core/hle/result.h"
// unified with nxemu-os' canonical Result definitions. Including the full copy
// here causes duplicate definitions when a translation unit also includes
// "nxemu-os/core/hle/result.h" through merged upstream code.
#include "nxemu-os/core/hle/result.h"
