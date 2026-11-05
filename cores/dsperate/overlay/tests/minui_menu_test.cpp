// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak, copied into DSperate's tests/ by
// the Makefile and registered by patches/0003-minui-integration.patch.
//
// The MinUI-style pause menu: that it stands in for ds::sdl::Menu wherever
// the frontend uses one, that every page does what upstream's does with the
// same hosts, that drawing never leaves the canvas, and that the pieces it
// draws with -- blending, thumbnails, the battery, the theme -- read what the
// launcher actually writes.
#include "frontend/sdl/minui_menu.h"
#include "frontend/sdl/minui_gfx.h"
#include "frontend/sdl/minui_pad.h"
#include "frontend/sdl/minui_shortcuts.h"
#include "frontend/sdl/menu.h"
#include "frontend/sdl/settings.h"
#include "core/cart/miniz/miniz_tdef.h"
#include "core/cheat/database.h"
#include "core/io/io.h"
#include "check.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

using ds::u32;
using ds::u8;
using ds::sdl::Menu;
using ds::sdl::MinUiMenu;
using B = ds::io::Io::Button;
using Page = MinUiMenu::Page;
namespace gfx = ds::sdl::minui_gfx;

namespace {

u32 press(B b) { return 1u << b; }
void draw_everywhere(MinUiMenu& m, const char* what);

struct FakeHost : ds::sdl::SettingsHost {
  std::map<std::string, std::string> kv;
  std::map<std::string, bool> off;
  std::vector<std::string> writes;
  bool per_game = false, game = true, pad = false, capturing_ = false;
  std::string pending;
  int commits = 0;
  int rows = 4;
  std::vector<std::string> pad_keys;   // the pad column's keys, when not pad.kN

  std::string get(const char* key) const override {
    const auto it = kv.find(key);
    return it == kv.end() ? "" : it->second;
  }
  void set(const char* key, const std::string& v) override { kv[key] = v; writes.push_back(key); }
  bool enabled(const ds::sdl::Setting& s) const override {
    const auto it = off.find(s.key);
    return it == off.end() || !it->second;
  }
  const char* disabled_reason(const ds::sdl::Setting& s) const override { return enabled(s) ? "" : "NO"; }
  bool value_allowed(const ds::sdl::Setting&, const char*) const override { return true; }
  void commit() override { ++commits; }
  bool save_per_game() const override { return per_game && game; }
  void set_save_per_game(bool on) override { per_game = on && game; }
  bool has_game() const override { return game; }
  int binding_count(bool) const override { return rows; }
  Binding binding(bool p, int i) const override {
    Binding b;
    b.key = p && static_cast<size_t>(i) < pad_keys.size() ? pad_keys[static_cast<size_t>(i)]
                                                          : (p ? "pad.k" : "keys.k") + std::to_string(i);
    b.label = "ROW" + std::to_string(i);
    const auto it = kv.find(b.key);
    b.value = it == kv.end() ? "NONE" : it->second;
    return b;
  }
  bool has_pad() const override { return pad; }
  void begin_capture(bool) override { capturing_ = true; }
  void cancel_capture() override { capturing_ = false; }
  bool capturing() const override { return capturing_; }
  std::string take_capture() override {
    if (pending.empty()) return "";
    std::string out;
    out.swap(pending);
    capturing_ = false;
    return out;
  }
  void bind(const std::string& k, const std::string& v) override { kv[k] = v; writes.push_back(k); }
  void reset_bindings(bool) override { writes.push_back("reset"); }
  std::vector<std::string> collisions() const override { return {}; }
  const char* user_settings_note() const override { return nullptr; }
};

struct FakeCheevos final : ds::sdl::CheevosHost {
  bool in = false;
  std::vector<Row> rows;
  int signed_out_calls = 0;
  std::vector<std::pair<std::string, std::string>> sign_ins;
  std::map<int, bool> opts;

  std::string status() const override { return in ? "SIGNED IN AS TESTER" : "NOT SIGNED IN"; }
  std::string progress() const override { return rows.empty() ? "" : "1/2 EARNED  5/15 POINTS"; }
  bool signed_in() const override { return in; }
  bool has_set() const override { return !rows.empty(); }
  int row_count() const override { return static_cast<int>(rows.size()); }
  Row row(int i) const override { return rows.at(static_cast<size_t>(i)); }
  void sign_in(const std::string& u, const std::string& p) override { sign_ins.push_back({u, p}); }
  void sign_out() override { ++signed_out_calls; in = false; }
  bool option(Option o) const override {
    const auto it = opts.find(static_cast<int>(o));
    return it != opts.end() && it->second;
  }
  void set_option(Option o, bool on) override { opts[static_cast<int>(o)] = on; }
};

// A note, an ordinary group and an exclusive one, as upstream's test uses.
struct Cheats {
  std::vector<ds::cheat::Code> codes;
  std::vector<ds::cheat::Group> groups;
  Cheats() {
    groups.push_back({"Misc", "", false});
    groups.push_back({"Difficulty", "", true});
    auto add = [&](const char* name, int group, bool note) {
      ds::cheat::Code c;
      c.name = name;
      c.group = group;
      if (!note) c.words = {0x02000000, 1};
      codes.push_back(c);
    };
    add("(M) note", 0, true);
    add("Infinite Lives", 0, false);
    add("Infinite Coins", 0, false);
    add("Easy", 1, false);
    add("Normal", 1, false);
    add("Hard", 1, false);
  }
};

// Options rows with a settings host, cheats and achievements, in order.
enum OptRow { kEmulation, kVisualFx, kLayout, kControls, kDsOptions, kCheatsRow, kAchievements, kSlots, kSaveChanges, kReset };

// To the row'th Options entry from a freshly opened menu with a settings
// host: Options is the fourth root row (Continue, Save, Load, Options, Quit).
void to_options(MinUiMenu& m, int row) {
  m.set_open(true);
  for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::Options);
  for (int i = 0; i < row; ++i) m.input(press(B::BTN_DOWN));
}

std::string scratch(const std::string& name) {
  const char* t = std::getenv("TMPDIR");
  std::string dir = std::string(t && *t ? t : "/tmp") + "/minui_menu_test." + std::to_string(::getpid());
  ::mkdir(dir.c_str(), 0755);
  return dir + "/" + name;
}

void write_file(const std::string& path, const void* data, size_t n) {
  FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f != nullptr);
  CHECK(std::fwrite(data, 1, n, f) == n);
  std::fclose(f);
}

void write_text(const std::string& path, const std::string& s) { write_file(path, s.data(), s.size()); }

// ---------------------------------------------------------------------------

// Every call the frontend makes on its menu, in the forms it makes them.
// Instantiated for both menus: the swap in main.cpp is by type alone, so a
// signature that drifted from upstream's would break the shipping build, and
// this keeps the two side by side where a test can see them.
template <class M>
void exercise(M& m, FakeHost& h, FakeCheevos& c, Cheats& k, std::vector<Menu::GameEntry>& games) {
  m.set_cheats(&k.codes, &k.groups);
  m.set_games(&games);
  m.set_settings_host(&h);
  m.set_cheevos_host(&c);
  m.set_network_session(false);
  for (int i = 0; i <= Menu::kAutoSlot; ++i) m.set_slot_used(i, false);
  m.set_slot_notice(nullptr);
  m.set_slot(0);
  m.set_open(true);
  CHECK(m.open());
  m.face_presses(0);
  const Menu::Result r = m.update(0, 0, 16);
  CHECK(r == Menu::Result::None);
  (void)m.slot();
  (void)m.doomed_slot();
  (void)m.chosen();
  if (m.cheats_dirty()) m.clear_cheats_dirty();
  std::vector<u32> px(64 * 48);
  const ds::sdl::Canvas cv{px.data(), 64, 64, 48};
  m.draw(cv);
  if (m.dirty()) m.clear_dirty();
  m.set_open(false);
  CHECK(!m.open());
}

void test_api_parity() {
  FakeHost h;
  FakeCheevos c;
  Cheats k;
  std::vector<Menu::GameEntry> games{{"A GAME", "/a.nds"}};
  Menu upstream;
  exercise(upstream, h, c, k, games);
  MinUiMenu ours(true);
  exercise(ours, h, c, k, games);
  static_assert(std::is_same<MinUiMenu::Result, Menu::Result>::value, "main.cpp switches over Menu::Result");
  static_assert(MinUiMenu::kAutoSlot == Menu::kAutoSlot, "the auto state's slot number is upstream's");
}

