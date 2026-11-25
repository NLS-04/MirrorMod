// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MirrorMod.h"

#include "audio/AudioHooks.h"
#include "core/Log.h"
#include "diagnostics/UiTrace.h"
#include "hud/HudMirror.h"
#include "render/CommandListHooks.h"

#include <IconsMaterialDesign.h>

#include <windows.h>

#include <format>

namespace {
    constexpr const char* c_Log = "mod";
    constexpr const char* c_Author = "NLS-04";
    constexpr int c_FramesPerTrace = 60;

    const ImVec4 c_ErrorColor { 1.f, 0.3f, 0.3f, 1.f };
    const ImVec4 c_WarningColor { 1.f, 0.75f, 0.2f, 1.f };

    void Tooltip(const char* p_Text) {
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", p_Text);
        }
    }

    // Width of the window's content, in multiples of the font size so that it follows the UI scale.
    constexpr float c_ContentWidthEm = 32.f;

    float ContentWidth() {
        return ImGui::GetFontSize() * c_ContentWidthEm;
    }

    // The window sizes itself to its content, so text cannot wrap at the window's edge: that edge would only move
    // inwards. It wraps at a fixed width instead, which also gives the window its width.
    void DrawWrapped(const char* p_Text) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ContentWidth());
        ImGui::TextUnformatted(p_Text);
        ImGui::PopTextWrapPos();
    }

    void DrawWrappedColored(const ImVec4& p_Color, const char* p_Text) {
        ImGui::PushStyleColor(ImGuiCol_Text, p_Color);
        DrawWrapped(p_Text);
        ImGui::PopStyleColor();
    }

    void DrawProblem(const char* p_What, const std::string& p_Error) {
        if (!p_Error.empty()) {
            DrawWrappedColored(c_ErrorColor, std::format("{}: {}", p_What, p_Error).c_str());
        }
    }
}

void MirrorMod::OnDrawMenu() {
    if (ImGui::Button(ICON_MD_FLIP " MIRROR MODE")) {
        m_ShowWindow = !m_ShowWindow;
    }
}

void MirrorMod::DrawWindow() {
    // Fits its content: grows when a section opens and shrinks when it closes. The minimum width keeps the collapsed
    // headers from shrinking to their labels.
    const ImVec2 s_Padding = ImGui::GetStyle().WindowPadding;
    ImGui::SetNextWindowSizeConstraints(ImVec2(ContentWidth() + 2.f * s_Padding.x, 0.f), ImVec2(FLT_MAX, FLT_MAX));

    if (ImGui::Begin(ICON_MD_FLIP " Mirror Mode", &m_ShowWindow, ImGuiWindowFlags_AlwaysAutoResize)) {
        bool s_Enabled = m_Settings.m_Enabled.Get();

        if (ImGui::Checkbox("Enable mirror mode", &s_Enabled)) {
            SetEnabled(s_Enabled);
        }

        DrawToggleKey();

        ImGui::Spacing();
        DrawWrappedColored(c_WarningColor, "Frame Generation is not supported. Turn it off in the game's graphics options.");
        DrawProblems();
        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Config")) {
            DrawConfig();
        }

        if (ImGui::CollapsingHeader("Developer")) {
            DrawDeveloper();
        }

        if (ImGui::CollapsingHeader("About")) {
            DrawAbout();
        }
    }

    ImGui::End();
}

void MirrorMod::DrawToggleKey() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Toggle key");
    ImGui::SameLine();

    if (!m_ToggleKeyCapture.IsActive()) {
        if (ImGui::Button(std::format("{}##toggle_key", Hotkey::KeyName(m_Settings.GetToggleKey())).c_str())) {
            m_ToggleKeyCapture.Start();
        }

        Tooltip("Click, then press the key that should switch the mirror on and off.");
        return;
    }

    ImGui::Button("Press a key (Esc cancels)##toggle_key");

    if (Hotkey::IsHeld(VK_ESCAPE)) {
        m_ToggleKeyCapture.Cancel();
    }
    else if (const std::optional<int> s_Key = m_ToggleKeyCapture.Update(Hotkey::FindHeldKey())) {
        m_Settings.SetToggleKey(*s_Key);
        Log::Info(c_Log, "Toggle key set to {}.", Hotkey::KeyName(*s_Key));
    }
}

void MirrorMod::DrawProblems() {
    DrawProblem("Renderer error", m_FrameMirror.GetError());
    DrawProblem("HUD error", m_HudError);
    DrawProblem("Audio error", m_AudioError);

    if (m_FrameMirror.IsEnginePathDisabled()) {
        DrawWrappedColored(c_ErrorColor, "The flip before the UI failed, so the UI is mirrored as well. See MirrorMod.log.");
    }
}

bool MirrorMod::DrawToggle(const char* p_Label, Settings::Toggle& p_Toggle, const char* p_Tooltip) {
    bool s_Value = p_Toggle.Get();
    const bool s_Changed = ImGui::Checkbox(p_Label, &s_Value);

    Tooltip(p_Tooltip);

    if (s_Changed) {
        m_Settings.Set(p_Toggle, s_Value);
    }

    return s_Changed;
}

