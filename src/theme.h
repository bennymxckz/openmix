#pragma once
// Visual language for the mixer.
//
// Kept apart from the widgets so colour and spacing decisions live in one
// place rather than being scattered as literals through the drawing code.

#include "imgui.h"

namespace theme {

// A cool, low-saturation ground so the meters and channel colours are the
// only saturated things on screen. Audio software is looked at for hours; it
// should recede.
constexpr ImU32 kBg        = IM_COL32(0x14, 0x16, 0x1B, 0xFF);
constexpr ImU32 kPanel     = IM_COL32(0x1B, 0x1E, 0x25, 0xFF);
constexpr ImU32 kPanelHi   = IM_COL32(0x23, 0x27, 0x30, 0xFF);
constexpr ImU32 kLine      = IM_COL32(0x2C, 0x31, 0x3C, 0xFF);
constexpr ImU32 kText      = IM_COL32(0xE7, 0xEA, 0xF0, 0xFF);
constexpr ImU32 kTextDim   = IM_COL32(0x8A, 0x93, 0xA3, 0xFF);
constexpr ImU32 kTextFaint = IM_COL32(0x5D, 0x66, 0x75, 0xFF);
constexpr ImU32 kAccent    = IM_COL32(0x4E, 0xC9, 0xC0, 0xFF);

// Meter colours. Green for most of the range, amber as it gets close, red at
// the top -- the convention every mixer uses, so it needs no explaining.
constexpr ImU32 kMeterOk   = IM_COL32(0x53, 0xC3, 0x82, 0xFF);
constexpr ImU32 kMeterWarn = IM_COL32(0xE0, 0xA9, 0x3C, 0xFF);
constexpr ImU32 kMeterHot  = IM_COL32(0xDE, 0x5A, 0x4F, 0xFF);
constexpr ImU32 kMeterBg   = IM_COL32(0x0E, 0x10, 0x14, 0xFF);

constexpr ImU32 kMuted     = IM_COL32(0xC4, 0x54, 0x4A, 0xFF);

// Each channel gets an identity colour so a strip can be found without
// reading its label.
inline ImU32 channelColor(size_t index) {
    static const ImU32 palette[] = {
        IM_COL32(0x5B, 0x9D, 0xF0, 0xFF),   // blue
        IM_COL32(0x5A, 0xC8, 0x8E, 0xFF),   // green
        IM_COL32(0xA9, 0x7B, 0xE8, 0xFF),   // violet
        IM_COL32(0xE8, 0x92, 0x4A, 0xFF),   // amber
        IM_COL32(0x4E, 0xC9, 0xC0, 0xFF),   // teal
        IM_COL32(0xE4, 0x6C, 0x9E, 0xFF),   // pink
        IM_COL32(0x8F, 0xB3, 0x4C, 0xFF),   // olive
        IM_COL32(0x6E, 0x8B, 0xE0, 0xFF),   // indigo
    };
    return palette[index % (sizeof(palette) / sizeof(palette[0]))];
}

inline ImVec4 toVec(ImU32 c) {
    return ImGui::ColorConvertU32ToFloat4(c);
}

inline ImU32 fade(ImU32 c, float alpha) {
    ImVec4 v = toVec(c);
    v.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

inline ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = toVec(a), y = toVec(b);
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
               x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

}  // namespace theme
