// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. Compiled into DSperate (GPLv3) by
// patches/0003-minui-integration.patch.
//
// The pause menu, dressed and laid out as MinUI's minarch draws its own:
// Continue, Save, Load, Options and Quit over the dimmed game, a slot picker
// with the launcher's thumbnail of each slot, the game's name in a pill, the
// battery, and the button hints along the bottom. Options holds every page
// DSperate's own menu has, drawn as minarch draws its option lists.
//
// It replaces ds::sdl::Menu in main.cpp by type and nothing else: the public
// API is Menu's, method for method, and the types are Menu's own
// (Menu::Result, Menu::GameEntry, Menu::kAutoSlot), so the frontend's switch
// over results and its seven draw calls compile unchanged. Everything it
// knows about settings, bindings, cheats and achievements comes through the
// interfaces upstream already keeps menu.cpp behind -- SettingsHost,
// CheevosHost, the Setting tables and cheat::Code -- and never through
// Menu's internals, so an upstream refactor of menu.cpp cannot break it
// silently. See TECHNICAL.md.
//
// Two things are left to an embedded upstream Menu: the game picker a
// cart-less firmware boot raises (a pak always boots a ROM), and everything,
// when no MINUI_DIR names a launcher, which keeps a desktop build exactly
// upstream's.
#pragma once
#include "frontend/sdl/menu.h"
#include "minui_gfx.h"

#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ds::sdl {

class MinUiMenu {
public:
  using Result = Menu::Result;
  using GameEntry = Menu::GameEntry;
  static constexpr int kAutoSlot = Menu::kAutoSlot;
  // The slots the root page's picker offers: minarch's MENU_SLOT_COUNT, and
  // all the launcher's switcher ever lists (MinUi::MAX_SLOT + 1).
  static constexpr int kRootSlots = 8;
  // The slots that can hold a state at all: the slot hotkeys step 0..9.
  static constexpr int kSlots = 10;

  enum class Page : u8 { Root, Options, Settings, Controls, Cheats, CheevosAccount, Cheevos, TextEdit, Slots, Shortcuts };

  // How long a notice (notify()) stays, then how long it takes to fade out.
  static constexpr u32 kNoticeHoldMs = 3000, kNoticeFadeMs = 500;

  // Active when the launcher set MINUI_DIR, the same test MinUi::active()
  // makes. The look comes from the MINUI_* environment launch.sh exports.
  MinUiMenu();
  explicit MinUiMenu(bool active);
  MinUiMenu(const MinUiMenu&) = delete;
  MinUiMenu& operator=(const MinUiMenu&) = delete;

  // ---- ds::sdl::Menu's public API, as the frontend uses it ----------------
  bool open() const;
  void set_open(bool o);
  void set_cheats(std::vector<cheat::Code>* codes, const std::vector<cheat::Group>* groups);
  void set_settings_host(SettingsHost* host);
  void set_network_session(bool on);
  void set_cheevos_host(CheevosHost* host);
  bool cheats_dirty() const;
  void clear_cheats_dirty();
  void set_games(const std::vector<GameEntry>* games);
  void open_games();
  const std::string& chosen() const;
  int  slot() const;
  int  doomed_slot() const;
  void set_slot(int s);
  void set_slot_used(int s, bool used);
  void set_slot_notice(const char* n);
  void clear_slot_notice();
  Result update(u32 presses, u32 held, u32 ms);
  Result input(u32 presses) { return update(presses, 0, 0); }
  void face_presses(u32 faces);
  void draw(const Canvas& d) const;
  bool dirty() const;
  void clear_dirty();

  // ---- The pak's own --------------------------------------------------------
  // The pill at the top of the root page: the ROM's display name.
  void set_title(const std::string& title) { title_ = title; }
  // Where the launcher keeps a slot's thumbnail (MinUi::slot_thumbnail_path);
  // empty for a slot it never shows.
  void set_thumbnails(std::function<std::string(int)> path_of) { thumb_path_ = std::move(path_of); thumbs_.clear(); }
  // Replaces the look read from the environment, loading its font. Tests
  // use it; the frontend never needs to.
  void set_theme(const minui_gfx::Theme& theme);
  void set_battery_root(const std::string& root) { battery_root_ = root; }
  // The device profile the Shortcuts page restores its defaults from, and
  // the buttons the exported pad mapping names. launch.sh exports both
  // (MINUI_PROFILE_INI, MINUI_PAD_BUTTONS); tests set them here.
  void set_profile(const std::string& path) { profile_ = path; }
  void set_pad_buttons(const std::vector<std::string>& names) { pad_buttons_ = names; }
  // The device's own names for buttons it labels differently from SDL, as
  // "name=LABEL" words: the Brick's "leftstick=F1 rightstick=F2". launch.sh
  // exports them as MINUI_BUTTON_LABELS.
  void set_button_labels(const std::string& pairs);
  bool active() const { return active_; }
  // A [padhotkeys] value as the Shortcuts page shows it: "MENU+R1", "L2",
  // "MENU" for the modifier's tap, NONE. Needs the settings host.
  std::string binding_label(const std::string& value) const;
  // A Controls page pad value, which upstream spells with a position pip
  // and compass point ("\x02 EAST"), FUNC BTN and MOD, the way the Shortcuts
  // page spells it: "A", "MENU+B".
  std::string controls_label(const std::string& value) const;
  // The host's Controls rows the page shows, by the host's index.
  std::vector<int> control_rows() const;