// minarch's five, in its order.
void test_root_rows() {
  FakeHost h;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  m.set_open(true);
  CHECK(m.page() == Page::Root);
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Resume);           // Continue
  m.set_open(true);
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Save);             // Save
  m.set_open(true);
  for (int i = 0; i < 2; ++i) m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);             // Load, on an empty slot
  m.set_slot_used(0, true);
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Load);
  m.set_open(true);
  for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);             // Options opens a page
  CHECK(m.page() == Page::Options);
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::None);             // and B comes back
  CHECK(m.page() == Page::Root);
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Quit);             // Quit
  // Up from the top wraps to Quit, and B anywhere on the root resumes.
  m.set_open(true);
  m.input(press(B::BTN_UP));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Quit);
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::Resume);
}

// Save leaves the menu as minarch's does: the frontend saves on Save, and the
// next tick, with nothing pressed, says Resume so it closes the usual way.
void test_save_then_resume() {
  MinUiMenu m(true);
  m.set_open(true);
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Save);
  CHECK(m.update(0, 0, 10) == Menu::Result::Resume);
  CHECK(m.update(0, 0, 10) == Menu::Result::None);   // once
  // Reopening forgets a pending close.
  m.input(press(B::BTN_A));
  m.set_open(true);
  CHECK(m.update(0, 0, 10) == Menu::Result::None);
}

// Left and right on Save or Load walk minarch's eight slots and wrap; the
// hidden 8 and 9 the hotkeys can reach open the picker on 0.
void test_slot_picker() {
  MinUiMenu m(true);
  m.set_slot(3);
  m.set_open(true);
  CHECK(m.slot() == 3);
  m.input(press(B::BTN_LEFT));                                       // not on Save or Load
  CHECK(m.slot() == 3);
  m.input(press(B::BTN_DOWN));                                       // Save
  m.input(press(B::BTN_RIGHT));
  CHECK(m.slot() == 4);
  for (int i = 0; i < 4; ++i) m.input(press(B::BTN_RIGHT));
  CHECK(m.slot() == 0);                                              // 7 -> 0
  m.input(press(B::BTN_LEFT));
  CHECK(m.slot() == 7);
  m.input(press(B::BTN_DOWN));                                       // Load moves it the same
  m.input(press(B::BTN_RIGHT));
  CHECK(m.slot() == 0);
  for (int s : {8, 9}) {
    m.set_slot(s);
    m.set_open(true);
    CHECK(m.slot() == 0);
  }
}

// A state freezes this machine and not the peer: a network session has no
// Save, no Load, no slot page and no reset.
void test_network_session() {
  FakeHost h;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  m.set_network_session(true);
  m.set_open(true);
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);
  CHECK(m.page() == Page::Options);
  m.input(press(B::BTN_B));
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Quit);
  // Options: the five settings pages and the save switch, nothing else.
  m.set_open(true);
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
  for (int i = 0; i < 5; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_RIGHT));
  CHECK(h.per_game);                                                 // the sixth is Save Changes
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::Settings);                                 // ... and it wraps to Emulation
  // With nothing behind it at all, the Options row goes.
  MinUiMenu bare(true);
  bare.set_network_session(true);
  bare.set_open(true);
  bare.input(press(B::BTN_DOWN));
  CHECK(bare.input(press(B::BTN_A)) == Menu::Result::Quit);
}

// Every Options row reaches its page, and B returns to Options.
void test_options_pages() {
  FakeHost h;
  FakeCheevos c;
  Cheats k;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  m.set_cheevos_host(&c);
  m.set_cheats(&k.codes, &k.groups);
  const struct { int row; Page page; } kPages[] = {
    {kEmulation, Page::Settings}, {kVisualFx, Page::Settings}, {kLayout, Page::Settings},
    {kControls, Page::Controls}, {kDsOptions, Page::Settings}, {kCheatsRow, Page::Cheats},
    {kAchievements, Page::CheevosAccount}, {kSlots, Page::Slots},
  };
  for (const auto& p : kPages) {
    to_options(m, p.row);
    CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);
    CHECK(m.page() == p.page);
    m.input(press(B::BTN_B));
    CHECK(m.page() == Page::Options);
  }
  // Save Changes works in place, on left, right and A alike.
  to_options(m, kSaveChanges);
  m.input(press(B::BTN_A));
  CHECK(h.per_game);
  m.input(press(B::BTN_LEFT));
  CHECK(!h.per_game);
  CHECK(m.page() == Page::Options);
  to_options(m, kReset);
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Reset);
}

// Rows with nothing behind them are not there at all.
void test_options_hide_what_is_absent() {
  FakeHost h;
  h.game = false;                                                    // no per-game file: no switch
  MinUiMenu m(true);
  m.set_settings_host(&h);
  std::vector<ds::cheat::Code> none;
  std::vector<ds::cheat::Group> no_groups;
  m.set_cheats(&none, &no_groups);
  // Emulation .. DS Options, Manage Save Slots, Reset Game: seven rows.
  to_options(m, 6);
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Reset);
  to_options(m, 7);                                                  // wraps to Emulation
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::Settings);
  // Without a settings host, only the slot page and reset remain.
  MinUiMenu bare(true);
  to_options(bare, 0);
  bare.input(press(B::BTN_A));
  CHECK(bare.page() == Page::Slots);
  bare.input(press(B::BTN_B));
  bare.input(press(B::BTN_DOWN));
  CHECK(bare.input(press(B::BTN_A)) == Menu::Result::Reset);
}

// The Visual FX row after SHADER PRESET is the link to the Shaders page.
int shaders_link_row() {
  for (int i = 0; ds::sdl::kVideoSettings[i].key; ++i)
    if (!std::strcmp(ds::sdl::kVideoSettings[i].key, "video.preset")) return i + 1;
  return -1;
}

// Opens the Shaders page through Visual FX's link row.
void to_shaders(MinUiMenu& m) {
  to_options(m, kVisualFx);
  m.input(press(B::BTN_A));
  for (int i = 0; i < shaders_link_row(); ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
}

// Each settings page walks upstream's own table to its terminator, so a row
// upstream adds appears here without a change. Right steps the row under the
// cursor; down past the last wraps to the first, as minarch's lists do. The
// pak's Shaders page walks its own table the same way, and Visual FX's link
// to it is passed over.
void test_settings_walk_upstream_tables() {
  const ds::sdl::Setting* tables[] = {ds::sdl::kEmuSettings, ds::sdl::kVideoSettings, ds::sdl::kLayoutSettings, ds::sdl::kUserSettings, ds::sdl::kShaderSettings};
  const int rows[] = {kEmulation, kVisualFx, kLayout, kDsOptions, -1};
  for (int t = 0; t < 5; ++t) {
    FakeHost h;
    MinUiMenu m(true);
    m.set_settings_host(&h);
    if (rows[t] < 0) to_shaders(m);
    else {
      to_options(m, rows[t]);
      m.input(press(B::BTN_A));
    }
    CHECK(m.page() == Page::Settings);
    const int n = ds::sdl::settings_count(tables[t]);
    CHECK(n > 0);
    for (int i = 0; i < n; ++i) {
      if (tables[t] == ds::sdl::kVideoSettings && i == shaders_link_row()) m.input(press(B::BTN_DOWN));
      const ds::sdl::Setting& s = tables[t][i];
      if (s.type != ds::sdl::Setting::Type::Text) {
        const size_t before = h.writes.size();
        const std::string cur = h.get(s.key);
        m.input(press(B::BTN_RIGHT));
        // A row whose next value is the one it has writes nothing, as upstream.
        if (ds::sdl::step_value(s, cur, +1, h) != cur) {
          CHECK(h.writes.size() == before + 1);
          CHECK(h.writes.back() == s.key);
        }
      }
      m.input(press(B::BTN_DOWN));
    }
    // Back on the first row.
    const ds::sdl::Setting& first = tables[t][0];
    if (first.type != ds::sdl::Setting::Type::Text) {
      const std::string cur = h.get(first.key);
      m.input(press(B::BTN_LEFT));
      if (ds::sdl::step_value(first, cur, -1, h) != cur) CHECK(h.writes.back() == first.key);
    }
  }
}

// A row the host switched off can be selected, so its reason shows, but it
// never steps -- upstream's rule.
void test_disabled_setting_does_not_step() {
  FakeHost h;
  h.off[ds::sdl::kEmuSettings[0].key] = true;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_options(m, kEmulation);
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_RIGHT));
  m.input(press(B::BTN_A));
  CHECK(h.writes.empty());
}

// Layout-affecting changes are applied once, when the menu closes.
void test_commit_on_closing() {
  FakeHost h;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_options(m, kEmulation);
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_RIGHT));
  m.input(press(B::BTN_B));
  m.input(press(B::BTN_B));
  CHECK(h.commits == 0);
  m.set_open(false);
  CHECK(h.commits == 1);
  m.set_open(false);
  CHECK(h.commits == 1);
}

