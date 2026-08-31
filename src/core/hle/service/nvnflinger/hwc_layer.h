// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "yuzu_common/math_util.h"
#include "yuzu_common/common_funcs.h"
#include "yuzu_common/fence.h"
#include "yuzu_common/nvdata.h"

namespace Service::Nvnflinger {

// hwc_layer_t::blending values
enum class LayerBlending : u32 {
    // No blending
    None = 0x100,

    // ONE / ONE_MINUS_SRC_ALPHA
    Premultiplied = 0x105,

    // SRC_ALPHA / ONE_MINUS_SRC_ALPHA
    Coverage = 0x405,
};

struct HwcLayer {
    u32 buffer_handle;
    u32 offset;
    android::PixelFormat format;
    u32 width;
    u32 height;
    u32 stride;
    s32 z_index;
    LayerBlending blending;
    android::BufferTransformFlags transform;
    Common::Rectangle<int> crop_rect;
    android::Fence acquire_fence;
};

} // namespace Service::Nvnflinger
