// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace VideoCommon {
// Native descriptors address the selected view directly. The legacy NVN
// layer-base bookkeeping is not an additional array prefix for this ABI.
// Reference: Djihads80/Eglantine nx2 image_info.h, 9c55e70d79.
template <typename Entry>
Entry NativeTextureViewHeader(Entry entry, bool native) {
    if (native) {
        entry.layer_base_0_2.Assign(0);
        entry.layer_base_3_7.Assign(0);
        entry.layer_base_8_10.Assign(0);
    }
    return entry;
}
} // namespace VideoCommon