// The Controls page binds what the device reports, to the row that asked.
void test_controls() {
  FakeHost h;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_options(m, kControls);
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::Controls);
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));                                          // listen
  CHECK(h.capturing_);
  m.update(0, 0, 10);                                                // nothing yet
  CHECK(h.writes.empty());
  h.pending = "f5";
  m.update(0, 0, 10);
  CHECK(h.kv["keys.k1"] == "f5");
  m.input(press(B::BTN_Y));                                          // clear
  CHECK(h.kv["keys.k1"] == "none");
  m.input(press(B::BTN_X));                                          // defaults, armed
  CHECK(h.writes.back() != "reset");
  m.input(press(B::BTN_X));                                          // and confirmed
  CHECK(h.writes.back() == "reset");
  m.input(press(B::BTN_X));
  m.input(press(B::BTN_DOWN));                                       // any other press disarms
  m.input(press(B::BTN_X));
  CHECK(std::count(h.writes.begin(), h.writes.end(), "reset") == 1);
  // L and R swap columns.
  m.input(press(B::BTN_B));                                          // backs out of the armed reset
  m.input(press(B::BTN_R));
  m.input(press(B::BTN_A));
  h.pending = "a";
  m.update(0, 0, 10);
  CHECK(h.kv["pad.k0"] == "a");
  // A shrinking list never leaves the cursor past its end.
  for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
  h.rows = 2;
  m.input(0);
  m.input(press(B::BTN_Y));
  CHECK(h.kv["pad.k1"] == "none");
}

// With a pad, Clear and Defaults are the west and north buttons by position.
void test_controls_by_position_on_a_pad() {
  FakeHost h;
  h.pad = true;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_options(m, kControls);
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_Y));                                          // DS Y is not Clear here
  CHECK(h.writes.empty());
  m.face_presses(1u << 2);                                           // west
  m.input(0);
  CHECK(h.kv["pad.k0"] == "none");
  m.face_presses(1u << 3);                                           // north, armed
  m.input(0);
  m.face_presses(1u << 3);
  m.input(0);
  CHECK(std::count(h.writes.begin(), h.writes.end(), "reset") == 1);
  // Upstream's Defaults put its chords back; the Shortcuts page's follow.
  CHECK(h.kv["padhotkeys.modifier"] == "guide");
  CHECK(h.kv["padhotkeys.pip_corner_next"] == "mod+start");
}

// With a pad, the Shortcuts page owns the modifier and its chords, so the
// Controls page's pad column leaves them out, and its rows still bind the
// right keys.
void test_controls_hides_shortcuts() {
  FakeHost h;
  h.pad = true;
  h.pad_keys = {"pad.a", "padhotkeys.modifier", "padhotkeys.quit", "padhotkeys.pause.alt",
                "padhotkeys.fast_forward", "padhotkeys.mute", "padhotkeys.fullscreen",
                "padhotkeys.volume_down.alt", "padhotkeys.lid.alt", "padhotkeys.quit.alt",
                "padhotkeys.screen_swap.alt", "padhotkeys.fullscreen.alt", "padhotkeys.colour_next",
                "padhotkeys.pattern_prev.alt", "pad.b"};
  h.rows = static_cast<int>(h.pad_keys.size());
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_options(m, kControls);
  m.input(press(B::BTN_A));                                          // opens on the pad column
  m.input(press(B::BTN_DOWN));
  m.face_presses(1u << 2);                                           // clear
  m.input(0);
  CHECK(h.kv["pad.b"] == "none");
  m.input(press(B::BTN_DOWN));                                       // wraps to the top
  m.face_presses(1u << 2);
  m.input(0);
  CHECK(h.kv["pad.a"] == "none");
  CHECK(!h.kv.count("padhotkeys.quit"));
  CHECK(!h.kv.count("padhotkeys.modifier"));
  CHECK(!h.kv.count("padhotkeys.fast_forward"));
  CHECK(!h.kv.count("padhotkeys.mute"));
  CHECK(!h.kv.count("padhotkeys.volume_down.alt"));
  CHECK(!h.kv.count("padhotkeys.lid.alt"));
  CHECK(!h.kv.count("padhotkeys.quit.alt"));
  CHECK(!h.kv.count("padhotkeys.screen_swap.alt"));
  CHECK(!h.kv.count("padhotkeys.colour_next"));
  CHECK(!h.kv.count("padhotkeys.pattern_prev.alt"));
  draw_everywhere(m, "the controls page without shortcuts");
  // The keyboard column keeps its chords.
  m.input(press(B::BTN_L));
  m.input(press(B::BTN_DOWN));
  m.face_presses(1u << 2);
  m.input(0);
  CHECK(h.kv["keys.k1"] == "none");
}

// Only codes are selectable, A toggles, and an exclusive group keeps one on.
void test_cheats() {
  FakeHost h;
  Cheats k;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  m.set_cheats(&k.codes, &k.groups);
  to_options(m, kCheatsRow);
  m.input(press(B::BTN_A));
  CHECK(!m.cheats_dirty());
  m.input(press(B::BTN_A));                                          // the first code, past heading and note
  CHECK(k.codes[1].enabled);
  CHECK(!k.codes[0].enabled);
  CHECK(m.cheats_dirty());
  m.clear_cheats_dirty();
  CHECK(!m.cheats_dirty());
  for (int i = 0; i < 2; ++i) m.input(press(B::BTN_DOWN));           // Easy, over the heading
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));                                          // Normal
  CHECK(!k.codes[3].enabled);
  CHECK(k.codes[4].enabled);
  for (int i = 0; i < 10; ++i) m.input(press(B::BTN_DOWN));          // clamps at the end
  m.input(press(B::BTN_A));
  CHECK(k.codes[5].enabled);
  CHECK(!k.codes[4].enabled);
}

// Signing in is two prompts, and the password goes straight to the host.
void test_achievements() {
  FakeHost h;
  FakeCheevos c;
  Cheats k;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  m.set_cheevos_host(&c);
  m.set_cheats(&k.codes, &k.groups);
  to_options(m, kAchievements);
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::CheevosAccount);
  m.input(press(B::BTN_A));                                          // Sign In
  CHECK(m.page() == Page::TextEdit);
  m.input(press(B::BTN_UP));
  m.input(press(B::BTN_RIGHT));                                      // a skipped slot ...
  m.input(press(B::BTN_RIGHT));
  m.input(press(B::BTN_UP));
  m.input(press(B::BTN_A));                                          // the username
  CHECK(m.page() == Page::TextEdit);                                 // straight on to the password
  m.input(press(B::BTN_UP));
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::CheevosAccount);
  CHECK(c.sign_ins.size() == 1);
  CHECK(c.sign_ins[0].first == "AA");                                // ... which is dropped
  CHECK(c.sign_ins[0].second == "A");
  // Cancelling sends nothing.
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_UP));
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_B));
  CHECK(c.sign_ins.size() == 1);
  // The switches toggle in place.
  m.input(press(B::BTN_DOWN));                                       // Unlock Notices
  m.input(press(B::BTN_RIGHT));
  CHECK(c.option(ds::sdl::CheevosHost::Option::Toasts));
  // With a set loaded the list is a row away, and signed in offers sign out.
  c.in = true;
  c.rows.push_back({"First", "Do it", 5, true, false});
  c.rows.push_back({"Second", "Do more", 10, false, false});
  m.input(press(B::BTN_UP));
  m.input(press(B::BTN_DOWN));                                       // View Achievements
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::Cheevos);
  m.input(press(B::BTN_B));
  m.input(press(B::BTN_UP));
  m.input(press(B::BTN_A));                                          // Sign Out
  CHECK(c.signed_out_calls == 1);
}

// The name editor edits within the field's length and writes only on A.
void test_text_editor() {
  FakeHost h;
  h.kv["user.nickname"] = "AB";
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_options(m, kDsOptions);
  m.input(press(B::BTN_A));
  CHECK(std::strcmp(ds::sdl::kUserSettings[0].key, "user.nickname") == 0);
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::TextEdit);
  CHECK(h.writes.empty());
  m.input(press(B::BTN_UP));
  m.input(press(B::BTN_B));
  CHECK(h.kv["user.nickname"] == "AB");
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_UP));
  m.input(press(B::BTN_A));
  CHECK(h.kv["user.nickname"] == "BB");
  m.input(press(B::BTN_A));
  for (int i = 0; i < 40; ++i) m.input(press(B::BTN_RIGHT));
  for (int i = 0; i < 40; ++i) m.input(press(B::BTN_UP));
  for (int i = 0; i < 8; ++i) m.input(press(B::BTN_L));
  m.input(press(B::BTN_A));
  CHECK(h.kv["user.nickname"].size() <= static_cast<size_t>(ds::sdl::kUserSettings[0].lo));
}

