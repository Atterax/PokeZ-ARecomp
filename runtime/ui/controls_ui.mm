// Control remapping editor (runtime/input_bind.h), shared by the in-game menu's Controls tab (menu.mm) and the start
// screen's Controls page (launcher.mm): a table of Switch inputs with the controller button and the key bound to
// each. Select a binding, then press the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over). Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <stdio.h>

#include "imgui.h"

extern "C" {
#include "../input_bind.h"
#include "../settings.h"
void launcher_prompts_refresh(void);  // runtime/ui/launcher.mm
}
#define TR(en, pt) UI_TR(en, pt)  // (runtime/settings.h: the start screen's PT | EN)

static void cell(int dev, int slot, int fam, int cap_dev, int cap_slot, int ms_left) {
    char lbl[96];
    bool waiting = cap_dev == dev && cap_slot == slot;
    if (waiting) snprintf(lbl, sizeof lbl, "%s...  %d##%d_%d",
                          dev == BIND_DEV_PAD ? TR("Press a button", "Aperte um botão") : TR("Press a key", "Aperte uma tecla"),
                          (ms_left + 999) / 1000, dev, slot);
    else snprintf(lbl, sizeof lbl, "%s##%d_%d", dev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[slot], fam) :
                                                 bind_key_label(g_bind_key[slot]), dev, slot);
    // changed from the default: gold text; the same button / key on two inputs (a hand-edited larecomp.ini): red
    const int16_t *t = dev == BIND_DEV_PAD ? g_bind_pad : g_bind_key;
    int n = dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT, pushed = 0;
    bool dup = false;
    for (int i = 0; i < n; i++) dup |= i != slot && t[i] == t[slot] && t[slot] > (dev == BIND_DEV_PAD ? -1 : 0);
    if (waiting) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.12f, 1)); pushed++; }
    else if (dup) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.12f, 1)); pushed++; }
    if (!bind_is_default_slot(dev, slot)) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.82f, 0.38f, 1)); pushed++; }
    if (ImGui::Button(lbl, ImVec2(-FLT_MIN, 0))) {
        if (waiting) bind_capture_cancel();
        else bind_capture_begin(dev, slot);
    }
    ImGui::PopStyleColor(pushed);
    if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
}

