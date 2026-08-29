#pragma once
// Custom mixer widgets.
//
// ImGui's stock slider is a horizontal bar with a number in it, which is fine
// for a debug panel and wrong for a mixer: faders are vertical, they show
// their scale, and they are grabbed rather than dragged along. These are drawn
// by hand for that reason, not for decoration.

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "theme.h"

namespace mix {

// Level in dBFS mapped to 0..1 of a meter's height. Amplitude is linear but
// hearing is not, so a linear meter wastes four fifths of its length.
inline float meterPosition(float amplitude) {
    if (amplitude <= 0.0f) return 0.0f;
    const float db = 20.0f * std::log10(amplitude);
    constexpr float floorDb = -54.0f;
    if (db <= floorDb) return 0.0f;
    if (db >= 0.0f) return 1.0f;
    return 1.0f - (db / floorDb);
}

// Faders are a percentage, linear in amplitude. A dB scale spends most of its
// travel on levels that are effectively silent, which makes the useful part of
// the fader a short stretch near the top; a percentage gives the whole length
// to levels you would actually choose.

struct MeterState {
    float level = 0.0f;
    float hold = 0.0f;
    float holdAge = 0.0f;
    float clipAge = 1e9f;
};

// A vertical level meter with a peak marker that hangs, and a clip indicator
// that latches -- a one-frame flash of red is exactly what you look away and
// miss.
inline void verticalMeter(MeterState& m, float peak, float dt, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float bottom = p.y + size.y;

    if (peak >= 0.999f) m.clipAge = 0.0f; else m.clipAge += dt;

    const float pos = meterPosition(peak);
    m.level = pos > m.level ? pos : (std::max)(pos, m.level - dt * 0.75f);
    if (pos >= m.hold) {
        m.hold = pos;
        m.holdAge = 0.0f;
    } else {
        m.holdAge += dt;
        if (m.holdAge > 1.2f) m.hold = (std::max)(pos, m.hold - dt * 0.5f);
    }

    dl->AddRectFilled(p, ImVec2(p.x + size.x, bottom), theme::kMeterBg, 2.0f);

    if (m.level > 0.001f) {
        const float h = size.y * m.level;
        // Segment the fill by zone rather than colouring the whole bar by its
        // tip, so a loud peak does not turn a quiet signal red.
        const float yTop = bottom - h;
        const float warnY = bottom - size.y * 0.82f;
        const float hotY  = bottom - size.y * 0.94f;

        dl->AddRectFilled(ImVec2(p.x, (std::max)(yTop, warnY)),
                          ImVec2(p.x + size.x, bottom), theme::kMeterOk, 2.0f);
        if (yTop < warnY) {
            dl->AddRectFilled(ImVec2(p.x, (std::max)(yTop, hotY)),
                              ImVec2(p.x + size.x, warnY), theme::kMeterWarn);
        }
        if (yTop < hotY) {
            dl->AddRectFilled(ImVec2(p.x, yTop), ImVec2(p.x + size.x, hotY),
                              theme::kMeterHot, 2.0f);
        }
    }

    if (m.hold > 0.01f) {
        const float y = bottom - size.y * m.hold;
        dl->AddRectFilled(ImVec2(p.x, y - 1.0f), ImVec2(p.x + size.x, y + 1.0f),
                          theme::fade(theme::kText, 0.75f));
    }

    if (m.clipAge < 2.0f) {
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + 3.0f), theme::kMeterHot);
    }

    ImGui::Dummy(size);
}

// A vertical fader over 0..100%. Returns true while being changed.
// Double-click resets to full, the one value anyone returns to exactly.
inline bool verticalFader(const char* id, float* percent, ImVec2 size, ImU32 accent,
                          bool* released = nullptr) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##f", size);
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    if (released) *released = ImGui::IsItemDeactivatedAfterEdit();

    bool changed = false;
    // A wheel notch is 2%, or a fifth of that with Ctrl held.
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            *percent = std::clamp(*percent + wheel * (ImGui::GetIO().KeyCtrl ? 0.4f : 2.0f),
                                  0.0f, 100.0f);
            changed = true;
            if (released) *released = true;   // persist it like a drag
        }
    }
    if (ImGui::IsItemActivated() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *percent = 100.0f;
        changed = true;
    } else if (active) {
        const float t = 1.0f - std::clamp((ImGui::GetIO().MousePos.y - p.y) / size.y, 0.0f, 1.0f);
        const float want = t * 100.0f;
        if (want != *percent) {
            *percent = want;
            changed = true;
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = p.x + size.x * 0.5f;
    const float trackW = 5.0f;

    dl->AddRectFilled(ImVec2(cx - trackW * 0.5f, p.y),
                      ImVec2(cx + trackW * 0.5f, p.y + size.y),
                      theme::kMeterBg, trackW * 0.5f);

    // Quarter marks, so a glance reads roughly where the fader sits.
    for (int q = 1; q <= 3; ++q) {
        const float y = p.y + size.y * (1.0f - q * 0.25f);
        dl->AddLine(ImVec2(p.x + 4.0f, y), ImVec2(p.x + size.x - 4.0f, y),
                    theme::fade(theme::kTextFaint, 0.35f), 1.0f);
    }

    const float t = std::clamp(*percent / 100.0f, 0.0f, 1.0f);
    const float y = p.y + size.y * (1.0f - t);
    dl->AddRectFilled(ImVec2(cx - trackW * 0.5f, y),
                      ImVec2(cx + trackW * 0.5f, p.y + size.y),
                      theme::fade(accent, active ? 1.0f : 0.85f), trackW * 0.5f);

    // The handle is wide and flat like a real fader cap, so the grab target
    // reads as grabbable.
    const float capW = size.x - 4.0f;
    const float capH = 13.0f;
    const ImVec2 a(cx - capW * 0.5f, y - capH * 0.5f);
    const ImVec2 b(cx + capW * 0.5f, y + capH * 0.5f);
    dl->AddRectFilled(a, b, (active || hovered) ? theme::kPanelHi : theme::kPanel, 3.0f);
    dl->AddRect(a, b, theme::fade(accent, (active || hovered) ? 1.0f : 0.6f), 3.0f, 0, 1.5f);
    dl->AddLine(ImVec2(a.x + 3.0f, y), ImVec2(b.x - 3.0f, y),
                theme::fade(accent, 0.9f), 1.0f);

    ImGui::PopID();
    return changed;
}

// A small pill-shaped toggle, used for mute and the EQ switch.
inline bool pillButton(const char* label, bool on, ImVec2 size, ImU32 onColor) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          on ? theme::fade(onColor, 0.9f) : theme::kPanelHi);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          on ? onColor : theme::fade(theme::kPanelHi, 1.3f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? onColor : theme::kLine);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          on ? IM_COL32(0x10, 0x12, 0x16, 0xFF) : theme::kTextDim);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, size.y * 0.5f);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return clicked;
}

inline void textDim(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kTextDim));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

}  // namespace mix