  // A pill at the top of the game naming what a hotkey just changed, held
  // for kNoticeHoldMs and faded over kNoticeFadeMs. `now_ms` is wall time
  // (SDL_GetTicks), so fast forward does not cut it short. Nothing when no
  // launcher is driving the menu, which keeps a desktop build upstream's.
  void notify(const std::string& text, u32 now_ms);
  bool notice_on(u32 now_ms) const;
  // What it covered, for the tiers that must repaint it once it is gone.
  Rect draw_notice(const Canvas& d, u32 now_ms) const;
  // "LAYOUT: DOMINANT V": the row's label and the value's own label, from
  // the settings tables the Layout page shows, or the raw value.
  static std::string setting_notice(const char* key, const std::string& value);
  // A COLOUR or PATTERN value measured too slow on this platform: launch.sh
  // exports config/platform.sh's list as MINUI_SLOW_SHADERS, "colour:<name>"
  // and "pattern:<name>" separated by spaces. Still offered; the page says so.
  static bool slow_on_device(const char* key, const std::string& value);
  // Whether a screenshot should say so: NextUI's notifyScreenshot, passed on
  // by launch.sh as MINUI_NOTIFY_SCREENSHOT. On unless it is "0".
  bool notify_screenshots() const { return active_ && notify_screenshot_; }
  void set_notify_screenshots(bool on) { notify_screenshot_ = on; }
  // The game's name as NextUI shows it and names its screenshots: the alias
  // map.txt beside the ROM gives it, else the filename without its
  // extensions or anything from the first ( or [ on. Empty for no ROM.
  static std::string display_name(const std::string& rom_path);
  // Where the player is, for tests.
  Page page() const { return stack_[depth_ - 1]; }

private:
  // A row of a list page. Headings and notes are shown but never selected.
  struct Row {
    std::string label, value;
    enum Kind : u8 { Item, Heading, Note } kind = Item;
    bool enabled = true;
  };
  enum class Opt : u8 { Emulation, VisualFx, Layout, Controls, DsOptions, Cheats, Achievements, Slots, SaveChanges, Reset, Shortcuts };
  enum class RootItem : u8 { Continue, Save, Load, Options, Quit };
  enum class AccountRow : u8 { SignIn, SignOut, List, Toasts, Screenshot, Encore };
  enum class EditDest : u8 { Setting, CheevosUser, CheevosPassword };
  // One line of the cheats page, as upstream lays it out.
  struct Line { enum Kind : u8 { Heading, Note, Toggle } kind; int at; };

  bool forwarding() const { return !active_ || games_; }

  // Navigation.
  void push(Page p);
  bool pop();
  std::vector<RootItem> root_items() const;
  std::vector<Opt> options() const;
  std::vector<AccountRow> account_rows() const;
  const Setting* table() const;
  Result handle(u32 presses);
  Result handle_root(u32 presses);
  Result handle_options(u32 presses);
  Result handle_settings(u32 presses);
  Result handle_controls(u32 presses);
  Result handle_cheats(u32 presses);
  Result handle_account(u32 presses);
  Result handle_cheevos(u32 presses);
  Result handle_text_edit(u32 presses);
  Result handle_slots(u32 presses);
  Result handle_shortcuts(u32 presses);
  void open_option(Opt o);
  // The Shortcuts page: row 0 is the modifier, then one row per entry of
  // minui_shortcuts::kShortcuts.
  std::string modifier() const;
  std::vector<std::string> modifier_choices() const;
  std::string button_label(const std::string& name) const;
  std::string shortcut_value(int i) const;
  void step_modifier(int dir);
  void clear_shortcut(int i);
  void restore_shortcuts();
  void step_setting(int dir);
  void build_lines();
  void move_cheat_row(int delta);
  void toggle_cheat();
  void open_text_edit();
  void open_credential_edit(EditDest dest);
  // Up/down on the current list page, wrapping as minarch's lists do. Used
  // by the buttons and the held-direction repeat alike.
  void move_list(int dir);
  bool list_page() const;