// The mouse: how it turns the camera, how fast, and the Switch button each of its buttons presses (runtime/hle/input.c)
static void mouse_draw() {
    ImGui::SeparatorText(TR("Mouse", "Mouse"));
    const char *const modes[] = {TR("Off", "Desligado"), TR("Hold the right button", "Segurando o botão direito"),
                                 TR("Always (the window holds the cursor)", "Sempre (a janela prende o cursor)")};
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    changed |= ImGui::Combo(TR("Camera###cam", "Câmera###cam"), &g_settings.mouse_camera, modes, 3);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    ImGui::SliderFloat(TR("Camera speed###camspeed", "Velocidade da câmera###camspeed"), &g_settings.mouse_sens, 0.1f, 5.0f,
                       "%.2f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;  // (saved once let go, not on every step of a drag)
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", TR("Always: while you play, the cursor is hidden and moving the mouse turns the camera; F1 or Esc "
                                 "(the menu) or switching windows gives the cursor back. The buttons below work only then.",
                                 "Sempre: enquanto você joga, o cursor some e mexer o mouse gira a câmera; F1 ou Esc (o menu) "
                                 "ou trocar de janela devolve o cursor. Os botões abaixo só valem nesse modo."));
    ImGui::PopTextWrapPos();
    // the Switch buttons a mouse button can press: names and order as the table above
    static const int opts[] = {-1, BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS, BIND_MINUS,
                               BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT, BIND_LSTICK, BIND_RSTICK};
    if (ImGui::BeginTable("##mouse", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        for (int m = 0; m < MOUSEB_COUNT; m++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(bind_mouse_name(m));
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            char id[16];
            snprintf(id, sizeof id, "##mb%d", m);
            const char *none = TR("(none)", "(nenhum)");
            if (ImGui::BeginCombo(id, g_bind_mouse[m] >= 0 ? bind_input_name(g_bind_mouse[m]) : none)) {
                for (int o : opts)
                    if (ImGui::Selectable(o >= 0 ? bind_input_name(o) : none, g_bind_mouse[m] == o)) {
                        g_bind_mouse[m] = (int16_t)o;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(bind_mouse_is_default());
    if (ImGui::Button(TR("Reset mouse buttons to defaults###mreset", "Voltar os botões do mouse ao padrão###mreset"))) {
        bind_mouse_reset();
        changed = true;
    }
    ImGui::EndDisabled();
    if (changed) settings_save();
}

extern "C" void controls_bindings_draw(float scale) {
    (void)scale;
    int fam = bind_label_family();
    static const char *const fam_name[] = {"Nintendo", "Xbox", "PlayStation"};
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    const char *pad_name = ids && n > 0 ? SDL_GetGamepadNameForID(ids[0]) : NULL;
    char line[200];
    if (n > 0) snprintf(line, sizeof line, TR("Controller: %s  (%s button names)", "Controle: %s  (nomes de botões %s)"),
                        pad_name ? pad_name : TR("connected", "conectado"), fam_name[fam % 3]);
    else snprintf(line, sizeof line, TR("No controller connected  (%s button names shown)", "Nenhum controle conectado  (nomes de botões %s)"),
                  fam_name[fam % 3]);
    SDL_free(ids);
    ImGui::TextUnformatted(line);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", TR("Select a binding, then press the new button or key. Esc (or 5 seconds) cancels. A button "
                                 "that's already in use swaps with the old one.",
                                 "Escolha um comando e aperte o novo botão ou tecla. Esc (ou 5 segundos) cancela. Um botão "
                                 "que já está em uso troca de lugar com o antigo."));
    ImGui::PopTextWrapPos();

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    // a finished capture: say what happened, keep larecomp.ini current
    static int seen_serial = -1;
    static char msg[200];
    int rdev, rslot, rother, serial;
    int kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen_serial < 0) seen_serial = serial;
    if (serial != seen_serial) {
        seen_serial = serial;
        const char *now = rslot < 0 ? "" : rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rslot], fam) : bind_key_label(g_bind_key[rslot]);
        if (kind == 5)
            snprintf(msg, sizeof msg, "%s", rdev == BIND_DEV_PAD ? TR("Controller bindings reset to the defaults.", "Comandos do controle de volta ao padrão.")
                                                                 : TR("Keyboard bindings reset to the defaults.", "Comandos do teclado de volta ao padrão."));
        else if (kind == 1) snprintf(msg, sizeof msg, TR("%s is now %s.", "%s agora é %s."), bind_input_name(rslot), now);
        else if (kind == 2)
            snprintf(msg, sizeof msg, TR("%s is now %s; %s moved to %s (swapped).", "%s agora é %s; %s foi para %s (trocados)."),
                     bind_input_name(rslot), now, bind_input_name(rother),
                     rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rother], fam) : bind_key_label(g_bind_key[rother]));
        else if (kind == 3) snprintf(msg, sizeof msg, TR("Cancelled: %s unchanged.", "Cancelado: %s não mudou."), bind_input_name(rslot));
        else if (kind == 4) snprintf(msg, sizeof msg, TR("Nothing pressed: %s unchanged.", "Nada apertado: %s não mudou."), bind_input_name(rslot));
        if (kind == 1 || kind == 2 || kind == 5) {
            settings_save();
            launcher_prompts_refresh();  // (the game's button icons drawn again for the new bindings: runtime/ui/launcher.mm)
        }
    }
    if (msg[0]) ImGui::TextColored(ImVec4(0.95f, 0.71f, 0.27f, 1), "%s", msg);

    if (ImGui::BeginTable("##binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(TR("Controller", "Controle"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn(TR("Keyboard", "Teclado"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableHeadersRow();
        // in the order a player thinks of them: face buttons, shoulders, Plus / Minus, d-pad, sticks
        static const int order[] = {BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS,
                                    BIND_MINUS, BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT, BIND_LSTICK, BIND_RSTICK,
                                    BIND_LS_UP, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT, BIND_RS_UP, BIND_RS_DOWN,
                                    BIND_RS_LEFT, BIND_RS_RIGHT};
        for (int slot : order) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(bind_input_name(slot));
            ImGui::TableNextColumn();
            if (slot < BIND_PAD_COUNT) cell(BIND_DEV_PAD, slot, fam, cd, cs, ms);
            else ImGui::TextDisabled("%s", slot < BIND_RS_UP ? TR("Left stick", "Analógico esquerdo") : TR("Right stick", "Analógico direito"));
            ImGui::TableNextColumn();
            cell(BIND_DEV_KEY, slot, fam, cd, cs, ms);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    mouse_draw();
    ImGui::Spacing();
    ImGui::BeginDisabled(bind_is_default(BIND_DEV_PAD));
    if (ImGui::Button(TR("Reset controller to defaults###preset", "Voltar o controle ao padrão###preset"))) {
        bind_capture_cancel();
        bind_reset(BIND_DEV_PAD);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(bind_is_default(BIND_DEV_KEY));
    if (ImGui::Button(TR("Reset keyboard to defaults###kreset", "Voltar o teclado ao padrão###kreset"))) {
        bind_capture_cancel();
        bind_reset(BIND_DEV_KEY);
    }
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", TR("Bindings apply to the game only. Menus keep the fixed layout (right face button / Enter "
                                 "select, bottom face button / Esc back), and the in-game menu always opens with F1 or Esc, or "
                                 "the controller's Back + Start held together, or its Home / Guide button - so this page can't be "
                                 "locked out. The game's button icons follow these bindings: after a change they are drawn "
                                 "again in the background and show from the next start.",
                                 "Os comandos valem só para o jogo. Os menus mantêm o layout fixo (botão da direita / Enter "
                                 "escolhe, botão de baixo / Esc volta), e o menu do jogo sempre abre com F1 ou Esc, com Back + "
                                 "Start do controle juntos, ou com o botão Home / Guide - então esta página nunca fica "
                                 "inacessível. Os ícones de botões do jogo seguem estes comandos: depois de uma mudança eles "
                                 "são desenhados de novo em segundo plano e aparecem a partir do próximo início."));
    ImGui::PopTextWrapPos();
}