// Every slot that can hold a state, 8 and 9 and the auto state included,
// deletes on a second A and on nothing else.
void test_manage_slots() {
  MinUiMenu m(true);
  for (int s : {1, 9, MinUiMenu::kAutoSlot}) m.set_slot_used(s, true);
  to_options(m, 0);                                                  // no host: the slot page is first
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::Slots);
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);             // slot 0 is empty
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);             // armed
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::None);             // disarmed, still here
  CHECK(m.page() == Page::Slots);
  m.input(press(B::BTN_A));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Delete);
  CHECK(m.doomed_slot() == 1);
  for (int i = 0; i < 8; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Delete);
  CHECK(m.doomed_slot() == 9);
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Delete);
  CHECK(m.doomed_slot() == MinUiMenu::kAutoSlot);
  m.input(press(B::BTN_DOWN));                                       // wraps to slot 0
  m.input(press(B::BTN_A));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::None);
  CHECK(m.page() == Page::Options);
}

// Holding a direction repeats at upstream's rate on list pages only.
void test_key_repeat() {
  FakeHost h;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_options(m, kEmulation);
  m.input(press(B::BTN_A));
  const u32 down = press(B::BTN_DOWN);
  const ds::sdl::Setting* t = ds::sdl::kEmuSettings;
  // Which row is under the cursor, read back by stepping it while still held.
  const auto row_is = [&](int i) {
    const std::string cur = h.get(t[i].key);
    if (t[i].type == ds::sdl::Setting::Type::Text || ds::sdl::step_value(t[i], cur, +1, h) == cur) return true;
    m.update(press(B::BTN_RIGHT), down, 0);
    return !h.writes.empty() && h.writes.back() == t[i].key;
  };
  m.update(0, down, 0);                                              // the hold starts
  m.update(0, down, 399);                                            // still inside the delay
  CHECK(row_is(0));
  m.update(0, down, 1);                                              // the delay is over: one step
  CHECK(row_is(1));
  m.update(0, down, 54);
  CHECK(row_is(1));
  m.update(0, down, 1);                                              // then one per 55 ms
  CHECK(row_is(2));
  m.update(0, down, 110);
  CHECK(row_is(4));
  // On the root, holding does nothing.
  m.set_open(true);
  m.update(0, down, 0);
  m.update(0, down, 5000);
  CHECK(m.update(press(B::BTN_A), down, 0) == Menu::Result::Resume);
}

// The firmware's game picker is upstream's page, reached through the same calls.
void test_game_picker_is_upstreams() {
  std::vector<Menu::GameEntry> games{{"ONE", "/one.nds"}, {"TWO", "/two.nds"}};
  MinUiMenu m(true);
  m.set_games(&games);
  m.open_games();
  CHECK(m.open());
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Launch);
  CHECK(m.chosen() == "/two.nds");
  m.set_open(false);
  CHECK(!m.open());
  // And the pause menu afterwards is ours again.
  m.set_open(true);
  CHECK(m.page() == Page::Root);
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Resume);
}

// Without a launcher it is upstream's menu, row for row.
void test_inactive_is_upstream() {
  MinUiMenu m(false);
  CHECK(!m.active());
  m.set_open(true);
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Save);             // upstream's first row is SAVE STATE
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Load);
  CHECK(m.update(0, 0, 10) == Menu::Result::None);                   // no pending close
}

// ---------------------------------------------------------------------------
// Drawing

// The canvas sits inside a guard band; nothing may write to it.
struct Guarded {
  static constexpr u32 kGuard = 0x5A5A5A5A;
  static constexpr int kBand = 16;
  int w, h, pitch;
  std::vector<u32> px;
  Guarded(int w_, int h_) : w(w_), h(h_), pitch(w_ + 2 * kBand), px(static_cast<size_t>(pitch) * (h_ + 2 * kBand), kGuard) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) at(x, y) = 0;
  }
  u32& at(int x, int y) { return px[static_cast<size_t>(y + kBand) * pitch + static_cast<size_t>(x + kBand)]; }
  ds::sdl::Canvas canvas() { return ds::sdl::Canvas{&at(0, 0), static_cast<u32>(pitch), w, h}; }
  bool guard_intact() const {
    for (int y = 0; y < h + 2 * kBand; ++y)
      for (int x = 0; x < pitch; ++x) {
        const bool inside = x >= kBand && x < kBand + w && y >= kBand && y < kBand + h;
        if (!inside && px[static_cast<size_t>(y) * pitch + static_cast<size_t>(x)] != kGuard) return false;
      }
    return true;
  }
  bool drew() {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (at(x, y) != 0) return true;
    return false;
  }
};

void draw_everywhere(MinUiMenu& m, const char* what) {
  const int sizes[][2] = {{256, 192}, {640, 480}, {720, 720}, {1024, 768}, {1280, 720}};
  for (const auto& s : sizes) {
    Guarded g(s[0], s[1]);
    m.draw(g.canvas());
    if (!g.guard_intact()) std::fprintf(stderr, "%s drew outside a %dx%d canvas\n", what, s[0], s[1]);
    CHECK(g.guard_intact());
    CHECK(g.drew());
  }
}

void test_draw_bounds() {
  FakeHost h;
  FakeCheevos c;
  c.rows.push_back({"An achievement with a name long enough to need cutting short", "And a description that wraps", 5, true, false});
  Cheats k;
  for (int i = 0; i < 60; ++i) {   // enough to scroll on every panel
    ds::cheat::Code code;
    code.name = "Cheat number " + std::to_string(i) + " with a long, long name";
    code.group = 1;
    code.words = {0x02000000, 1};
    k.codes.push_back(code);
  }
  MinUiMenu m(true);
  m.set_theme(gfx::Theme{});                                         // no font: the bitmap fallback
  m.set_battery_root(scratch("no-battery"));
  m.set_title("A Game With A Very Long Title That Will Not Fit In The Pill At All (USA)");
  m.set_settings_host(&h);
  m.set_cheevos_host(&c);
  m.set_cheats(&k.codes, &k.groups);
  m.set_slot_used(0, true);
  m.set_open(true);
  draw_everywhere(m, "root, Continue");
  m.input(press(B::BTN_DOWN));
  draw_everywhere(m, "root, Save");                                  // with the preview window
  m.set_slot_notice("REJECTED");
  m.input(press(B::BTN_DOWN));
  draw_everywhere(m, "root, Load, refused");
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
  draw_everywhere(m, "options");
  for (int row : {kEmulation, kVisualFx, kLayout, kControls, kDsOptions, kCheatsRow, kAchievements, kSlots}) {
    to_options(m, row);
    m.input(press(B::BTN_A));
    draw_everywhere(m, "a sub-page");
    for (int i = 0; i < 40; ++i) m.input(press(B::BTN_DOWN));       // and scrolled
    draw_everywhere(m, "a scrolled sub-page");
  }
  to_options(m, kAchievements);
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_A));
  draw_everywhere(m, "the name editor");
  // The settings page with the canvas too small for even one row of text
  // must still stay inside it.
  to_options(m, kEmulation);
  m.input(press(B::BTN_A));
  Guarded tiny(40, 30);
  m.draw(tiny.canvas());
  CHECK(tiny.guard_intact());
}

// A list page's title has the whole band up to the battery, as the root's
// does, so the longer page names are not cut. They were once kept to half the
// screen, clear of the centred scroll arrow, and "RetroAchievements" and
// "Manage Save Slots" both lost their ends.
void test_list_titles_are_not_cut() {
  FakeHost h;
  FakeCheevos c;
  Cheats k;
  MinUiMenu m(true);
  gfx::Theme t;
  t.nextui = true;                                                   // a title pill that shows on black
  m.set_theme(t);
  m.set_battery_root(scratch("no-battery"));
  m.set_settings_host(&h);
  m.set_cheevos_host(&c);
  m.set_cheats(&k.codes, &k.groups);
  const u32 accent = t.title_pill();
  for (int row : {kAchievements, kSlots}) {
    to_options(m, row);
    m.input(press(B::BTN_A));
    Guarded g(640, 480);
    m.draw(g.canvas());
    // 17 glyphs of the 5x7 font at scale 4 run past x=380; half the screen
    // stopped the pill at 272. y=22 is the pill's edge, clear of the text.
    CHECK(g.at(380, 22) == accent);
    CHECK(g.guard_intact());
  }
}

// The battery pill appears only when there is a battery to show.
void test_battery_pill() {
  const std::string root = scratch("power_supply");
  ::mkdir(root.c_str(), 0755);
  MinUiMenu m(true);
  m.set_theme(gfx::Theme{});
  m.set_battery_root(root);
  m.set_open(true);
  Guarded none(640, 480);
  m.draw(none.canvas());
  const u32 corner_none = none.at(640 - 30, 30);                     // centre of where the pill goes
  ::mkdir((root + "/axp2202-battery").c_str(), 0755);
  write_text(root + "/axp2202-battery/type", "Battery\n");
  write_text(root + "/axp2202-battery/capacity", "50\n");
  write_text(root + "/axp2202-battery/status", "Discharging\n");
  Guarded with(640, 480);
  m.draw(with.canvas());
  CHECK(corner_none == 0);
  CHECK(with.at(640 - 50, 50) != 0);                                 // the pill itself
  CHECK(with.guard_intact());
}