  // Drawing.
  void draw_root(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  void draw_list(minui_gfx::Painter& p, const minui_gfx::Metrics& m, const std::string& title,
                 const std::vector<Row>& rows, int sel, int& top, const std::vector<std::string>& desc,
                 int value_w = 0) const;
  void draw_text_edit(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  void draw_options(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  void draw_settings(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  void draw_controls(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  void draw_cheats(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  void draw_account(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  void draw_cheevos(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  void draw_slots(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  void draw_shortcuts(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  // The pill at the top left, no wider than `max_w`. Returns where it ends.
  int  draw_title(minui_gfx::Painter& p, const minui_gfx::Metrics& m, const std::string& text, int max_w) const;
  // The battery pill at the top right. Returns its width, 0 with no battery.
  int  draw_hardware(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  // One of minarch's bottom pills: (button, hint) pairs.
  void draw_buttons(minui_gfx::Painter& p, const minui_gfx::Metrics& m,
                    const std::vector<std::pair<std::string, std::string>>& pairs, bool right) const;
  void draw_preview(minui_gfx::Painter& p, const minui_gfx::Metrics& m) const;
  // Wrapped onto at most `lines` lines of `max_w`, on spaces.
  std::vector<std::string> wrap(int px, const std::string& text, int max_w, int lines) const;

  bool active_ = false;
  Menu fallback_;
  bool games_ = false;   // the picker is up, on fallback_

  bool open_ = false;
  bool dirty_ = false;
  static constexpr int kMaxDepth = 6;
  Page stack_[kMaxDepth] = {Page::Root};
  int  depth_ = 1;

  int  row_ = 0;                 // the root page's selection
  int  slot_ = 0;
  bool used_[kAutoSlot + 1] = {};
  std::string notice_;           // a refused state, on the slot that refused it
  int  notice_slot_ = -1;
  bool close_next_ = false;      // Save leaves the menu, a tick after main has saved

  int  opt_row_ = 0;
  int  set_table_ = 0;           // which Setting table the settings page shows
  int  set_row_[5] = {};
  mutable int set_top_[5] = {};
  // Table 4 is the pak's Shaders (Advanced) page, opened from a row Visual FX
  // carries after SHADER PRESET. The rows of the page shown, that link
  // included: null for the link.
  static constexpr int kShaderTable = 4;
  int  settings_rows() const;
  const Setting* setting_row(int i) const;
  int  link_row() const;          // the link's row on this page, or -1
  mutable int opt_top_ = 0;

  SettingsHost* host_ = nullptr;
  bool bind_pad_ = false;
  int  bind_row_ = 0;
  mutable int bind_top_ = 0;
  bool reset_armed_ = false;
  bool listen_shown_ = false;
  u32  faces_ = 0;

  std::vector<cheat::Code>* codes_ = nullptr;
  const std::vector<cheat::Group>* groups_ = nullptr;
  std::vector<Line> lines_;
  int  cheat_row_ = 0;
  mutable int cheat_top_ = 0;
  bool cheats_dirty_ = false;

  CheevosHost* cheevos_ = nullptr;
  int  account_row_ = 0;
  mutable int account_top_ = 0;
  int  cheevos_row_ = 0;
  mutable int cheevos_top_ = 0;

  bool net_session_ = false;

  std::string edit_key_, edit_label_, edit_buf_;
  int  edit_pos_ = 0, edit_table_ = 0, edit_max_ = 0;
  EditDest edit_dest_ = EditDest::Setting;
  std::string pending_user_;   // between the two sign-in prompts; the password is never kept

  int  slots_row_ = 0;         // 0..kSlots-1, then kAutoSlot
  mutable int slots_top_ = 0;
  bool slots_armed_ = false;
  int  doomed_ = 0;

  int  short_row_ = 0;         // 0 the modifier, then kShortcuts
  mutable int short_top_ = 0;
  bool short_armed_ = false;   // Defaults, waiting for its second press
  bool short_refused_ = false; // the last way into the menu was nearly turned off
  std::string profile_;
  std::vector<std::string> pad_buttons_;
  std::map<std::string, std::string> button_labels_;

  std::string toast_;          // the notice, and when it went up
  u32  toast_ms_ = 0;
  bool notify_screenshot_ = true;

  static constexpr u32 kRepeatDelayMs = 400, kRepeatRateMs = 55;
  int  repeat_dir_ = 0;
  u32  repeat_ms_ = 0;
  bool repeating_ = false;
  // Rows the last draw fitted on a list page; L/R page by it.
  mutable int visible_ = 7;

  std::string title_;
  std::function<std::string(int)> thumb_path_;
  minui_gfx::Theme theme_;
  std::unique_ptr<minui_gfx::Font> font_;
  std::string battery_root_ = "/sys/class/power_supply";

  // The preview's picture of each slot, kept until the file changes.
  struct Thumb { std::string path; std::time_t mtime = 0; long size = -1; int w = 0, h = 0; minui_gfx::Image img; };
  mutable std::map<int, Thumb> thumbs_;
  const minui_gfx::Image* thumbnail(int slot, int w, int h) const;
};

} // namespace ds::sdl