void MirrorMod::DrawConfig() {
    bool s_Changed = false;

    int s_Mode = static_cast<int>(m_Settings.GetFlipMode());
    ImGui::TextUnformatted("Flip the image");
    bool s_ModeChanged = ImGui::RadioButton("Before the UI", &s_Mode, static_cast<int>(FlipMode::BeforeUi));
    ImGui::SameLine();
    s_ModeChanged |= ImGui::RadioButton("Whole frame (fallback, mirrors the UI too)", &s_Mode, static_cast<int>(FlipMode::WholeFrame));

    if (s_ModeChanged) {
        m_Settings.SetFlipMode(static_cast<FlipMode>(s_Mode));
        s_Changed = true;
    }

    ImGui::Separator();

    s_Changed |= DrawToggle("Correct HUD positions", m_Settings.m_CorrectHud, "Moves prompts, markers, the hit and detection indicators and the picture-in-picture to match the mirrored world.");
    s_Changed |= DrawToggle("Mirror the minimap", m_Settings.m_MirrorMinimap, "Needs the HUD corrections.");
    s_Changed |= DrawToggle("Mirror the map in the pause menu", m_Settings.m_MirrorFullMap, "Needs the HUD corrections.");

    int s_Axis = m_Settings.GetFullMapAxis();
    ImGui::TextUnformatted("Pause menu map axis");
    bool s_AxisChanged = ImGui::RadioButton("Horizontal", &s_Axis, 0);
    ImGui::SameLine();
    s_AxisChanged |= ImGui::RadioButton("Vertical", &s_Axis, 1);

    if (s_AxisChanged) {
        m_Settings.SetFullMapAxis(s_Axis);
        s_Changed = true;
    }

    ImGui::Separator();

    DrawToggle("Reverse horizontal look", m_Settings.m_InvertLook, "Mouse and right stick, so that the view turns the way you push.");
    DrawToggle("Reverse horizontal movement", m_Settings.m_InvertMove, "Strafe keys and left stick, so that 47 moves the way you push.");
    DrawToggle(
        "Reverse free camera controls", m_Settings.m_InvertFreeCam,
        "Turning, panning and sideways flight of the SDK's FreeCam mod, so that the camera moves the way you push."
    );
    s_Changed |= DrawToggle("Swap left and right audio", m_Settings.m_SwapAudio, "Applies to all sound of the game, including menus.");
    DrawToggle("Change shoulder when toggling the mod", m_Settings.m_SwapShoulder, "Only affects switching the mod on or off. A level always starts on the other shoulder while the mod is on.");

    if (s_Changed) {
        ApplySettings();
    }
}

void MirrorMod::DrawDeveloper() {
    const auto s_Frame = m_FrameMirror.GetStats();
    ImGui::Text("Flips: %llu before UI, %llu at present, %llu aborted", s_Frame.m_FlipsBeforeUi, s_Frame.m_FlipsAtPresent, s_Frame.m_FlipsAborted);
    ImGui::Text("UI passes without a screen target: %llu", s_Frame.m_TargetsRejected);
    ImGui::Text("Armed draws with unknown pipeline: %llu", CommandListHooks::GetStats().m_PipelineUnknown);
    ImGui::Text("Player in control: %s", m_Scene.IsPlayerInControl() ? "yes" : "no");

    const auto s_Hud = HudMirror::GetStats();
    ImGui::Text(
        "HUD: %llu positions, %llu prompts, %llu wedge angles, %llu ring moves, %llu hit markers, %llu PiP rects, %llu map flips",
        s_Hud.m_ProjectedPoints, s_Hud.m_Prompts, s_Hud.m_Wedges, s_Hud.m_AttentionMoves, s_Hud.m_HitMarkers, s_Hud.m_PipRects, s_Hud.m_MapFlips
    );

    const auto s_Audio = AudioHooks::GetStats();
    ImGui::Text("Audio: %u streams, %u channels, %u pairs, %llu buffers swapped", s_Audio.m_Streams, s_Audio.m_Channels, s_Audio.m_NumPairs, s_Audio.m_Buffers);

    ImGui::Separator();

    bool s_Verbose = Log::IsVerbose();

    if (ImGui::Checkbox("Verbose log", &s_Verbose)) {
        Log::SetVerbose(s_Verbose);
    }

    Tooltip("Adds the DEBUG lines to MirrorMod.log.");

    bool s_LogActions = m_LogActions;

    if (ImGui::Checkbox("Log used input actions", &s_LogActions)) {
        m_LogActions = s_LogActions;
    }

    bool s_UiTrace = UiTrace::IsEnabled();

    if (ImGui::Checkbox("Record UI trace", &s_UiTrace)) {
        SetUiTrace(s_UiTrace);
    }

    Tooltip("Records what the engine tells the UI into MirrorMod-uitrace.log.");
    ImGui::SameLine();
    ImGui::Text("%llu entries", static_cast<unsigned long long>(UiTrace::GetEntryCount()));
    DrawProblem("UI trace error", m_UiTraceError);

    if (ImGui::Button(std::format("Trace next {} frames", c_FramesPerTrace).c_str())) {
        m_FrameMirror.TraceFrames(c_FramesPerTrace);
    }

    Tooltip("Writes one line per frame to MirrorMod.log.");
}

void MirrorMod::DrawAbout() {
    // The Appropriate Legal Notices of the GPL. NOTICE requires derived works to keep the attribution here.
    ImGui::Text("MirrorMod %s by %s", MIRRORMOD_VERSION, c_Author);
    ImGui::Text("Copyright (C) 2026 %s", c_Author);
    ImGui::Spacing();
    DrawWrapped(
        "This program comes with ABSOLUTELY NO WARRANTY. It is free software, and you are welcome to redistribute it "
        "under the GNU General Public License version 3 or later and the attribution term in NOTICE. "
        "The license: https://www.gnu.org/licenses/gpl-3.0.html"
    );
}