// A thumbnail written the way write_png writes one -- PNG bytes under the
// launcher's .bmp name -- shows in the preview window.
void test_preview_shows_the_thumbnail() {
  const int w = 64, h = 48;
  std::vector<u8> rgba(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < rgba.size(); i += 4) { rgba[i] = 0x20; rgba[i + 1] = 0xC0; rgba[i + 2] = 0x40; rgba[i + 3] = 0xFF; }
  size_t len = 0;
  void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), w, h, 4, &len, 6, MZ_FALSE);
  CHECK(png != nullptr);
  const std::string path = scratch("Game.nds.0.bmp");
  write_file(path, png, len);
  std::free(png);

  MinUiMenu m(true);
  m.set_theme(gfx::Theme{});
  m.set_battery_root(scratch("no-battery"));
  m.set_thumbnails([&](int slot) { return slot == 0 ? path : std::string(); });
  m.set_slot_used(0, true);
  m.set_open(true);
  m.input(press(B::BTN_DOWN));                                       // Save: the preview is up
  Guarded g(640, 480);
  m.draw(g.canvas());
  // The window is the right half: 320x240 of picture centred vertically.
  const u32 c = g.at(640 - 10 - 4 - 160, 240 - 8);
  CHECK(c == 0xFF20C040);
  // An empty slot shows no picture, even with a file left behind.
  m.set_slot_used(0, false);
  Guarded e(640, 480);
  m.draw(e.canvas());
  CHECK(e.at(640 - 10 - 4 - 160, 240 - 8) != 0xFF20C040);
}

// ---------------------------------------------------------------------------
// The Shortcuts page

namespace sc = ds::sdl::minui_shortcuts;

// Options rows with a pad: Shortcuts follows Controls.
constexpr int kShortcutsRow = kControls + 1;

int shortcut_row(const char* key) {
  for (int i = 0; i < sc::kShortcutCount; ++i)
    if (std::strcmp(sc::kShortcuts[i].key, key) == 0) return i + 1;   // row 0 is the modifier
  CHECK(false);
  return 0;
}

// Onto the Shortcuts page, then down to `key`'s row (nullptr: the modifier).
void to_shortcut(MinUiMenu& m, const char* key) {
  to_options(m, kShortcutsRow);
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::Shortcuts);
  const int row = key ? shortcut_row(key) : 0;
  for (int i = 0; i < row; ++i) m.input(press(B::BTN_DOWN));
}

std::string pad_hot(FakeHost& h, const char* key) { return h.kv[std::string("padhotkeys.") + key]; }

// Only with a pad: every chord is a pad chord.
void test_shortcuts_need_a_pad() {
  FakeHost h;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_options(m, kShortcutsRow);
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::Settings);                                 // DS Options moved up
  h.pad = true;
  to_options(m, kShortcutsRow);
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::Shortcuts);
}

// NextUI's binding: A listens, the next button is bound, alone or with the
// modifier held, and the cursor moves on. X clears and moves on too.
void test_shortcuts_bind() {
  FakeHost h;
  h.pad = true;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_shortcut(m, "save_state");
  m.input(press(B::BTN_A));
  CHECK(h.capturing_);
  draw_everywhere(m, "the shortcuts page, listening");
  m.input(0);                                                        // nothing pressed yet
  CHECK(h.capturing_);
  h.pending = "mod+x";
  m.input(0);
  CHECK(pad_hot(h, "save_state") == "mod+x");
  CHECK(!h.capturing_);
  m.input(press(B::BTN_A));                                          // on to Load state
  h.pending = "+righttrigger";
  m.input(0);
  CHECK(pad_hot(h, "load_state") == "+righttrigger");
  // Clear is the north button by position, and moves on as well.
  to_shortcut(m, "screenshot");
  m.face_presses(1u << 3);
  m.input(0);
  CHECK(pad_hot(h, "screenshot") == "none");
  m.face_presses(1u << 3);
  m.input(0);
  CHECK(pad_hot(h, "fps") == "none");
  // Left and right leave a shortcut alone; they are the modifier row's.
  const size_t writes = h.writes.size();
  to_shortcut(m, "quit");
  m.input(press(B::BTN_LEFT));
  m.input(press(B::BTN_RIGHT));
  CHECK(h.writes.size() == writes);
  draw_everywhere(m, "the shortcuts page");
  for (int i = 0; i < 20; ++i) m.input(press(B::BTN_DOWN));
  draw_everywhere(m, "the shortcuts page, scrolled");
}

// How a binding reads: the modifier by its label, a trigger's axis as the
// trigger, anything unknown in capitals.
void test_shortcuts_labels() {
  FakeHost h;
  h.pad = true;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  CHECK(m.binding_label("mod+rightshoulder") == "MENU+R1");
  CHECK(m.binding_label("mod++lefttrigger") == "MENU+L2");
  CHECK(m.binding_label("+righttrigger") == "R2");
  CHECK(m.binding_label("mod+-leftx") == "MENU+LS LEFT");
  CHECK(m.binding_label("mod") == "MENU");
  CHECK(m.binding_label("mod+start+back") == "MENU+START+SELECT");
  CHECK(m.binding_label("dpup") == "UP");
  CHECK(m.binding_label("none") == "NONE");
  CHECK(m.binding_label("") == "NONE");
  CHECK(m.binding_label("mod+misc2") == "MENU+MISC2");
  // Face buttons by position, named as on a Nintendo-layout pad.
  CHECK(m.binding_label("a") == "B");
  CHECK(m.binding_label("b") == "A");
  CHECK(m.binding_label("x") == "Y");
  CHECK(m.binding_label("y") == "X");
  h.kv["padhotkeys.modifier"] = "back";
  CHECK(m.binding_label("mod+y") == "SELECT+X");
  // The Brick's F1 and F2 are SDL's sticks, and go by the device's names.
  m.set_button_labels("leftstick=F1 rightstick=F2 bad =X Y=");
  CHECK(m.binding_label("mod+leftstick") == "SELECT+F1");
  CHECK(m.binding_label("rightstick") == "F2");
  CHECK(m.binding_label("bad") == "BAD");
  h.kv["padhotkeys.modifier"] = "rightstick";
  CHECK(m.binding_label("mod+a") == "F2+B");
  m.set_button_labels("");
  CHECK(m.binding_label("mod+a") == "R3+B");
}

// The Controls page's pad values, which upstream spells with a position pip
// our font cannot draw, read as the Shortcuts page's do.
void test_controls_labels() {
  FakeHost h;
  h.pad = true;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  CHECK(m.controls_label("\x02 EAST") == "A");
  CHECK(m.controls_label("\x01 SOUTH") == "B");
  CHECK(m.controls_label("\x03 WEST") == "Y");
  CHECK(m.controls_label("\x04 NORTH") == "X");
  CHECK(m.controls_label("FUNC BTN") == "MENU");
  CHECK(m.controls_label("MOD+\x02 EAST") == "MENU+A");
  CHECK(m.controls_label("MOD++LEFTX") == "MENU++LEFTX");
  CHECK(m.controls_label("MOD+START+SELECT") == "MENU+START+SELECT");
  CHECK(m.controls_label("LEFT STICK") == "LEFT STICK");
  CHECK(m.controls_label("NONE") == "NONE");
  h.kv["padhotkeys.modifier"] = "back";
  m.set_button_labels("leftstick=F1");
  CHECK(m.controls_label("MOD+L3") == "SELECT+F1");
}

// The pause chord and its alternate cannot both be cleared: nothing else
// opens the menu.
void test_shortcuts_keep_a_way_in() {
  FakeHost h;
  h.pad = true;
  h.kv["padhotkeys.pause"] = "mod+y";
  h.kv["padhotkeys.pause.alt"] = "mod";
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_shortcut(m, "pause.alt");
  m.face_presses(1u << 3);
  m.input(0);
  CHECK(pad_hot(h, "pause.alt") == "none");                         // the chord is still bound
  to_shortcut(m, "pause");
  m.face_presses(1u << 3);
  m.input(0);
  CHECK(pad_hot(h, "pause") == "mod+y");                            // refused
  draw_everywhere(m, "the shortcuts page, refusing");
  to_shortcut(m, "pause.alt");
  m.input(press(B::BTN_A));
  h.pending = "mod";
  m.input(0);
  CHECK(pad_hot(h, "pause.alt") == "mod");
  to_shortcut(m, "pause");
  m.face_presses(1u << 3);
  m.input(0);
  CHECK(pad_hot(h, "pause") == "none");                             // the tap is back, so it may go
}

