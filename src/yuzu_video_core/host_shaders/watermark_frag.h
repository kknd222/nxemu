// SPDX-FileCopyrightText: 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace HostShaders {

constexpr std::string_view WATERMARK_FRAG = {
R"(// SPDX-License-Identifier: GPL-2.0-or-later
)" R"(
)" R"(#version 460 core
)" R"(
)" R"(layout(location = 0) in vec2 tex_coord;
)" R"(layout(location = 0) out vec4 frag_color;
)" R"(
)" R"(layout(set = 0, binding = 0) uniform sampler2D watermark_texture;
)" R"(
)" R"(layout(push_constant) uniform PushConstants {
)" R"(    vec4 alpha_data;  // Use vec4 like yuzu does
)" R"(};
)" R"(
)" R"(void main() {
)" R"(    vec4 tex_color = texture(watermark_texture, tex_coord);
)" R"(    frag_color = vec4(tex_color.rgb, tex_color.a * alpha_data.x);
)" R"(}
)" 
};

} // namespace HostShaders
