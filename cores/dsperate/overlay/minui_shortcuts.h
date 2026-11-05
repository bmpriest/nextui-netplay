// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. Compiled into DSperate (GPLv3) by
// patches/0003-minui-integration.patch.
//
// The pak's modifier chords, which the pause menu's Shortcuts page binds the
// way NextUI's does: A and then a button, alone or with the modifier held,
// and X for none. Which are bound out of the box is the device profile's
// business (config/dsperate/*.ini), not this table's: `chord` is only what
// Defaults falls back to when there is no profile to read.
//
// One entry per line, in this exact shape: tests/makefile.bats reads the
// chords out of this file to check every profile against them.
#pragma once

namespace ds::sdl::minui_shortcuts {

struct Shortcut {
  const char* key;        // under [padhotkeys]
  const char* label;      // the Shortcuts page's row
  const char* chord;      // the pak's binding, when no profile says otherwise
  const char* upstream;   // what an absent key means: upstream's default (src/frontend/sdl/input.cpp)
};

inline constexpr Shortcut kShortcuts[] = {
  {"pause.alt", "Pause menu (alt)", "mod", "none"},
  {"pause", "Pause menu", "mod+y", "mod+start"},
  {"quit", "Quit", "mod+a", "mod+start+back"},
  {"layout_next", "Next layout", "mod+dpup", "mod+back"},
  {"layout_prev", "Previous layout", "mod+dpdown", "mod+x"},
  {"screen_swap", "Swap screens", "mod++lefttrigger", "mod+y"},
  {"pip_corner_next", "Move PiP screen", "mod+start", "none"},
  {"screenshot", "Screenshot", "mod+b", "none"},
  {"fps", "FPS counter", "mod+x", "none"},
  {"fast_forward", "Fast forward (hold)", "+righttrigger", "mod++righttrigger"},
  {"fast_forward_toggle", "Fast forward (toggle)", "mod++righttrigger", "none"},
  {"game_switcher", "Game switcher", "mod+back", "none"},
  {"mic", "Blow into the mic (hold)", "mod+leftstick", "leftstick"},
  {"lid", "Close the lid", "mod+rightstick", "none"},
  {"save_state", "Save state", "mod+rightshoulder", "mod+rightshoulder"},
  {"load_state", "Load state", "mod+leftshoulder", "mod+leftshoulder"},
  {"slot_next", "Next state slot", "mod+dpright", "mod+dpright"},
  {"slot_prev", "Previous state slot", "mod+dpleft", "mod+dpleft"},
  {"colour_next", "Next colour", "none", "none"},
  {"colour_prev", "Previous colour", "none", "none"},
  {"pattern_next", "Next pattern", "none", "none"},
  {"pattern_prev", "Previous pattern", "none", "none"},
  {"preset_next", "Next shader preset", "none", "none"},
  {"preset_prev", "Previous shader preset", "none", "none"},
};
inline constexpr int kShortcutCount = static_cast<int>(sizeof kShortcuts / sizeof kShortcuts[0]);

// The buttons that may be the modifier, in the order the page steps through
// them, and only those the exported pad mapping names (MINUI_PAD_BUTTONS).
// SELECT and START are DS buttons as well: as the modifier, the game sees
// them only as a tap on release, and not at all while a tap opens the menu.
struct Modifier {
  const char* name;       // SDL's, as [padhotkeys] modifier takes it
  bool ds_button;
};

inline constexpr Modifier kModifiers[] = {
  {"guide", false},
  {"back", true},
  {"start", true},
  {"leftstick", false},
  {"rightstick", false},
  {"misc1", false},
  {"paddle1", false},
  {"paddle2", false},
  {"paddle3", false},
  {"paddle4", false},
};
inline constexpr int kModifierCount = static_cast<int>(sizeof kModifiers / sizeof kModifiers[0]);

// How the page names what a binding holds: SDL's names, as on the device.
// A trigger is bound as an axis, "+lefttrigger". DSperate reads a, b, x and y
// as the south, east, west and north buttons, and every supported pad has
// Nintendo's layout, where those are B, A, Y and X.
struct ButtonLabel {
  const char* name;
  const char* label;
};

inline constexpr ButtonLabel kButtonLabels[] = {
  {"a", "B"}, {"b", "A"}, {"x", "Y"}, {"y", "X"},
  {"back", "SELECT"}, {"guide", "MENU"}, {"start", "START"},
  {"leftstick", "L3"}, {"rightstick", "R3"},
  {"leftshoulder", "L1"}, {"rightshoulder", "R1"},
  {"+lefttrigger", "L2"}, {"+righttrigger", "R2"},
  {"dpup", "UP"}, {"dpdown", "DOWN"}, {"dpleft", "LEFT"}, {"dpright", "RIGHT"},
  {"-leftx", "LS LEFT"}, {"+leftx", "LS RIGHT"}, {"-lefty", "LS UP"}, {"+lefty", "LS DOWN"},
  {"-rightx", "RS LEFT"}, {"+rightx", "RS RIGHT"}, {"-righty", "RS UP"}, {"+righty", "RS DOWN"},
  {"misc1", "MISC"}, {"paddle1", "PADDLE 1"}, {"paddle2", "PADDLE 2"},
  {"paddle3", "PADDLE 3"}, {"paddle4", "PADDLE 4"}, {"touchpad", "TOUCHPAD"},
};

// Upstream's default modifier on a pad (padhotkeys.modifier).
inline constexpr const char* kDefaultModifier = "guide";

} // namespace ds::sdl::minui_shortcuts