// The modifier steps through what the pad mapping names.
void test_shortcuts_modifier() {
  FakeHost h;
  h.pad = true;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  m.set_pad_buttons({"a", "b", "back", "guide", "start", "dpup"});
  to_shortcut(m, nullptr);
  m.input(press(B::BTN_RIGHT));
  CHECK(pad_hot(h, "modifier") == "back");
  draw_everywhere(m, "the shortcuts page, SELECT as the modifier");
  m.input(press(B::BTN_RIGHT));
  CHECK(pad_hot(h, "modifier") == "start");
  m.input(press(B::BTN_RIGHT));                                      // wraps: L3 and the rest are not mapped
  CHECK(pad_hot(h, "modifier") == "guide");
  m.input(press(B::BTN_LEFT));
  CHECK(pad_hot(h, "modifier") == "start");
  m.input(press(B::BTN_A));                                          // A steps it too, never listens
  CHECK(pad_hot(h, "modifier") == "guide");
  CHECK(!h.capturing_);
  // With no mapping known, the modifier stays where it is.
  FakeHost bare;
  bare.pad = true;
  MinUiMenu n(true);
  n.set_settings_host(&bare);
  n.set_pad_buttons({});
  to_shortcut(n, nullptr);
  n.input(press(B::BTN_RIGHT));
  CHECK(bare.writes.empty());
}

// Y twice restores the device's profile; without one, every chord.
void test_shortcuts_defaults() {
  const std::string profile = scratch("no-sticks.ini");
  write_text(profile, "[pad]\nstylus_dpad = +righttrigger\n\n[padhotkeys]\n# a comment\nquit = mod+a\n"
                      "pause = mod+y\npause.alt = mod\nsave_state = none\nslot_next = none\n\n[cheevos]\nquit = x\n");
  FakeHost h;
  h.pad = true;
  h.kv["padhotkeys.modifier"] = "back";
  h.kv["padhotkeys.save_state"] = "mod+x";
  MinUiMenu m(true);
  m.set_settings_host(&h);
  m.set_profile(profile);
  to_shortcut(m, "quit");
  m.face_presses(1u << 2);                                           // west, armed
  m.input(0);
  CHECK(pad_hot(h, "save_state") == "mod+x");
  draw_everywhere(m, "the shortcuts page, Defaults armed");
  m.face_presses(1u << 2);
  m.input(0);
  CHECK(pad_hot(h, "modifier") == "guide");
  CHECK(pad_hot(h, "save_state") == "none");
  CHECK(pad_hot(h, "slot_next") == "none");
  CHECK(pad_hot(h, "quit") == "mod+a");                              // [cheevos] is not [padhotkeys]
  CHECK(pad_hot(h, "load_state") == "mod+leftshoulder");             // the profile leaves it out
  // Any other press backs out of an armed Defaults, and does only that.
  h.kv["padhotkeys.quit"] = "none";
  m.face_presses(1u << 2);
  m.input(0);
  m.face_presses(1u << 3);
  m.input(0);
  m.face_presses(1u << 2);
  m.input(0);
  CHECK(pad_hot(h, "quit") == "none");
  // No profile: every chord.
  FakeHost g;
  g.pad = true;
  MinUiMenu n(true);
  n.set_settings_host(&g);
  n.set_profile(scratch("missing.ini"));
  to_shortcut(n, nullptr);
  n.face_presses(1u << 2);
  n.input(0);
  n.face_presses(1u << 2);
  n.input(0);
  for (int i = 0; i < sc::kShortcutCount; ++i) CHECK(pad_hot(g, sc::kShortcuts[i].key) == sc::kShortcuts[i].chord);
}

// ---------------------------------------------------------------------------
// The pad's mapping, put together from SDL's GUID and the pak's bodies

namespace pad = ds::sdl::minui_pad;

void test_pad_mapping() {
  const std::string body = "a:b1,b:b0,guide:b8,lefttrigger:a2,dpup:h0.1";
  const std::string fixed = "a:b3,b:b4,guide:b11";
  const std::string fixed_guid = "19000000010000000100000000016e01";
  // The GUID in front, the name, the body, and the platform.
  pad::Mapping m = pad::choose("0300a3845e0400008e02000014010000", "Xbox 360 Controller", body, "", fixed_guid);
  CHECK(m.sdl == "0300a3845e0400008e02000014010000,Xbox 360 Controller," + body + ",platform:Linux");
  CHECK(m.buttons == "a b guide lefttrigger dpup");
  CHECK(!m.fixed);
  // NextUI's fixed h700 layout takes the other body, and only by its GUID.
  m = pad::choose(fixed_guid, "ANBERNIC-keys", body, fixed, fixed_guid);
  CHECK(m.fixed);
  CHECK(m.sdl == fixed_guid + ",ANBERNIC-keys," + fixed + ",platform:Linux");
  CHECK(m.buttons == "a b guide");
  m = pad::choose("19000000010000000100000000010000", "ANBERNIC-keys", body, fixed, fixed_guid);
  CHECK(!m.fixed);
  CHECK(m.buttons == "a b guide lefttrigger dpup");
  // Off h700 there is no fixed body, so the GUID changes nothing.
  m = pad::choose(fixed_guid, "Pad", body, "", fixed_guid);
  CHECK(!m.fixed);
  CHECK(m.buttons == "a b guide lefttrigger dpup");
  // A comma in the name would end its field; no name at all gets one.
  CHECK(pad::choose("00ff", "Pad, wired", "a:b1", "", "").sdl == "00ff,Pad wired,a:b1,platform:Linux");
  CHECK(pad::choose("00ff", "", "a:b1", "", "").sdl == "00ff,Controller,a:b1,platform:Linux");
  // No body or no GUID: nothing, rather than a mapping SDL would refuse.
  CHECK(pad::choose("00ff", "Pad", "", "", "").sdl.empty());
  CHECK(pad::choose("", "Pad", body, "", "").sdl.empty());
  CHECK(pad::choose("", "Pad", body, "", "").buttons.empty());
}

// ---------------------------------------------------------------------------
// The screenshot's name: NextUI's getDisplayName, then its map.txt alias

void test_display_name() {
  using M = MinUiMenu;
  CHECK(M::display_name("") == "");
  CHECK(M::display_name("/Roms/DS (DSP)/Mario Kart DS (USA, Australia) (En,Fr,De,Es,It).nds") == "Mario Kart DS");
  // Everything from the first ( on, not just the trailing groups.
  CHECK(M::display_name("/r/Game (USA) Extra.nds") == "Game");
  CHECK(M::display_name("/r/Game [!] (USA).nds") == "Game");
  // Stacked short extensions go; a long "extension" is part of the name.
  CHECK(M::display_name("/r/Beckon the Hellspawn.p8.png") == "Beckon the Hellspawn");
  CHECK(M::display_name("/r/Dr. Mario.nds") == "Dr. Mario");
  // A name that is all tags keeps itself rather than vanishing.
  CHECK(M::display_name("/r/(Prototype).nds") == "(Prototype)");
  CHECK(M::display_name("/r/Title  .nds") == "Title");
  // map.txt beside the ROM: tab separated, exact filename, CRLF tolerated.
  const std::string dir = scratch("Roms");
  ::mkdir(dir.c_str(), 0755);
  write_text(dir + "/map.txt", "Other.nds\tNot This\r\nPokemon - Black (USA).nds\tPokemon Black\r\nPokemon - Black (USA).nd\tNo\r\n");
  CHECK(M::display_name(dir + "/Pokemon - Black (USA).nds") == "Pokemon Black");
  CHECK(M::display_name(dir + "/Pokemon - White (USA).nds") == "Pokemon - White");
}

void test_screenshot_notice_switch() {
  ::unsetenv("MINUI_NOTIFY_SCREENSHOT");
  CHECK(MinUiMenu(true).notify_screenshots());
  ::setenv("MINUI_NOTIFY_SCREENSHOT", "0", 1);
  CHECK(!MinUiMenu(true).notify_screenshots());
  ::setenv("MINUI_NOTIFY_SCREENSHOT", "1", 1);
  CHECK(MinUiMenu(true).notify_screenshots());
  ::unsetenv("MINUI_NOTIFY_SCREENSHOT");
  CHECK(!MinUiMenu(false).notify_screenshots());                     // no launcher, no notice
}

// ---------------------------------------------------------------------------
// The notice

