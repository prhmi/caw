#pragma once
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <vector>

// CAW disclosure controls use plus/minus marks rather than arrows.
namespace caw_ui {
inline void disclosure(ImDrawList* dl, ImVec2 center, float radius, ImU32 color, bool open) {
    const float s = std::max(2.f, radius);
    const float thickness = std::max(1.f, ImGui::GetFontSize() * .085f);
    dl->AddLine(ImVec2(center.x - s, center.y), ImVec2(center.x + s, center.y), color, thickness);
    if (!open) dl->AddLine(ImVec2(center.x, center.y - s), ImVec2(center.x, center.y + s), color, thickness);
}
inline void arrow(ImDrawList* dl, ImVec2 center, ImGuiDir dir, float radius, ImU32 color) {
    const float s = std::max(1.0f, radius);
    ImVec2 tip, a, b, tail;
    if (dir == ImGuiDir_Right || dir == ImGuiDir_Left) {
        const float sign = dir == ImGuiDir_Right ? 1.f : -1.f;
        tip = ImVec2(center.x+sign*s, center.y); tail = ImVec2(center.x-sign*s, center.y);
        a = ImVec2(center.x, center.y-s); b = ImVec2(center.x, center.y+s);
    } else {
        const float sign = dir == ImGuiDir_Down ? 1.f : -1.f;
        tip = ImVec2(center.x, center.y+sign*s); tail = ImVec2(center.x, center.y-sign*s);
        a = ImVec2(center.x-s, center.y); b = ImVec2(center.x+s, center.y);
    }
    const float thickness = std::max(1.f, ImGui::GetFontSize() * .085f);
    dl->AddLine(tail, tip, color, thickness);
    dl->AddLine(a, tip, color, thickness);
    dl->AddLine(b, tip, color, thickness);
}
inline bool beginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::CalcItemWidth(), height = ImGui::GetFrameHeight();
    const bool open = ImGui::BeginCombo(label, preview, flags | ImGuiComboFlags_NoArrowButton);
    const ImVec2 left(pos.x + width - height, pos.y), right(pos.x + width, pos.y + height);
    dl->AddRectFilled(left, right, ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
    disclosure(dl, ImVec2((left.x + right.x) * .5f, (left.y + right.y) * .5f),
               height * .16f, ImGui::GetColorU32(ImGuiCol_Text), open);
    return open;
}
inline bool combo(const char* label, int* current, const char* const items[], int count) {
    const char* preview = *current >= 0 && *current < count ? items[*current] : "";
    bool changed = false;
    if (beginCombo(label, preview)) {
        for (int i = 0; i < count; ++i) {
            const bool selected = i == *current;
            if (ImGui::Selectable(items[i], selected)) { *current = i; changed = true; }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}
inline bool combo(const char* label, int* current, const char* items) {
    const char* preview = "";
    int index = 0;
    for (const char* p = items; *p; p += std::strlen(p) + 1, ++index)
        if (index == *current) preview = p;
    bool changed = false;
    if (beginCombo(label, preview)) {
        index = 0;
        for (const char* p = items; *p; p += std::strlen(p) + 1, ++index) {
            const bool selected = index == *current;
            if (ImGui::Selectable(p, selected)) { *current = index; changed = true; }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}
inline bool arrowButton(const char* id, ImGuiDir dir) {
    const float size = ImGui::GetFrameHeight();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImU32 bg = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive :
                                       ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, bg, ImGui::GetStyle().FrameRounding);
    (void)dir;
    disclosure(dl, ImVec2((a.x+b.x)*.5f, (a.y+b.y)*.5f), size*.16f, ImGui::GetColorU32(ImGuiCol_Text), false);
    return pressed;
}
inline bool treeNode(const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool open = ImGui::TreeNode(label);
    const float h = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 a(p.x, p.y), b(p.x + ImGui::GetFontSize(), p.y+h);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_HeaderHovered : ImGuiCol_WindowBg));
    disclosure(dl, ImVec2(p.x + ImGui::GetFontSize()*.45f, p.y+h*.5f),
               ImGui::GetFontSize()*.18f, ImGui::GetColorU32(ImGuiCol_Text), open);
    return open;
}
}