void test_notice_timing() {
  MinUiMenu m(true);
  m.set_theme(gfx::Theme{});
  CHECK(!m.notice_on(0));
  m.notify("LAYOUT: PIP", 1000);
  CHECK(m.notice_on(1000));
  CHECK(m.notice_on(1000 + MinUiMenu::kNoticeHoldMs + MinUiMenu::kNoticeFadeMs - 1));
  CHECK(!m.notice_on(1000 + MinUiMenu::kNoticeHoldMs + MinUiMenu::kNoticeFadeMs));
  // A desktop build has no launcher and no notice.
  MinUiMenu off(false);
  off.notify("LAYOUT: PIP", 1000);
  CHECK(!off.notice_on(1000));
  std::vector<u32> px(64 * 48, 0);
  CHECK(off.draw_notice(ds::sdl::Canvas{px.data(), 64, 64, 48}, 1000).w == 0);
}

// Opaque while it holds, half there half way through the fade, then gone;
// on an overlay cleared to 0 the fade is in the alpha, not blended to black.
void test_notice_fades() {
  MinUiMenu m(true);
  m.set_theme(gfx::Theme{});
  m.notify("LAYOUT: DOMINANT V", 0);
  const u32 hold = MinUiMenu::kNoticeHoldMs, fade = MinUiMenu::kNoticeFadeMs;
  Guarded a(640, 480);
  const ds::sdl::Rect r = m.draw_notice(a.canvas(), hold);
  CHECK(r.w > 0 && r.h > 0);
  CHECK(r.x + r.w / 2 >= 319 && r.x + r.w / 2 <= 321);               // centred
  const int cy = r.y + r.h / 2, cx = r.x + r.h / 4;                  // in the pill, left of the text
  CHECK((a.at(cx, cy) >> 24) == 0xFF);
  Guarded b(640, 480);
  m.draw_notice(b.canvas(), hold + fade / 2);
  const u32 half = b.at(cx, cy) >> 24;
  CHECK(half >= 0x70 && half <= 0x90);
  Guarded c(640, 480);
  CHECK(m.draw_notice(c.canvas(), hold + fade).w == 0);
  CHECK(!c.drew());
}

void test_notice_bounds() {
  MinUiMenu m(true);
  m.set_theme(gfx::Theme{});
  m.notify("A NOTICE FAR TOO LONG FOR ANY PANEL THIS PAK RUNS ON, LET ALONE THE DS SCREEN", 0);
  const int sizes[][2] = {{256, 192}, {640, 480}, {720, 720}, {1024, 768}, {1280, 720}};
  for (const auto& s : sizes) {
    for (u32 t : {0u, MinUiMenu::kNoticeHoldMs + MinUiMenu::kNoticeFadeMs / 3}) {
      Guarded g(s[0], s[1]);
      const ds::sdl::Rect r = m.draw_notice(g.canvas(), t);
      CHECK(g.guard_intact());
      CHECK(g.drew());
      CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= s[0] && r.y + r.h <= s[1]);
    }
  }
}

void test_setting_notice() {
  CHECK(MinUiMenu::setting_notice("video.layout", "dominant_v") == "LAYOUT: DOMINANT V");
  CHECK(MinUiMenu::setting_notice("video.screen", "bottom") == "MAIN SCREEN: BOTTOM");
  CHECK(MinUiMenu::setting_notice("video.pip_corner", "tr") == "PIP CORNER: TOP RIGHT");
  CHECK(MinUiMenu::setting_notice("video.nothing", "pip") == "pip");
}

// Visual FX has SHADER PRESET after BILINEAR, then the link to the Shaders
// page, where the pak's COLOUR, SCALING, FILTER, PATTERN, GLOW and CURVE rows
// step from their defaults and say when the value is one this device is too
// slow for, while still letting it be picked. B goes back to the link. The
// shortcuts that step them ship unbound.
void test_shader_rows() {
  const int link = shaders_link_row();
  CHECK(link > 0);
  CHECK(!std::strcmp(ds::sdl::kVideoSettings[link - 2].key, "video.linear"));
  for (int i = 0; ds::sdl::kVideoSettings[i].key; ++i)
    CHECK(std::strcmp(ds::sdl::kVideoSettings[i].key, "video.colour") != 0);
  const char* const order[] = {"video.colour", "video.scaling", "video.filter", "video.pattern", "video.glow"};
  for (int i = 0; i < 5; ++i) CHECK(!std::strcmp(ds::sdl::kShaderSettings[i].key, order[i]));
  FakeHost h;
  MinUiMenu m(true);
  m.set_settings_host(&h);
  to_options(m, kVisualFx);
  m.input(press(B::BTN_A));
  for (int i = 0; i < link - 1; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_RIGHT));
  CHECK(h.writes.back() == "video.preset");
  m.input(press(B::BTN_DOWN));
  draw_everywhere(m, "the shaders link");
  const size_t before = h.writes.size();
  m.input(press(B::BTN_LEFT));                                       // the link steps nothing
  CHECK(h.writes.size() == before);
  m.input(press(B::BTN_A));
  CHECK(m.page() == Page::Settings);
  m.input(press(B::BTN_RIGHT));
  CHECK(h.kv["video.colour"] == "nds");
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_RIGHT));
  CHECK(h.kv["video.scaling"] == "sharp_bilinear");
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_RIGHT));
  CHECK(h.kv["video.pattern"] == "lcd");
  draw_everywhere(m, "the shaders page with a pattern");
  // A switched-off PATTERN row is still drawn, and does not step.
  h.off["video.pattern"] = true;
  m.input(press(B::BTN_RIGHT));
  CHECK(h.kv["video.pattern"] == "lcd");
  draw_everywhere(m, "the shaders page with the pattern off");
  h.off.erase("video.pattern");
  // B goes back to Visual FX on the link, and RIGHT opens the page again at its top.
  m.input(press(B::BTN_B));
  CHECK(m.page() == Page::Settings);
  m.input(press(B::BTN_LEFT));
  CHECK(h.writes.back() == "video.pattern");                         // still the link: nothing written
  m.input(press(B::BTN_RIGHT));
  m.input(press(B::BTN_LEFT));
  CHECK(h.kv["video.colour"] == "off");

  unsetenv("MINUI_SLOW_SHADERS");
  CHECK(!MinUiMenu::slow_on_device("video.colour", "nds"));
  setenv("MINUI_SLOW_SHADERS", "colour:nds pattern:beads", 1);
  CHECK(MinUiMenu::slow_on_device("video.colour", "nds"));
  CHECK(MinUiMenu::slow_on_device("video.pattern", "beads"));
  CHECK(!MinUiMenu::slow_on_device("video.pattern", "nds"));
  CHECK(!MinUiMenu::slow_on_device("video.colour", "gba"));
  CHECK(!MinUiMenu::slow_on_device("emu.speed", "nds"));
  m.input(press(B::BTN_RIGHT));
  CHECK(h.kv["video.colour"] == "nds");   // offered all the same
  draw_everywhere(m, "a value too slow for the device");
  unsetenv("MINUI_SLOW_SHADERS");

  for (const char* key : {"colour_next", "colour_prev", "pattern_next", "pattern_prev", "preset_next", "preset_prev"}) {
    bool found = false;
    for (const auto& s : ds::sdl::minui_shortcuts::kShortcuts)
      if (!std::strcmp(s.key, key)) { found = true; CHECK(!std::strcmp(s.chord, "none") && !std::strcmp(s.upstream, "none")); }
    CHECK(found);
  }
  CHECK(MinUiMenu::setting_notice("video.colour", "ds_classic") == "COLOUR: DS CLASSIC");
  CHECK(MinUiMenu::setting_notice("video.pattern", "scanlines") == "PATTERN: SCANLINES");
  CHECK(MinUiMenu::setting_notice("video.glow", "halation") == "GLOW: HALATION");
  CHECK(MinUiMenu::setting_notice("video.curve", "barrel") == "CURVE: BARREL");
  // GLOW and its numbers step from auto.
  for (int i = 0; i < 4; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_RIGHT));
  CHECK(h.kv["video.glow"] == "off");
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_RIGHT));
  CHECK(h.kv["video.glow_strength"] == "0.05");
  draw_everywhere(m, "the glow rows");
}

// ---------------------------------------------------------------------------
// The pieces

// A painter's alpha scales fills, plots and text alike.
void test_painter_alpha() {
  std::vector<u32> px(8 * 8, 0);
  gfx::Painter p(ds::sdl::Canvas{px.data(), 8, 8, 8});
  p.set_alpha(128);
  p.fill(0, 0, 2, 2, gfx::kWhite);
  CHECK(px[0] == 0x80FFFFFF);
  p.plot(4, 4, gfx::kWhite);
  CHECK(px[4 * 8 + 4] == 0x80FFFFFF);
  p.set_alpha(0);
  p.fill(6, 6, 2, 2, gfx::kWhite);
  CHECK(px[6 * 8 + 6] == 0);
  // The bitmap font's fallback writes opaque pixels, so a faded line goes
  // through a scratch canvas; it must come out partial too.
  std::vector<u32> tx(100 * 40, 0);
  gfx::Painter q(ds::sdl::Canvas{tx.data(), 100, 100, 40});
  q.set_alpha(128);
  gfx::Font f;
  f.draw(q, 2, 2, 16, gfx::kWhite, "OK");
  CHECK(std::count(tx.begin(), tx.end(), 0x80FFFFFFu) > 0);
  CHECK(std::count(tx.begin(), tx.end(), gfx::kWhite) == 0);
}

void test_blend() {
  // Opaque wins outright, transparent leaves the destination alone.
  CHECK(gfx::blend(0xFF102030, 0xFFAABBCC) == 0xFFAABBCC);
  CHECK(gfx::blend(0xFF102030, 0x00AABBCC) == 0xFF102030);
  // Onto a transparent overlay the colour is kept and only the alpha is partial.
  CHECK(gfx::blend(0x00000000, 0x80FF0000) == 0x80FF0000);
  // Onto opaque black, half of white is mid grey and opaque.
  const u32 g = gfx::blend(0xFF000000, 0x80FFFFFF);
  CHECK((g >> 24) == 0xFF);
  CHECK(((g >> 16) & 0xFF) >= 0x7F && ((g >> 16) & 0xFF) <= 0x81);
  // Half over half: alpha is 1 - (1 - a)(1 - b).
  const u32 hh = gfx::blend(0x800000FF, 0x80FF0000);
  CHECK((hh >> 24) >= 0xBF && (hh >> 24) <= 0xC1);
}

void test_decode_png_and_bmp() {
  const u8 rgba[] = {0xFF, 0, 0, 0xFF, 0, 0xFF, 0, 0xFF};
  size_t len = 0;
  void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba, 2, 1, 4, &len, 6, MZ_FALSE);
  const gfx::Image a = gfx::decode_image(static_cast<const u8*>(png), len);
  std::free(png);
  CHECK(a.w == 2 && a.h == 1);
  CHECK(a.px[0] == 0xFFFF0000 && a.px[1] == 0xFF00FF00);
  // A 2x1 24-bit bottom-up BMP, which is what MinUI's SDL_SaveBMP writes.
  const u8 bmp[] = {
    'B', 'M', 62, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
    40, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 8, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0xFF, 0, 0, 0, 0, 0xFF, 0, 0,   // blue, red (BGR), padded to four bytes
  };
  const gfx::Image b = gfx::decode_image(bmp, sizeof bmp);
  CHECK(b.w == 2 && b.h == 1);
  CHECK(b.px[0] == 0xFF0000FF && b.px[1] == 0xFFFF0000);
  CHECK(gfx::decode_image(rgba, sizeof rgba).empty());               // not an image
  CHECK(gfx::load_image(scratch("missing.bmp")).empty());
  // Halving a 2x2 averages it.
  gfx::Image four;
  four.w = four.h = 2;
  four.px = {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000};
  const gfx::Image one = gfx::scale_image(four, 1, 1);
  CHECK(one.w == 1 && ((one.px[0] >> 16) & 0xFF) == 0x7F);
}

void test_battery() {
  const std::string root = scratch("supplies");
  ::mkdir(root.c_str(), 0755);
  CHECK(gfx::read_battery(root).percent == -1);
  ::mkdir((root + "/usb").c_str(), 0755);
  write_text(root + "/usb/type", "USB\n");
  write_text(root + "/usb/capacity", "99\n");
  CHECK(gfx::read_battery(root).percent == -1);                      // not a battery
  ::mkdir((root + "/battery").c_str(), 0755);
  write_text(root + "/battery/type", "Battery\n");
  write_text(root + "/battery/capacity", "42\n");
  write_text(root + "/battery/status", "Charging\n");
  const gfx::Battery b = gfx::read_battery(root);
  CHECK(b.percent == 42);
  CHECK(b.charging);
  write_text(root + "/battery/capacity", "140\n");
  write_text(root + "/battery/status", "Full\n");
  CHECK(gfx::read_battery(root).percent == 100);
  CHECK(!gfx::read_battery(root).charging);
  CHECK(gfx::read_battery(scratch("nowhere")).percent == -1);
}

void test_theme() {
  u32 c = 0;
  CHECK(gfx::parse_colour("0x9B2257FF", c) && c == 0xFF9B2257);     // NextUI's own, alpha dropped
  CHECK(gfx::parse_colour("0x123456", c) && c == 0xFF123456);
  CHECK(gfx::parse_colour("abcdef", c) && c == 0xFFABCDEF);
  CHECK(!gfx::parse_colour("", c));
  CHECK(!gfx::parse_colour("0x12345", c));
  CHECK(!gfx::parse_colour("0xGG0000", c));
  ::setenv("MINUI_STYLE", "nextui", 1);
  ::setenv("MINUI_FONT", "/nowhere/font1.ttf", 1);
  ::setenv("MINUI_COLOR2", "0x112233FF", 1);
  ::setenv("MINUI_COLOR5", "bogus", 1);
  const gfx::Theme t = gfx::Theme::from_env();
  CHECK(t.nextui);
  CHECK(t.font == "/nowhere/font1.ttf");
  CHECK(t.title_pill() == 0xFF112233);
  CHECK(t.selected_text() == 0xFF000000);                            // the default stands
  CHECK(!t.shadow());
  ::setenv("MINUI_STYLE", "minui", 1);
  const gfx::Theme u = gfx::Theme::from_env();
  CHECK(!u.nextui);
  CHECK(u.title_pill() == gfx::kBlack);                              // MinUI ignores the colours
  CHECK(u.shadow());
  ::unsetenv("MINUI_STYLE");
  ::unsetenv("MINUI_FONT");
  ::unsetenv("MINUI_COLOR2");
  ::unsetenv("MINUI_COLOR5");
}

// No font, a missing font, and a file that is not a font all fall back to the
// bitmap font rather than to nothing.
void test_font_fallback() {
  gfx::Font f;
  CHECK(!f.load(""));
  CHECK(!f.load(scratch("missing.ttf")));
  const std::string junk = scratch("junk.ttf");
  write_text(junk, "this is not a font");
  CHECK(!f.load(junk));
  CHECK(!f.loaded());
  CHECK(f.width(16, "SAVE") > 0);
  CHECK(f.line_height(16) >= 7);
  CHECK(f.fit(16, "A LONG LINE OF TEXT", f.width(16, "A LONG")) .size() < std::string("A LONG LINE OF TEXT").size());
  std::vector<u32> px(100 * 40, 0);
  gfx::Painter p(ds::sdl::Canvas{px.data(), 100, 100, 40});
  f.draw(p, 2, 2, 16, gfx::kWhite, "OK");
  CHECK(std::count(px.begin(), px.end(), gfx::kWhite) > 0);
}

// Pills and rounded windows stay inside their box and fill its middle.
void test_shapes() {
  Guarded g(40, 20);
  gfx::Painter p(g.canvas());
  p.pill(-10, 2, 70, 16, gfx::kWhite);                               // runs off both sides
  CHECK(g.guard_intact());
  CHECK(g.at(20, 10) == gfx::kWhite);
  Guarded r(30, 30);
  gfx::Painter q(r.canvas());
  q.rounded(5, 5, 20, 20, 6, gfx::kWhite);
  CHECK(r.at(15, 15) == gfx::kWhite);
  CHECK(r.at(5, 5) == 0);                                            // the corner is cut
  CHECK(r.at(4, 15) == 0);
  CHECK(r.guard_intact());
}

} // namespace

int main() {
  test_api_parity();
  test_root_rows();
  test_save_then_resume();
  test_slot_picker();
  test_network_session();
  test_options_pages();
  test_options_hide_what_is_absent();
  test_settings_walk_upstream_tables();
  test_disabled_setting_does_not_step();
  test_commit_on_closing();
  test_controls();
  test_controls_by_position_on_a_pad();
  test_controls_hides_shortcuts();
  test_cheats();
  test_achievements();
  test_text_editor();
  test_manage_slots();
  test_key_repeat();
  test_game_picker_is_upstreams();
  test_inactive_is_upstream();
  test_draw_bounds();
  test_list_titles_are_not_cut();
  test_battery_pill();
  test_preview_shows_the_thumbnail();
  test_shortcuts_need_a_pad();
  test_shortcuts_bind();
  test_shortcuts_labels();
  test_controls_labels();
  test_shortcuts_keep_a_way_in();
  test_shortcuts_modifier();
  test_shortcuts_defaults();
  test_pad_mapping();
  test_display_name();
  test_screenshot_notice_switch();
  test_notice_timing();
  test_notice_fades();
  test_notice_bounds();
  test_setting_notice();
  test_shader_rows();
  test_painter_alpha();
  test_blend();
  test_decode_png_and_bmp();
  test_battery();
  test_theme();
  test_font_fallback();
  test_shapes();
  std::puts("minui_menu: ok");
  return 0;
}
