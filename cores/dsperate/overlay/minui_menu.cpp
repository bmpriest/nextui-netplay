// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. See minui_menu.h.
#include "minui_menu.h"
#include "minui_shortcuts.h"
#include "core/io/io.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace ds::sdl {

using namespace minui_gfx;
using namespace minui_shortcuts;
using B = io::Io::Button;

namespace {

bool hit(u32 presses, B b) { return (presses >> b) & 1; }
bool confirm(u32 presses) { return hit(presses, B::BTN_A) || hit(presses, B::BTN_START); }

// The name editor's character sets. Firmware stores each byte as one UTF-16
// unit, so they are ASCII; the same three upstream's editor offers, so a
// name typed in either reads back the same.
const char* const kCharTables[] = {
  " ABCDEFGHIJKLMNOPQRSTUVWXYZ",
  " abcdefghijklmnopqrstuvwxyz",
  " 0123456789.,!?'-&+()/:@",
};
const char* const kCharTableNames[] = {"Capitals", "Lowercase", "Numbers & symbols"};
constexpr int kCharTableCount = 3;
// Widest a field is laid out before it wraps onto a second line.
constexpr int kEditCols = 13;

int table_of(char c) {
  for (int t = 0; t < kCharTableCount; ++t)
    if (std::strchr(kCharTables[t], c)) return t;
  return 0;
}


const char* const kSettingsTitles[5] = {"Emulation", "Visual FX", "Layout", "DS Options", "Shaders"};
const char* const kShadersLink = "SHADERS (ADVANCED)";

// Filled, unsmoothed: the battery's bolt and the list's scroll arrows are a
// few pixels across, where smoothing buys nothing.
void fill_triangle(Painter& p, double x0, double y0, double x1, double y1, double x2, double y2, u32 colour) {
  const int minx = static_cast<int>(std::floor(std::min({x0, x1, x2})));
  const int maxx = static_cast<int>(std::ceil(std::max({x0, x1, x2})));
  const int miny = static_cast<int>(std::floor(std::min({y0, y1, y2})));
  const int maxy = static_cast<int>(std::ceil(std::max({y0, y1, y2})));
  const auto edge = [](double ax, double ay, double bx, double by, double px, double py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
  };
  const double area = edge(x0, y0, x1, y1, x2, y2);
  if (area == 0.0) return;
  for (int y = miny; y < maxy; ++y)
    for (int x = minx; x < maxx; ++x) {
      const double px = x + 0.5, py = y + 0.5;
      const double w0 = edge(x1, y1, x2, y2, px, py), w1 = edge(x2, y2, x0, y0, px, py), w2 = edge(x0, y0, x1, y1, px, py);
      if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) p.plot(x, y, colour);
    }
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// One [section] of an ini as key -> value. The profiles are the pak's own and
// plain: no inline comments, no continuation lines.
std::map<std::string, std::string> read_section(const std::string& path, const std::string& section) {
  std::map<std::string, std::string> out;
  std::ifstream f(path);
  if (!f) return out;
  bool in = false;
  for (std::string line; std::getline(f, line);) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line[0] == '[') { in = line == "[" + section + "]"; continue; }
    const size_t eq = line.find('=');
    if (!in || eq == std::string::npos) continue;
    out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return out;
}

std::vector<std::string> words(const char* s) {
  std::vector<std::string> out;
  std::istringstream in(s ? s : "");
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}

int shortcut_index(const char* key) {
  for (int i = 0; i < kShortcutCount; ++i)
    if (std::strcmp(kShortcuts[i].key, key) == 0) return i;
  return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and the forwarded half of Menu's API

MinUiMenu::MinUiMenu() : MinUiMenu([] {
  const char* d = std::getenv("MINUI_DIR");
  return d && *d;
}()) {}

MinUiMenu::MinUiMenu(bool active) : active_(active), font_(std::make_unique<Font>()) {
  if (!active_) return;
  set_theme(Theme::from_env());
  if (const char* p = std::getenv("MINUI_PROFILE_INI")) profile_ = p;
  pad_buttons_ = words(std::getenv("MINUI_PAD_BUTTONS"));
  if (const char* l = std::getenv("MINUI_BUTTON_LABELS")) set_button_labels(l);
  // NextUI's own switch for its "Screenshot saved" notice; on unless it is off.
  const char* n = std::getenv("MINUI_NOTIFY_SCREENSHOT");
  notify_screenshot_ = !(n && std::strcmp(n, "0") == 0);
}

// NextUI's name for a game (getDisplayName, then getAlias in minarch): the
// ROM's map.txt alias when it has one, else the filename with its short
// extensions and everything from the first ( or [ dropped. Empty for no ROM.
std::string MinUiMenu::display_name(const std::string& rom_path) {
  if (rom_path.empty()) return {};
  const size_t slash = rom_path.find_last_of('/');
  const std::string file = slash == std::string::npos ? rom_path : rom_path.substr(slash + 1);
  const std::string dir = slash == std::string::npos ? std::string(".") : rom_path.substr(0, slash);
  std::ifstream map(dir + "/map.txt");
  for (std::string line; map && std::getline(map, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    const size_t tab = line.find('\t');
    if (tab != std::string::npos && line.compare(0, tab, file) == 0 && tab == file.size()) return line.substr(tab + 1);
  }
  std::string name = file;
  // ".nds", and ".p8.png" in two goes: a dot and one to four letters.
  for (size_t dot; (dot = name.find_last_of('.')) != std::string::npos;) {
    const size_t len = name.size() - dot;
    if (len > 2 && len <= 5) name.resize(dot);
    else break;
  }
  const std::string whole = name;
  // From the last ( to the end, again and again, then the same for [; a
  // group that starts the name stops it, as getDisplayName's loop does.
  for (const char open : {'(', '['}) {
    bool stop = false;
    for (size_t at; !stop && (at = name.find_last_of(open)) != std::string::npos;) {
      if (at == 0) stop = true;
      else name.resize(at);
    }
    if (stop) break;
  }
  if (name.empty()) name = whole;
  while (name.size() > 1 && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
  return name;
}

void MinUiMenu::set_theme(const Theme& theme) {
  theme_ = theme;
  font_ = std::make_unique<Font>();
  if (!theme_.font.empty() && font_->load(theme_.font))
    std::fprintf(stderr, "minui: menu font %s (%s look)\n", theme_.font.c_str(), theme_.nextui ? "NextUI" : "MinUI");
  dirty_ = true;
}

bool MinUiMenu::open() const { return forwarding() ? fallback_.open() : open_; }

void MinUiMenu::set_open(bool o) {
  if (!active_) { fallback_.set_open(o); return; }
  if (games_) {
    // The picker closes the way it opened, on upstream's menu.
    fallback_.set_open(false);
    games_ = false;
    if (!o) return;
  }
  // Layout-affecting changes are applied here, once, as upstream does: the
  // screens must not jump about under a page the player is still reading.
  if (!o && open_ && host_) host_->commit();
  open_ = o;
  depth_ = 1;
  stack_[0] = Page::Root;
  row_ = 0;
  close_next_ = false;
  slots_armed_ = false;
  reset_armed_ = false;
  repeat_dir_ = 0; repeat_ms_ = 0; repeating_ = false;
  faces_ = 0;
  // minarch's picker has eight slots, and its hidden default 8 opens on 0.
  if (o && (slot_ < 0 || slot_ >= kRootSlots)) slot_ = 0;
  dirty_ = true;
}

void MinUiMenu::set_cheats(std::vector<cheat::Code>* codes, const std::vector<cheat::Group>* groups) {
  fallback_.set_cheats(codes, groups);
  codes_ = codes;
  groups_ = groups;
  lines_.clear();
}

void MinUiMenu::set_settings_host(SettingsHost* host) {
  fallback_.set_settings_host(host);
  host_ = host;
}

void MinUiMenu::set_network_session(bool on) {
  fallback_.set_network_session(on);
  net_session_ = on;
  // The root and Options pages lose rows; neither selection may be past the end.
  const int n = static_cast<int>(root_items().size());
  if (row_ >= n) row_ = n - 1;
  const int no = static_cast<int>(options().size());
  if (opt_row_ >= no) opt_row_ = std::max(0, no - 1);
  // Nothing below the root that needs a state may stay up.
  if (on && (page() == Page::Slots)) depth_ = 1;
  dirty_ = true;
}

void MinUiMenu::set_cheevos_host(CheevosHost* host) {
  fallback_.set_cheevos_host(host);
  cheevos_ = host;
}

bool MinUiMenu::cheats_dirty() const { return active_ ? cheats_dirty_ || fallback_.cheats_dirty() : fallback_.cheats_dirty(); }

void MinUiMenu::clear_cheats_dirty() {
  fallback_.clear_cheats_dirty();
  cheats_dirty_ = false;
}

void MinUiMenu::set_games(const std::vector<GameEntry>* games) { fallback_.set_games(games); }

void MinUiMenu::open_games() {
  if (active_) {
    open_ = false;
    games_ = true;
  }
  fallback_.open_games();
}

const std::string& MinUiMenu::chosen() const { return fallback_.chosen(); }

int MinUiMenu::slot() const { return active_ ? slot_ : fallback_.slot(); }

int MinUiMenu::doomed_slot() const { return active_ ? doomed_ : fallback_.doomed_slot(); }

void MinUiMenu::set_slot(int s) {
  fallback_.set_slot(s);
  slot_ = s;
}

void MinUiMenu::set_slot_used(int s, bool used) {
  fallback_.set_slot_used(s, used);
  if (s >= 0 && s <= kAutoSlot) used_[s] = used;
  dirty_ = true;
}

void MinUiMenu::set_slot_notice(const char* n) {
  fallback_.set_slot_notice(n);
  notice_ = n ? n : "";
  notice_slot_ = notice_.empty() ? -1 : slot_;
  dirty_ = true;
}

void MinUiMenu::clear_slot_notice() {
  fallback_.clear_slot_notice();
  notice_.clear();
  notice_slot_ = -1;
}

void MinUiMenu::face_presses(u32 faces) {
  if (forwarding()) fallback_.face_presses(faces);
  else faces_ |= faces;
}

bool MinUiMenu::dirty() const { return forwarding() ? fallback_.dirty() : dirty_; }

void MinUiMenu::clear_dirty() {
  fallback_.clear_dirty();
  dirty_ = false;
}

// ---------------------------------------------------------------------------
// Navigation

void MinUiMenu::push(Page p) {
  if (depth_ < kMaxDepth) stack_[depth_++] = p;
}

bool MinUiMenu::pop() {
  if (depth_ <= 1) return false;
  --depth_;
  return true;
}

std::vector<MinUiMenu::RootItem> MinUiMenu::root_items() const {
  std::vector<RootItem> v{RootItem::Continue};
  // A state freezes this machine and not the peer, so a network session
  // has neither, as upstream's menu hides them.
  if (!net_session_) { v.push_back(RootItem::Save); v.push_back(RootItem::Load); }
  if (!options().empty()) v.push_back(RootItem::Options);
  v.push_back(RootItem::Quit);
  return v;
}

std::vector<MinUiMenu::Opt> MinUiMenu::options() const {
  std::vector<Opt> v;
  if (host_) {
    v.push_back(Opt::Emulation);
    v.push_back(Opt::VisualFx);
    v.push_back(Opt::Layout);
    v.push_back(Opt::Controls);
    // The pak's chords are all on the pad.
    if (host_->has_pad()) v.push_back(Opt::Shortcuts);
    v.push_back(Opt::DsOptions);
  }
  if (codes_ && !codes_->empty()) v.push_back(Opt::Cheats);
  if (cheevos_) v.push_back(Opt::Achievements);
  if (!net_session_) v.push_back(Opt::Slots);
  if (host_ && host_->has_game()) v.push_back(Opt::SaveChanges);
  // Ends a session just as surely as a state would, so it goes with them.
  if (!net_session_) v.push_back(Opt::Reset);
  return v;
}

std::vector<MinUiMenu::AccountRow> MinUiMenu::account_rows() const {
  std::vector<AccountRow> v;
  if (!cheevos_) return v;
  v.push_back(cheevos_->signed_in() ? AccountRow::SignOut : AccountRow::SignIn);
  if (cheevos_->has_set() && cheevos_->row_count() > 0) v.push_back(AccountRow::List);
  v.push_back(AccountRow::Toasts);
  v.push_back(AccountRow::Screenshot);
  v.push_back(AccountRow::Encore);
  return v;
}

const Setting* MinUiMenu::table() const {
  static const Setting* const kTables[5] = {kEmuSettings, kVideoSettings, kLayoutSettings, kUserSettings, kShaderSettings};
  return kTables[std::clamp(set_table_, 0, kShaderTable)];
}

int MinUiMenu::link_row() const {
  if (set_table_ != 1) return -1;
  const Setting* t = table();
  for (int i = 0; t[i].key; ++i) if (!std::strcmp(t[i].key, "video.preset")) return i + 1;
  return -1;
}

int MinUiMenu::settings_rows() const { return settings_count(table()) + (link_row() >= 0 ? 1 : 0); }

const Setting* MinUiMenu::setting_row(int i) const {
  const int link = link_row();
  if (link >= 0 && i == link) return nullptr;
  return &table()[link >= 0 && i > link ? i - 1 : i];
}

bool MinUiMenu::list_page() const { return page() != Page::Root && page() != Page::TextEdit; }

MinUiMenu::Result MinUiMenu::update(u32 presses, u32 held, u32 ms) {
  if (forwarding()) return fallback_.update(presses, held, ms);
  if (presses || faces_) dirty_ = true;

  // Held up/down repeat on the list pages, at upstream's rate.
  if (list_page()) {
    const int dir = (held & (1u << B::BTN_UP)) ? -1 : (held & (1u << B::BTN_DOWN)) ? 1 : 0;
    if (dir != repeat_dir_) { repeat_dir_ = dir; repeat_ms_ = 0; repeating_ = false; }
    else if (dir != 0) {
      repeat_ms_ += ms;
      for (u32 step = repeating_ ? kRepeatRateMs : kRepeatDelayMs; repeat_ms_ >= step; step = kRepeatRateMs) {
        repeat_ms_ -= step;
        repeating_ = true;
        move_list(dir);
        dirty_ = true;
      }
    }
  } else {
    repeat_dir_ = 0; repeat_ms_ = 0; repeating_ = false;
  }

  const Result r = handle(presses);
  faces_ = 0;
  return r;
}

MinUiMenu::Result MinUiMenu::handle(u32 presses) {
  // Save leaves the menu, as minarch's does. Main saves on the Save result
  // and refreshes the slots; the Resume a tick later closes through main's
  // ordinary path, which also keeps the cheats and unpauses.
  if (close_next_) { close_next_ = false; dirty_ = true; return Result::Resume; }
  switch (page()) {
  case Page::Root:           return handle_root(presses);
  case Page::Options:        return handle_options(presses);
  case Page::Settings:       return handle_settings(presses);
  case Page::Controls:       return handle_controls(presses);
  case Page::Cheats:         return handle_cheats(presses);
  case Page::CheevosAccount: return handle_account(presses);
  case Page::Cheevos:        return handle_cheevos(presses);
  case Page::TextEdit:       return handle_text_edit(presses);
  case Page::Slots:          return handle_slots(presses);
  case Page::Shortcuts:      return handle_shortcuts(presses);
  }
  return Result::None;
}

void MinUiMenu::move_list(int dir) {
  const auto wrap = [dir](int& at, int n) { if (n > 0) at = ((at + dir) % n + n) % n; };
  switch (page()) {
  case Page::Root:     wrap(row_, static_cast<int>(root_items().size())); break;
  case Page::Options:  wrap(opt_row_, static_cast<int>(options().size())); break;
  case Page::Settings: wrap(set_row_[set_table_], settings_rows()); break;
  case Page::Controls: wrap(bind_row_, host_ ? static_cast<int>(control_rows().size()) : 0); reset_armed_ = false; break;
  case Page::Cheats:   move_cheat_row(dir); break;
  case Page::CheevosAccount: wrap(account_row_, static_cast<int>(account_rows().size())); break;
  case Page::Cheevos: {
    const int n = cheevos_ ? cheevos_->row_count() : 0;
    if (n > 0) cheevos_row_ = std::clamp(cheevos_row_ + dir, 0, n - 1);
    break;
  }
  case Page::Slots:    wrap(slots_row_, kAutoSlot + 1); slots_armed_ = false; break;
  case Page::Shortcuts: wrap(short_row_, kShortcutCount + 1); short_armed_ = false; short_refused_ = false; break;
  case Page::TextEdit: break;
  }
}

MinUiMenu::Result MinUiMenu::handle_root(u32 presses) {
  const std::vector<RootItem> items = root_items();
  const int n = static_cast<int>(items.size());
  row_ = std::clamp(row_, 0, n - 1);
  if (hit(presses, B::BTN_UP))   move_list(-1);
  if (hit(presses, B::BTN_DOWN)) move_list(+1);
  const RootItem item = items[static_cast<size_t>(row_)];
  if (item == RootItem::Save || item == RootItem::Load) {
    if (hit(presses, B::BTN_LEFT))  slot_ = (slot_ + kRootSlots - 1) % kRootSlots;
    if (hit(presses, B::BTN_RIGHT)) slot_ = (slot_ + 1) % kRootSlots;
  }
  if (hit(presses, B::BTN_B)) return Result::Resume;
  if (!confirm(presses)) return Result::None;
  switch (item) {
  case RootItem::Continue: return Result::Resume;
  case RootItem::Save:     close_next_ = true; return Result::Save;
  // Nothing to load is nothing to do; the preview already says Empty Slot.
  case RootItem::Load:     return used_[slot_] ? Result::Load : Result::None;
  case RootItem::Options:  opt_row_ = 0; opt_top_ = 0; push(Page::Options); return Result::None;
  case RootItem::Quit:     return Result::Quit;
  }
  return Result::None;
}

MinUiMenu::Result MinUiMenu::handle_options(u32 presses) {
  const std::vector<Opt> opts = options();
  if (opts.empty()) { if (hit(presses, B::BTN_B)) pop(); return Result::None; }
  opt_row_ = std::clamp(opt_row_, 0, static_cast<int>(opts.size()) - 1);
  if (hit(presses, B::BTN_UP))   move_list(-1);
  if (hit(presses, B::BTN_DOWN)) move_list(+1);
  const Opt o = opts[static_cast<size_t>(opt_row_)];
  if (hit(presses, B::BTN_B)) { pop(); return Result::None; }
  // Left and right work the save switch in place, as they work a setting.
  if (o == Opt::SaveChanges) {
    if (hit(presses, B::BTN_LEFT) || hit(presses, B::BTN_RIGHT) || confirm(presses))
      host_->set_save_per_game(!host_->save_per_game());
    return Result::None;
  }
  if (!confirm(presses)) return Result::None;
  if (o == Opt::Reset) return Result::Reset;
  open_option(o);
  return Result::None;
}

void MinUiMenu::open_option(Opt o) {
  switch (o) {
  case Opt::Emulation: case Opt::VisualFx: case Opt::Layout: case Opt::DsOptions:
    set_table_ = o == Opt::Emulation ? 0 : o == Opt::VisualFx ? 1 : o == Opt::Layout ? 2 : 3;
    set_row_[set_table_] = 0;
    set_top_[set_table_] = 0;
    push(Page::Settings);
    break;
  case Opt::Controls:
    // Opens on the pad column when one is plugged in, as upstream's does.
    bind_pad_ = host_->has_pad();
    bind_row_ = 0;
    bind_top_ = 0;
    reset_armed_ = false;
    push(Page::Controls);
    break;
  case Opt::Cheats:
    build_lines();
    cheat_row_ = 0;
    cheat_top_ = 0;
    if (!lines_.empty() && lines_[0].kind != Line::Toggle) move_cheat_row(+1);
    push(Page::Cheats);
    break;
  case Opt::Achievements:
    // The account page first: it says signed in or why not, carries the
    // switches, and the list is one row away from it.
    account_row_ = 0;
    account_top_ = 0;
    push(Page::CheevosAccount);
    break;
  case Opt::Slots:
    slots_row_ = std::clamp(slot_, 0, kSlots - 1);
    slots_top_ = 0;
    slots_armed_ = false;
    push(Page::Slots);
    break;
  case Opt::Shortcuts:
    short_row_ = 0;
    short_top_ = 0;
    short_armed_ = false;
    short_refused_ = false;
    push(Page::Shortcuts);
    break;
  case Opt::SaveChanges: case Opt::Reset:
    break;
  }
}

void MinUiMenu::step_setting(int dir) {
  const Setting* row = setting_row(set_row_[set_table_]);
  if (!row) return;
  const Setting& s = *row;
  // A row the host has switched off can be selected, so its reason shows,
  // but it never steps.
  if (!host_->enabled(s)) return;
  if (s.type == Setting::Type::Text) return;   // A opens the editor instead
  const std::string cur = host_->get(s.key);
  const std::string next = step_value(s, cur, dir, *host_);
  if (next != cur) host_->set(s.key, next);
}

MinUiMenu::Result MinUiMenu::handle_settings(u32 presses) {
  const int n = settings_rows();
  // Back from the Shaders page lands on Visual FX's link row.
  const auto back = [this] { if (set_table_ == kShaderTable) set_table_ = 1; pop(); };
  if (n <= 0) { if (hit(presses, B::BTN_B)) back(); return Result::None; }
  int& row = set_row_[set_table_];
  row = std::clamp(row, 0, n - 1);
  if (hit(presses, B::BTN_UP))    move_list(-1);
  if (hit(presses, B::BTN_DOWN))  move_list(+1);
  if (hit(presses, B::BTN_L))     row = std::max(0, row - std::max(1, visible_));
  if (hit(presses, B::BTN_R))     row = std::min(n - 1, row + std::max(1, visible_));
  if (row == link_row()) {
    if (confirm(presses) || hit(presses, B::BTN_RIGHT)) {
      set_table_ = kShaderTable;
      set_row_[set_table_] = 0;
      set_top_[set_table_] = 0;
      push(Page::Settings);
    } else if (hit(presses, B::BTN_B)) back();
    return Result::None;
  }
  if (hit(presses, B::BTN_LEFT))  step_setting(-1);
  if (hit(presses, B::BTN_RIGHT)) step_setting(+1);
  // A steps forward too, so the page can be worked with one button.
  if (confirm(presses)) {
    const Setting& cur = *setting_row(row);
    if (cur.type == Setting::Type::Text && host_->enabled(cur)) open_text_edit();
    else step_setting(+1);
  }
  if (hit(presses, B::BTN_B)) back();
  return Result::None;
}

// The host's rows the Controls page shows. With a pad, the Shortcuts page
// owns the modifier and its chords, so the pad column leaves them out, and
// their second bindings, the "(2)" rows upstream adds once one is bound. The
// launcher owns the volume, and the pak is always full screen, so neither
// column offers those hotkeys.
std::vector<int> MinUiMenu::control_rows() const {
  static const char* const kLauncherOwns[] = {"volume_up", "volume_down", "mute", "fullscreen"};
  const std::string section = bind_pad_ ? "padhotkeys." : "hotkeys.";
  const bool shortcuts = bind_pad_ && host_->has_pad();
  std::vector<int> out;
  for (int i = 0, n = host_->binding_count(bind_pad_); i < n; ++i) {
    const std::string key = host_->binding(bind_pad_, i).key;
    bool hidden = shortcuts && key == "padhotkeys.modifier";
    for (const Shortcut& sc : kShortcuts)
      if (shortcuts && (key == section + sc.key || key == section + sc.key + ".alt")) hidden = true;
    for (const char* name : kLauncherOwns)
      if (key == section + name || key == section + name + ".alt") hidden = true;
    if (!hidden) out.push_back(i);
  }
  return out;
}

MinUiMenu::Result MinUiMenu::handle_controls(u32 presses) {
  // A hotkey's second row comes and goes as its first is bound and cleared,
  // so the row under the cursor may have gone.
  const std::vector<int> rows = control_rows();
  const int n = static_cast<int>(rows.size());
  bind_row_ = n > 0 ? std::clamp(bind_row_, 0, n - 1) : 0;
  const int row = n > 0 ? rows[static_cast<size_t>(bind_row_)] : -1;
  // Repaint when listening stops, even from a capture the device cancelled.
  if (const bool listening = host_->capturing(); listening != listen_shown_) {
    listen_shown_ = listening;
    dirty_ = true;
  }
  if (host_->capturing()) {
    const std::string got = host_->take_capture();
    if (!got.empty()) {
      const SettingsHost::Binding b = host_->binding(bind_pad_, row);
      if (!b.key.empty()) host_->bind(b.key, got);
      dirty_ = true;
    }
    return Result::None;
  }
  // With a pad, Clear and Defaults are the west and north buttons by
  // position, so rebinding DS Y and X on this page cannot move them.
  const bool pad = host_->has_pad();
  const bool clear = pad ? (faces_ >> 2) & 1 : hit(presses, B::BTN_Y);
  const bool reset = pad ? (faces_ >> 3) & 1 : hit(presses, B::BTN_X);
  // Defaults wipes the whole column, so it takes a second press, and any
  // other press backs out and does only that.
  if (reset_armed_ && (presses || faces_)) {
    reset_armed_ = false;
    dirty_ = true;
    if (reset) {
      host_->reset_bindings(bind_pad_);
      // That put upstream's chords back too, which this page no longer
      // shows: the Shortcuts page's own defaults instead.
      if (bind_pad_ && host_->has_pad()) restore_shortcuts();
    }
    return Result::None;
  }
  if (hit(presses, B::BTN_UP))   move_list(-1);
  if (hit(presses, B::BTN_DOWN)) move_list(+1);
  if (hit(presses, B::BTN_L) || hit(presses, B::BTN_R)) {
    bind_pad_ = !bind_pad_;
    bind_row_ = 0;
    bind_top_ = 0;
  }
  // A alone opens a capture: Start may be the very control being bound.
  if (hit(presses, B::BTN_A)) host_->begin_capture(bind_pad_);
  if (clear) {
    const SettingsHost::Binding b = host_->binding(bind_pad_, row);
    if (!b.key.empty()) host_->bind(b.key, "none");
  }
  if (reset) { reset_armed_ = true; dirty_ = true; }
  if (hit(presses, B::BTN_B)) pop();
  return Result::None;
}

// A heading wherever the group changes, then every code; notes keep their
// place so a "must be on" stays with the codes it is about.
void MinUiMenu::build_lines() {
  lines_.clear();
  if (!codes_) return;
  int last = -2;
  for (size_t i = 0; i < codes_->size(); ++i) {
    const cheat::Code& c = (*codes_)[i];
    if (c.group != last) {
      if (c.group >= 0 && groups_ && static_cast<size_t>(c.group) < groups_->size())
        lines_.push_back({Line::Heading, c.group});
      last = c.group;
    }
    lines_.push_back({c.is_note() ? Line::Note : Line::Toggle, static_cast<int>(i)});
  }
}

// To the next code in `delta`'s direction, over headings and notes, stopping
// at the ends: a list thousands of codes long should not wrap underfoot.
void MinUiMenu::move_cheat_row(int delta) {
  const int n = static_cast<int>(lines_.size());
  for (int at = cheat_row_ + (delta > 0 ? 1 : -1); at >= 0 && at < n; at += delta > 0 ? 1 : -1)
    if (lines_[static_cast<size_t>(at)].kind == Line::Toggle) { cheat_row_ = at; return; }
}

void MinUiMenu::toggle_cheat() {
  if (!codes_ || lines_.empty()) return;
  const Line& l = lines_[static_cast<size_t>(cheat_row_)];
  if (l.kind != Line::Toggle) return;
  cheat::Code& c = (*codes_)[static_cast<size_t>(l.at)];
  c.enabled = !c.enabled;
  cheats_dirty_ = true;
  // In a group of alternatives, turning one on turns the rest off.
  if (!c.enabled || c.group < 0 || !groups_ || static_cast<size_t>(c.group) >= groups_->size()) return;
  if (!(*groups_)[static_cast<size_t>(c.group)].exclusive) return;
  for (size_t i = 0; i < codes_->size(); ++i) {
    cheat::Code& other = (*codes_)[i];
    if (i != static_cast<size_t>(l.at) && other.group == c.group) other.enabled = false;
  }
}

MinUiMenu::Result MinUiMenu::handle_cheats(u32 presses) {
  if (hit(presses, B::BTN_UP))   move_cheat_row(-1);
  if (hit(presses, B::BTN_DOWN)) move_cheat_row(+1);
  if (hit(presses, B::BTN_L)) for (int i = 0; i < visible_; ++i) move_cheat_row(-1);
  if (hit(presses, B::BTN_R)) for (int i = 0; i < visible_; ++i) move_cheat_row(+1);
  if (confirm(presses)) toggle_cheat();
  if (hit(presses, B::BTN_B)) pop();
  return Result::None;
}

MinUiMenu::Result MinUiMenu::handle_account(u32 presses) {
  const std::vector<AccountRow> rows = account_rows();
  if (rows.empty()) { if (hit(presses, B::BTN_B)) pop(); return Result::None; }
  account_row_ = std::clamp(account_row_, 0, static_cast<int>(rows.size()) - 1);
  if (hit(presses, B::BTN_UP))   move_list(-1);
  if (hit(presses, B::BTN_DOWN)) move_list(+1);
  if (hit(presses, B::BTN_B)) { pop(); return Result::None; }
  const AccountRow at = rows[static_cast<size_t>(account_row_)];
  const bool toggle = at == AccountRow::Toasts || at == AccountRow::Screenshot || at == AccountRow::Encore;
  if (toggle && (hit(presses, B::BTN_LEFT) || hit(presses, B::BTN_RIGHT) || confirm(presses))) {
    const CheevosHost::Option o = at == AccountRow::Toasts ? CheevosHost::Option::Toasts
                                : at == AccountRow::Screenshot ? CheevosHost::Option::Screenshot
                                : CheevosHost::Option::Encore;
    cheevos_->set_option(o, !cheevos_->option(o));
    return Result::None;
  }
  if (!confirm(presses)) return Result::None;
  switch (at) {
  case AccountRow::SignIn:  open_credential_edit(EditDest::CheevosUser); break;
  case AccountRow::SignOut: cheevos_->sign_out(); account_row_ = 0; break;
  case AccountRow::List:    cheevos_row_ = 0; cheevos_top_ = 0; push(Page::Cheevos); break;
  case AccountRow::Toasts: case AccountRow::Screenshot: case AccountRow::Encore: break;
  }
  return Result::None;
}

MinUiMenu::Result MinUiMenu::handle_cheevos(u32 presses) {
  const int n = cheevos_ ? cheevos_->row_count() : 0;
  if (hit(presses, B::BTN_UP))   move_list(-1);
  if (hit(presses, B::BTN_DOWN)) move_list(+1);
  if (n > 0 && hit(presses, B::BTN_L)) cheevos_row_ = std::max(0, cheevos_row_ - std::max(1, visible_));
  if (n > 0 && hit(presses, B::BTN_R)) cheevos_row_ = std::min(n - 1, cheevos_row_ + std::max(1, visible_));
  if (hit(presses, B::BTN_B)) pop();
  return Result::None;
}

void MinUiMenu::open_text_edit() {
  const Setting& s = *setting_row(set_row_[set_table_]);
  edit_dest_ = EditDest::Setting;
  edit_key_ = s.key;
  edit_label_ = s.label;
  edit_max_ = s.lo;
  const std::string cur = host_->get(s.key);
  edit_buf_ = cur.empty() ? default_value(s) : cur;
  if (static_cast<int>(edit_buf_.size()) > edit_max_) edit_buf_.resize(static_cast<size_t>(edit_max_));
  if (edit_buf_.empty()) edit_buf_ = " ";   // one space, so there is a character to cycle
  edit_pos_ = 0;
  edit_table_ = table_of(edit_buf_[0]);
  push(Page::TextEdit);
}

// RetroAchievements' own limits: a username is 20 characters.
void MinUiMenu::open_credential_edit(EditDest dest) {
  edit_dest_ = dest;
  edit_key_.clear();
  edit_label_ = dest == EditDest::CheevosUser ? "RetroAchievements username" : "RetroAchievements password";
  edit_max_ = dest == EditDest::CheevosUser ? 20 : 32;
  edit_buf_ = " ";
  edit_pos_ = 0;
  edit_table_ = table_of(edit_buf_[0]);
  push(Page::TextEdit);
}

MinUiMenu::Result MinUiMenu::handle_text_edit(u32 presses) {
  const char* tab = kCharTables[edit_table_];
  const int n = static_cast<int>(std::strlen(tab));
  if (hit(presses, B::BTN_UP) || hit(presses, B::BTN_DOWN)) {
    // A character from another set starts the walk at this set's beginning.
    const char* at = std::strchr(tab, edit_buf_[static_cast<size_t>(edit_pos_)]);
    int i = at ? static_cast<int>(at - tab) : 0;
    i = (i + (hit(presses, B::BTN_UP) ? 1 : n - 1)) % n;
    edit_buf_[static_cast<size_t>(edit_pos_)] = tab[i];
  }
  if (hit(presses, B::BTN_L) || hit(presses, B::BTN_R)) {
    edit_table_ = (edit_table_ + (hit(presses, B::BTN_L) ? kCharTableCount - 1 : 1)) % kCharTableCount;
    // The character under the cursor moves into the new set at the same place.
    const char c = edit_buf_[static_cast<size_t>(edit_pos_)];
    const char* from = kCharTables[table_of(c)];
    if (const char* at = std::strchr(from, c)) {
      const int i = static_cast<int>(at - from);
      const char* to = kCharTables[edit_table_];
      if (i < static_cast<int>(std::strlen(to))) edit_buf_[static_cast<size_t>(edit_pos_)] = to[i];
    }
  }
  if (hit(presses, B::BTN_LEFT) && edit_pos_ > 0) {
    --edit_pos_;
    edit_table_ = table_of(edit_buf_[static_cast<size_t>(edit_pos_)]);
  }
  if (hit(presses, B::BTN_RIGHT)) {
    // Past the end grows the field, up to what it may hold.
    if (edit_pos_ + 1 < static_cast<int>(edit_buf_.size())) {
      ++edit_pos_;
      edit_table_ = table_of(edit_buf_[static_cast<size_t>(edit_pos_)]);
    } else if (static_cast<int>(edit_buf_.size()) < edit_max_) {
      edit_buf_ += ' ';
      ++edit_pos_;
    }
  }
  if (confirm(presses)) {
    // Trailing spaces are what moving right leaves, not part of the name.
    std::string out = edit_buf_;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    // A credential cannot hold a space at all: one in the middle is a slot
    // the cursor passed over without choosing.
    if (edit_dest_ != EditDest::Setting) out.erase(std::remove(out.begin(), out.end(), ' '), out.end());
    switch (edit_dest_) {
    case EditDest::Setting:
      host_->set(edit_key_.c_str(), out);
      pop();
      break;
    case EditDest::CheevosUser:
      pending_user_ = out;
      pop();
      // Straight on to the password, so the two prompts read as one action.
      if (!pending_user_.empty()) open_credential_edit(EditDest::CheevosPassword);
      break;
    case EditDest::CheevosPassword:
      pop();
      if (cheevos_) cheevos_->sign_in(pending_user_, out);
      pending_user_.clear();
      out.assign(out.size(), ' ');
      edit_buf_.assign(edit_buf_.size(), ' ');
      break;
    }
    return Result::None;
  }
  if (hit(presses, B::BTN_B)) {   // nothing was written until A
    pop();
    pending_user_.clear();        // a cancelled sign-in cannot be finished later
    if (edit_dest_ == EditDest::CheevosPassword) edit_buf_.assign(edit_buf_.size(), ' ');
  }
  return Result::None;
}

// Every slot that can hold a state, and the auto state, with a delete that
// takes a second press. The root's picker offers only minarch's eight, but
// the slot hotkeys reach 8 and 9, and a state there must still be removable.
MinUiMenu::Result MinUiMenu::handle_slots(u32 presses) {
  const bool yes = confirm(presses);
  // The press that backs out of an armed delete does only that.
  if (slots_armed_ && presses && !yes) { slots_armed_ = false; return Result::None; }
  if (hit(presses, B::BTN_UP))   move_list(-1);
  if (hit(presses, B::BTN_DOWN)) move_list(+1);
  if (hit(presses, B::BTN_B)) { pop(); return Result::None; }
  if (!yes) return Result::None;
  if (!used_[slots_row_]) return Result::None;
  if (!slots_armed_) { slots_armed_ = true; return Result::None; }
  slots_armed_ = false;
  doomed_ = slots_row_;
  if (doomed_ == notice_slot_) { notice_.clear(); notice_slot_ = -1; }
  return Result::Delete;
}

// The pak's modifier chords and the modifier they hang on. Every
// change goes through SettingsHost::bind, as the Controls page's do, so it
// lands where Save Changes says and the input is reconfigured at once.
std::string MinUiMenu::modifier() const {
  const std::string m = host_->get("padhotkeys.modifier");
  return m.empty() ? kDefaultModifier : m;
}

// The candidates the pad mapping actually has, in kModifiers' order, and the
// current modifier wherever it came from, so stepping never loses it.
std::vector<std::string> MinUiMenu::modifier_choices() const {
  const std::string cur = modifier();
  std::vector<std::string> out;
  bool known = false;
  for (const Modifier& m : kModifiers) {
    const bool mapped = std::find(pad_buttons_.begin(), pad_buttons_.end(), m.name) != pad_buttons_.end();
    if (cur == m.name) known = true;
    if (mapped || cur == m.name) out.push_back(m.name);
  }
  if (!known) out.insert(out.begin(), cur);
  return out;
}

void MinUiMenu::set_button_labels(const std::string& pairs) {
  button_labels_.clear();
  for (const std::string& w : words(pairs.c_str())) {
    const size_t eq = w.find('=');
    if (eq != std::string::npos && eq > 0 && eq + 1 < w.size()) button_labels_[w.substr(0, eq)] = w.substr(eq + 1);
  }
}

std::string MinUiMenu::button_label(const std::string& name) const {
  if (const auto it = button_labels_.find(name); it != button_labels_.end()) return it->second;
  for (const ButtonLabel& b : kButtonLabels)
    if (name == b.name) return b.label;
  std::string up = name;
  for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return up;
}

// What the key holds, or what upstream runs with when it holds nothing.
std::string MinUiMenu::shortcut_value(int i) const {
  const Shortcut& s = kShortcuts[i];
  const std::string v = host_->get((std::string("padhotkeys.") + s.key).c_str());
  return v.empty() ? s.upstream : v;
}

// A binding as the page shows it, NextUI's way: "MENU+R1", "L2", "MENU" for
// the modifier's tap, or NONE. "mod++lefttrigger" is the modifier and the
// trigger's axis, whose own sign follows the separator.
std::string MinUiMenu::binding_label(const std::string& v) const {
  if (v.empty() || v == "none") return "NONE";
  std::string out, sign;
  for (size_t at = 0; at <= v.size();) {
    const size_t plus = v.find('+', at);
    const std::string part = v.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
    at = plus == std::string::npos ? v.size() + 1 : plus + 1;
    if (part.empty()) { sign = "+"; continue; }
    if (!out.empty()) out += "+";
    out += part == "mod" ? button_label(modifier()) : button_label(sign + part);
    sign.clear();
  }
  return out;
}

std::string MinUiMenu::controls_label(const std::string& v) const {
  // Upstream's label for each control the pak names otherwise
  // (Input::pad_label, src/frontend/sdl/input.cpp).
  static const ButtonLabel kUpstream[] = {
    {"a", "\x01 SOUTH"}, {"b", "\x02 EAST"}, {"x", "\x03 WEST"}, {"y", "\x04 NORTH"},
    {"guide", "FUNC BTN"}, {"leftstick", "L3"}, {"rightstick", "R3"},
  };
  std::string out;
  for (size_t at = 0; at <= v.size();) {
    const size_t plus = v.find('+', at);
    std::string part = v.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
    at = plus == std::string::npos ? v.size() + 1 : plus + 1;
    if (part == "MOD") part = button_label(modifier());
    for (const ButtonLabel& b : kUpstream)
      if (part == b.label) part = button_label(b.name);
    out += part;
    if (at <= v.size()) out += "+";
  }
  return out;
}

void MinUiMenu::step_modifier(int dir) {
  const std::vector<std::string> choices = modifier_choices();
  const int n = static_cast<int>(choices.size());
  const int at = static_cast<int>(std::find(choices.begin(), choices.end(), modifier()) - choices.begin());
  const std::string next = choices[static_cast<size_t>(((at + dir) % n + n) % n)];
  if (next != modifier()) host_->bind("padhotkeys.modifier", next);
}

// X: none, and on to the next row as NextUI does. The menu is the only way
// to bind these again, so the last way into it stays: the pause chord and
// its alternate cannot both be none.
void MinUiMenu::clear_shortcut(int i) {
  const int pause = shortcut_index("pause"), alt = shortcut_index("pause.alt");
  if ((i == pause || i == alt) && shortcut_value(i == pause ? alt : pause) == "none") {
    short_refused_ = true;
    return;
  }
  host_->bind(std::string("padhotkeys.") + kShortcuts[i].key, "none");
  move_list(+1);
}

// The device's own: the profile launch.sh seeded the config from. A key the
// profile leaves out, or no profile at all, is the table's chord.
void MinUiMenu::restore_shortcuts() {
  const std::map<std::string, std::string> shipped = read_section(profile_, "padhotkeys");
  const auto it = shipped.find("modifier");
  host_->bind("padhotkeys.modifier", it == shipped.end() ? kDefaultModifier : it->second);
  for (const Shortcut& s : kShortcuts) {
    const auto v = shipped.find(s.key);
    host_->bind(std::string("padhotkeys.") + s.key, v == shipped.end() ? s.chord : v->second);
  }
}

MinUiMenu::Result MinUiMenu::handle_shortcuts(u32 presses) {
  const int n = kShortcutCount + 1;
  short_row_ = std::clamp(short_row_, 0, n - 1);
  // Repaint when listening stops, even from a capture the device cancelled.
  if (const bool listening = host_->capturing(); listening != listen_shown_) {
    listen_shown_ = listening;
    dirty_ = true;
  }
  // NextUI's binding: the next button, alone or with the modifier held, and
  // on to the next row. SettingsHost::bind turns the modifier alone into its
  // tap, "mod".
  if (host_->capturing()) {
    const std::string got = host_->take_capture();
    if (!got.empty() && short_row_ > 0) {
      host_->bind(std::string("padhotkeys.") + kShortcuts[short_row_ - 1].key, got);
      move_list(+1);
      dirty_ = true;
    }
    return Result::None;
  }
  // Clear and Defaults are the north and west buttons by position, as on the
  // Controls page, so rebinding DS X and Y cannot move them.
  const bool pad = host_->has_pad();
  const bool clear = pad ? (faces_ >> 3) & 1 : hit(presses, B::BTN_X);
  const bool reset = pad ? (faces_ >> 2) & 1 : hit(presses, B::BTN_Y);
  // Defaults rewrites every row, so it takes a second press, and any other
  // press backs out and does only that.
  if (short_armed_ && (presses || faces_)) {
    short_armed_ = false;
    if (reset) restore_shortcuts();
    return Result::None;
  }
  if (presses || faces_) short_refused_ = false;
  if (hit(presses, B::BTN_UP))   move_list(-1);
  if (hit(presses, B::BTN_DOWN)) move_list(+1);
  if (hit(presses, B::BTN_L))    short_row_ = std::max(0, short_row_ - std::max(1, visible_));
  if (hit(presses, B::BTN_R))    short_row_ = std::min(n - 1, short_row_ + std::max(1, visible_));
  if (short_row_ == 0) {
    const int dir = hit(presses, B::BTN_LEFT) ? -1 : hit(presses, B::BTN_RIGHT) || confirm(presses) ? 1 : 0;
    if (dir != 0) step_modifier(dir);
  } else if (hit(presses, B::BTN_A)) {
    // A alone opens a capture: Start may be the very button being bound.
    host_->begin_capture(true);
  } else if (clear) {
    clear_shortcut(short_row_ - 1);
  }
  if (reset) short_armed_ = true;
  if (hit(presses, B::BTN_B)) pop();
  return Result::None;
}

// ---------------------------------------------------------------------------
// Drawing

void MinUiMenu::draw(const Canvas& d) const {
  if (forwarding()) { fallback_.draw(d); return; }
  if (d.w <= 0 || d.h <= 0 || !d.px) return;
  Painter p(d);
  const Metrics m = Metrics::for_canvas(d.w, d.h);
  switch (page()) {
  case Page::Root:           draw_root(p, m); break;
  case Page::Options:        draw_options(p, m); break;
  case Page::Settings:       draw_settings(p, m); break;
  case Page::Controls:       draw_controls(p, m); break;
  case Page::Cheats:         draw_cheats(p, m); break;
  case Page::CheevosAccount: draw_account(p, m); break;
  case Page::Cheevos:        draw_cheevos(p, m); break;
  case Page::TextEdit:       draw_text_edit(p, m); break;
  case Page::Slots:          draw_slots(p, m); break;
  case Page::Shortcuts:      draw_shortcuts(p, m); break;
  }
}

int MinUiMenu::draw_title(Painter& p, const Metrics& m, const std::string& text, int max_w) const {
  const int px = m.u(Metrics::FONT_LARGE), bp = m.u(Metrics::BUTTON_PADDING);
  const int x = m.u(m.padding), y = m.u(m.padding), h = m.u(Metrics::PILL_SIZE);
  if (max_w <= 2 * bp) return x;
  const std::string shown = font_->fit(px, text, max_w - 2 * bp);
  const int w = std::min(max_w, font_->width(px, shown) + 2 * bp);
  p.pill(x, y, w, h, theme_.title_pill());
  font_->draw(p, x + bp, y + (h - font_->line_height(px)) / 2, px, theme_.title_text(), shown);
  return x + w;
}

int MinUiMenu::draw_hardware(Painter& p, const Metrics& m) const {
  const Battery bat = read_battery(battery_root_);
  if (bat.percent < 0) return 0;
  const int size = m.u(Metrics::PILL_SIZE);
  const int x = p.canvas().w - m.u(m.padding) - size, y = m.u(m.padding);
  p.pill(x, y, size, size, theme_.group_pill());

  // minarch's battery asset, 17x10 units: a body, its terminal, and a fill
  // that drains from the left. Red when low, a bolt while charging.
  const int bx = x + (size - m.u(17)) / 2, by = y + (size - m.u(10)) / 2;
  const u32 bg = theme_.group_pill();
  const u32 edge = !bat.charging && bat.percent <= 10 ? kRed : kWhite;
  p.rounded(bx, by, m.u(15), m.u(10), m.u(2), edge);
  p.rounded(bx + m.u(1), by + m.u(1), m.u(13), m.u(8), m.u(1), bg);
  p.fill(bx + m.u(15), by + m.u(3), m.u(2), m.u(4), edge);
  const int fx = bx + m.u(2), fy = by + m.u(2), fw = m.u(11), fh = m.u(6);
  if (bat.charging) {
    p.fill(fx, fy, fw, fh, kWhite);
    const double ux = fw / 11.0, uy = fh / 6.0;
    fill_triangle(p, fx + 6.5 * ux, fy, fx + 2.5 * ux, fy + 3.5 * uy, fx + 6 * ux, fy + 3 * uy, bg);
    fill_triangle(p, fx + 5 * ux, fy + 3 * uy, fx + 8.5 * ux, fy + 2.5 * uy, fx + 4.5 * ux, fy + fh, bg);
  } else {
    const int w = fw * bat.percent / 100;
    if (w > 0) p.fill(fx + fw - w, fy, w, fh, bat.percent <= 20 ? kRed : kWhite);
  }
  return size;
}

void MinUiMenu::draw_buttons(Painter& p, const Metrics& m,
                             const std::vector<std::pair<std::string, std::string>>& pairs, bool right) const {
  const int bs = m.u(Metrics::BUTTON_SIZE), margin = m.u(Metrics::BUTTON_MARGIN);
  const int tiny = m.u(Metrics::FONT_TINY), small = m.u(Metrics::FONT_SMALL), medium = m.u(Metrics::FONT_MEDIUM);
  const auto button_w = [&](const std::string& b) { return b.size() == 1 ? bs : bs / 2 + font_->width(tiny, b); };
  std::vector<int> widths;
  int ow = 0;
  for (const auto& [button, hint] : pairs) {
    const int w = button_w(button) + margin + font_->width(small, hint) + margin;
    widths.push_back(w);
    ow += margin + w;
  }
  ow += margin;
  const Canvas& c = p.canvas();
  int ox = right ? c.w - m.u(m.padding) - ow : m.u(m.padding);
  const int oy = c.h - m.u(m.padding + Metrics::PILL_SIZE);
  p.pill(ox, oy, ow, m.u(Metrics::PILL_SIZE), theme_.group_pill());
  ox += margin;
  const int by = oy + margin;
  for (size_t i = 0; i < pairs.size(); ++i) {
    const std::string& button = pairs[i].first;
    const std::string& hint = pairs[i].second;
    const int bw = button_w(button);
    if (button.size() == 1) {
      p.circle(ox + bs / 2, by + bs / 2, bs / 2, kWhite);
      font_->draw(p, ox + (bs - font_->width(medium, button)) / 2, by + (bs - font_->line_height(medium)) / 2, medium, kGray, button);
    } else {
      p.pill(ox, by, bw, bs, kWhite);
      font_->draw(p, ox + bs / 4, by + (bs - font_->line_height(tiny)) / 2, tiny, kGray, button);
    }
    font_->draw(p, ox + bw + margin, by + (bs - font_->line_height(small)) / 2, small, theme_.hint_text(), hint);
    ox += widths[i] + margin;
  }
}

const Image* MinUiMenu::thumbnail(int slot, int w, int h) const {
  const std::string path = thumb_path_ ? thumb_path_(slot) : std::string();
  if (path.empty()) return nullptr;
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) { thumbs_.erase(slot); return nullptr; }
  Thumb& t = thumbs_[slot];
  const long size = static_cast<long>(st.st_size);
  if (t.path == path && t.mtime == st.st_mtime && t.size == size && t.w == w && t.h == h)
    return t.img.empty() ? nullptr : &t.img;
  t.path = path;
  t.mtime = st.st_mtime;
  t.size = size;
  t.w = w;
  t.h = h;
  t.img = Image{};
  const Image raw = load_image(path);
  if (raw.empty()) return nullptr;
  // Fitted, not stretched: the picture is the whole panel the state was
  // saved from, which need not have the preview window's shape.
  const double k = std::min(static_cast<double>(w) / raw.w, static_cast<double>(h) / raw.h);
  t.img = scale_image(raw, std::max(1, static_cast<int>(raw.w * k)), std::max(1, static_cast<int>(raw.h * k)));
  return &t.img;
}

void MinUiMenu::draw_preview(Painter& p, const Metrics& m) const {
  const Canvas& c = p.canvas();
  const int hw = c.w / 2, hh = c.h / 2;
  const int r = m.u(Metrics::WINDOW_RADIUS);
  const int pw = hw + 2 * r;
  const int ph = hh + m.u(Metrics::WINDOW_RADIUS * 2 + Metrics::PAGINATION_HEIGHT + Metrics::WINDOW_RADIUS);
  int ox = c.w - pw - m.u(m.padding);
  int oy = (c.h - ph) / 2;
  p.rounded(ox, oy, pw, ph, r, kWhite);
  ox += r;
  oy += r;

  p.fill(ox, oy, hw, hh, kBlack);
  const bool refused = !notice_.empty() && notice_slot_ == slot_;
  const Image* img = used_[slot_] && !refused ? thumbnail(slot_, hw, hh) : nullptr;
  if (img) {
    blit(p, *img, ox + (hw - img->w) / 2, oy + (hh - img->h) / 2);
  } else {
    const std::string msg = refused ? notice_ : used_[slot_] ? "No Preview" : "Empty Slot";
    const int px = m.u(Metrics::FONT_LARGE);
    const std::string shown = font_->fit(px, msg, hw - 2 * m.u(Metrics::BUTTON_PADDING));
    font_->draw(p, ox + (hw - font_->width(px, shown)) / 2, oy + (hh - font_->line_height(px)) / 2, px, kWhite, shown);
  }

  // minarch's pagination: one dot per slot, the current one large and black.
  ox += (pw - m.u(15 * kRootSlots)) / 2;
  oy += hh + r;
  for (int i = 0; i < kRootSlots; ++i) {
    const int x = ox + m.u(i * 15);
    if (i == slot_) p.circle(x + m.u(3), oy + m.u(3), m.u(3), kBlack);
    else p.fill(x + m.u(2), oy + m.u(2), std::max(1, m.u(2)), std::max(1, m.u(2)), kLightGray);
  }
}

void MinUiMenu::draw_root(Painter& p, const Metrics& m) const {
  const Canvas& c = p.canvas();
  const int ow = draw_hardware(p, m);
  draw_title(p, m, title_.empty() ? std::string("DSperate") : title_, c.w - m.u(m.padding * 2) - ow);
  draw_buttons(p, m, {{"POWER", "SLEEP"}}, false);
  draw_buttons(p, m, {{"B", "BACK"}, {"A", "OKAY"}}, true);

  const std::vector<RootItem> items = root_items();
  const int n = static_cast<int>(items.size());
  const int pill = m.u(Metrics::PILL_SIZE), bp = m.u(Metrics::BUTTON_PADDING);
  const int px = m.u(Metrics::FONT_LARGE), lh = font_->line_height(px);
  const int x = m.u(m.padding);
  const int oy = (c.h - m.u(m.padding * 2) - n * pill) / 2 + m.u(m.padding);
  const int sel = std::clamp(row_, 0, n - 1);
  for (int i = 0; i < n; ++i) {
    static const char* const kLabels[] = {"Continue", "Save", "Load", "Options", "Quit"};
    const RootItem item = items[static_cast<size_t>(i)];
    const std::string label = kLabels[static_cast<int>(item)];
    const int y = oy + i * pill, ty = y + (pill - lh) / 2;
    if (i == sel) {
      p.pill(x, y, font_->width(px, label) + 2 * bp, pill, theme_.selected_pill());
      font_->draw(p, x + bp, ty, px, theme_.selected_text(), label);
      continue;
    }
    if (theme_.shadow()) font_->draw(p, x + bp + m.u(2), ty + m.u(1), px, kBlack, label);
    // Nothing to load reads dimmer, as it does in upstream's menu.
    const bool weak = item == RootItem::Load && !used_[slot_];
    font_->draw(p, x + bp, ty, px, weak ? kGray : theme_.text(), label);
  }
  const RootItem at = items[static_cast<size_t>(sel)];
  if (at == RootItem::Save || at == RootItem::Load) draw_preview(p, m);
}

std::vector<std::string> MinUiMenu::wrap(int px, const std::string& text, int max_w, int lines) const {
  std::vector<std::string> out;
  std::string rest = text;
  while (!rest.empty() && static_cast<int>(out.size()) < lines) {
    std::string take = rest;
    while (font_->width(px, take) > max_w) {
      const size_t cut = take.find_last_of(' ');
      if (cut == std::string::npos) break;
      take.resize(cut);
    }
    // The last line, or one word longer than the line, is cut instead.
    const bool last = static_cast<int>(out.size()) + 1 == lines;
    if (font_->width(px, take) > max_w || (last && take.size() < rest.size())) take = font_->fit(px, last ? rest : take, max_w);
    out.push_back(take);
    if (last || take.size() >= rest.size()) break;
    rest.erase(0, take.size());
    while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
  }
  return out;
}

// minarch's option list (Menu_options): rows a button tall in the small font
// under the title pill, centred and as wide as the widest row needs, the
// selected one in a dark pill with a white pill around its name, values in
// the tiny font at the right, arrows when there is more above or below, and
// a description at the foot. `value_w` is the widest value any row can
// take, so stepping a value never resizes the list under the cursor.
void MinUiMenu::draw_list(Painter& p, const Metrics& m, const std::string& title, const std::vector<Row>& rows,
                          int sel, int& top, const std::vector<std::string>& desc, int value_w) const {
  const Canvas& c = p.canvas();
  p.clear(theme_.nextui ? theme_.color[6] : kBlack);
  // The title has the band up to the battery, as on the root page; the
  // scroll arrow that shares the band makes way for it below.
  const int ow = draw_hardware(p, m);
  const int band_end = c.w - m.u(m.padding) - (ow ? ow + m.u(m.padding) : 0);
  const int title_end = draw_title(p, m, title, band_end - m.u(m.padding));

  const int row_h = m.u(Metrics::BUTTON_SIZE), op = m.u(Metrics::OPTION_PADDING);
  const int head = m.u(m.padding + Metrics::PILL_SIZE);
  // minarch keeps two lines of description and an arrow's row below the
  // list; a longer description takes rows from the list rather than
  // running into it.
  const int leading = m.u(12);
  const int desc_top = c.h - m.u(m.padding) - static_cast<int>(std::max<size_t>(2, desc.size())) * leading;
  const int visible = std::max(1, (desc_top - row_h - head) / row_h);
  visible_ = visible;
  const int n = static_cast<int>(rows.size());
  if (sel >= 0) {
    if (sel < top) top = sel;
    if (sel >= top + visible) top = sel - visible + 1;
  }
  top = std::clamp(top, 0, std::max(0, n - visible));

  const int small = m.u(Metrics::FONT_SMALL), tiny = m.u(Metrics::FONT_TINY);
  int mw = 0;
  for (const Row& r : rows) {
    const int vw = std::max(value_w, r.value.empty() ? 0 : font_->width(tiny, r.value));
    mw = std::max(mw, font_->width(small, r.label) + (vw ? vw + 4 * op : 2 * op));
  }
  mw = std::min(mw, c.w - m.u(m.padding * 2));
  const int ox = (c.w - mw) / 2;
  for (int j = 0; j < visible && top + j < n; ++j) {
    const Row& r = rows[static_cast<size_t>(top + j)];
    const int y = head + j * row_h;
    const bool is_sel = top + j == sel;
    const int vw = r.value.empty() ? 0 : font_->width(tiny, r.value);
    const int avail = mw - 2 * op - (vw ? vw + op : 0);
    const std::string label = font_->fit(small, r.label, std::max(0, avail));
    u32 ink = r.kind == Row::Item && r.enabled ? theme_.text() : kGray;
    if (is_sel) {
      p.pill(ox, y, mw, row_h, theme_.option_row());
      p.pill(ox, y, font_->width(small, label) + 2 * op, row_h, theme_.selected_pill());
      ink = theme_.selected_text();
    }
    font_->draw(p, ox + op, y + (row_h - font_->line_height(small)) / 2, small, ink, label);
    if (vw)
      font_->draw(p, ox + mw - vw - op, y + (row_h - font_->line_height(tiny)) / 2, tiny,
                  is_sel ? kWhite : (r.enabled ? theme_.text() : kGray), r.value);
  }

  // Up and down arrows, centred, as minarch's ASSET_SCROLL_UP/DOWN: in the
  // title's band above the list, and in the row's worth of room below it.
  // minarch has no title on these pages; ours can reach the centre, and then
  // the up arrow sits in what is left of the band between it and the battery.
  const double aw = m.u(24), ah = std::max(2, m.u(Metrics::SCROLL_HEIGHT));
  const double ax = (c.w - aw) / 2.0;
  if (top > 0) {
    const double ay = m.u(m.padding) + (m.u(Metrics::PILL_SIZE) - ah) / 2;
    const double gap = m.u(4), lo = title_end + gap, hi = band_end - gap;
    double x = ax, w = aw;
    if (x < lo) {
      w = std::min(aw, hi - lo);
      x = lo + (hi - lo - w) / 2;
    }
    if (w >= m.u(8)) fill_triangle(p, x, ay + ah, x + w, ay + ah, x + w / 2, ay, kGray);
  }
  if (top + visible < n) {
    const double ay = head + visible * row_h + (row_h - ah) / 2;
    fill_triangle(p, ax, ay, ax + aw, ay, ax + aw / 2, ay + ah, kGray);
  }

  if (desc.empty()) return;
  int y = c.h - m.u(m.padding) - static_cast<int>(desc.size()) * leading;
  for (const std::string& line : desc) {
    const std::string shown = font_->fit(tiny, line, c.w - m.u(m.padding * 2));
    font_->draw(p, (c.w - font_->width(tiny, shown)) / 2, y, tiny, theme_.hint_text(), shown);
    y += leading;
  }
}

void MinUiMenu::draw_options(Painter& p, const Metrics& m) const {
  static const char* const kNames[] = {"Emulation", "Visual FX", "Layout", "Controls", "DS Options",
                                       "Cheats", "Achievements", "Manage Save Slots", "Save Changes", "Reset Game",
                                       "Shortcuts"};
  const std::vector<Opt> opts = options();
  std::vector<Row> rows;
  for (Opt o : opts) {
    Row r;
    r.label = kNames[static_cast<int>(o)];
    if (o == Opt::SaveChanges) r.value = host_->save_per_game() ? "This Game" : "Global";
    rows.push_back(r);
  }
  const int sel = opts.empty() ? -1 : std::clamp(opt_row_, 0, static_cast<int>(opts.size()) - 1);
  std::vector<std::string> desc;
  const int tiny = m.u(Metrics::FONT_TINY), w = p.canvas().w - m.u(m.padding * 2);
  if (sel >= 0 && opts[static_cast<size_t>(sel)] == Opt::SaveChanges)
    desc = wrap(tiny, host_->save_per_game() ? "Changes made in these pages are saved for this game only."
                                             : "Changes made in these pages are saved for every game.", w, 2);
  else if (sel >= 0 && opts[static_cast<size_t>(sel)] == Opt::Reset)
    desc = wrap(tiny, "Restarts the game. Anything not saved is lost.", w, 2);
  draw_list(p, m, "Options", rows, sel, opt_top_, desc);
}

void MinUiMenu::draw_settings(Painter& p, const Metrics& m) const {
  const int n = settings_rows();
  const int tiny = m.u(Metrics::FONT_TINY);
  std::vector<Row> rows;
  // The widest value any row could show, as minarch measures every choice,
  // so the list keeps its width while values are stepped.
  int value_w = 0;
  for (int i = 0; i < n; ++i) {
    const Setting* row = setting_row(i);
    Row r;
    if (!row) { r.label = kShadersLink; rows.push_back(r); continue; }   // the Shaders page's link
    const Setting& s = *row;
    r.label = s.label;
    r.enabled = host_->enabled(s);
    r.value = r.enabled ? display_value(s, host_->get(s.key)) : "--";
    rows.push_back(r);
    for (int k = 0; k < s.nchoices; ++k) value_w = std::max(value_w, font_->width(tiny, display_value(s, s.choices[k].value)));
    if (s.type == Setting::Type::Int || s.type == Setting::Type::Percent) {
      value_w = std::max(value_w, font_->width(tiny, display_value(s, std::to_string(s.hi))));
      if (s.sentinel_label) value_w = std::max(value_w, font_->width(tiny, s.sentinel_label));
    }
  }
  const int sel = n > 0 ? std::clamp(set_row_[set_table_], 0, n - 1) : -1;
  std::vector<std::string> desc;
  const int w = p.canvas().w - m.u(m.padding * 2);
  if (sel >= 0 && !setting_row(sel)) {
    desc = wrap(tiny, "COLOUR, SCALING, FILTER, PATTERN, GLOW AND CURVE ONE BY ONE. A CHANGE MAKES THE PRESET CUSTOM", w, 2);
  } else if (sel >= 0) {
    const Setting& cur = *setting_row(sel);
    const char* why = host_->disabled_reason(cur);
    const char* line = why && *why ? why : cur.note;
    // What it takes to see a change, as upstream's page says it.
    std::string when;
    if (host_->enabled(cur)) {
      if (slow_on_device(cur.key, host_->get(cur.key))) when = "SLOW ON THIS DEVICE";
      else if (cur.flags & FlagRestart) when = "RESTART REQUIRED";
      else if (cur.flags & FlagDeferred) when = "APPLIED WHEN THE MENU CLOSES";
      if (!when.empty() && set_table_ == 3)
        if (const char* src = host_->user_settings_note()) { when += " - "; when += src; }
    }
    if (line && *line) desc = wrap(tiny, line, w, 2);
    if (!when.empty()) desc.push_back(when);
  }
  draw_list(p, m, kSettingsTitles[std::clamp(set_table_, 0, kShaderTable)], rows, sel, set_top_[set_table_], desc, value_w);
}

void MinUiMenu::draw_controls(Painter& p, const Metrics& m) const {
  const std::vector<int> shown = control_rows();
  const int n = static_cast<int>(shown.size());
  const int sel = n > 0 ? std::clamp(bind_row_, 0, n - 1) : -1;
  const bool listening = host_->capturing();
  std::vector<Row> rows;
  for (int i = 0; i < n; ++i) {
    const SettingsHost::Binding b = host_->binding(bind_pad_, shown[static_cast<size_t>(i)]);
    Row r;
    r.label = b.label;
    const std::string value = bind_pad_ ? controls_label(b.value) : b.value;
    r.value = i == sel && listening ? "Press any..." : (value.empty() || value == "none" ? "--" : value);
    rows.push_back(r);
  }
  std::vector<std::string> desc;
  if (listening) desc.push_back("Press the control to bind, or Escape to cancel");
  else if (reset_armed_) desc.push_back("X again resets the whole column");
  else desc.push_back("A Bind   Y Clear   X Defaults");
  const std::vector<std::string> clash = host_->collisions();
  if (!clash.empty()) desc.push_back(clash[0]);
  else if (!listening) desc.push_back(host_->has_pad() ? "L/R: keyboard or pad" : "L/R: swap column");
  draw_list(p, m, bind_pad_ ? "Controls: Pad" : "Controls: Keyboard", rows, sel, bind_top_, desc);
}

void MinUiMenu::draw_shortcuts(Painter& p, const Metrics& m) const {
  const int tiny = m.u(Metrics::FONT_TINY), w = p.canvas().w - m.u(m.padding * 2);
  const std::string mod = button_label(modifier());
  const bool listening = host_->capturing();
  const int sel = std::clamp(short_row_, 0, kShortcutCount);
  std::vector<Row> rows;
  Row head;
  head.label = "Modifier";
  head.value = mod;
  rows.push_back(head);
  // Wide enough for any modifier and the prompt, so stepping and listening
  // never resize the list.
  int value_w = 0;
  for (const std::string& c : modifier_choices()) value_w = std::max(value_w, font_->width(tiny, button_label(c)));
  for (const char* v : {"NONE", "Press any..."}) value_w = std::max(value_w, font_->width(tiny, v));
  for (int i = 0; i < kShortcutCount; ++i) {
    Row r;
    r.label = kShortcuts[i].label;
    r.value = i + 1 == sel && listening ? "Press any..." : binding_label(shortcut_value(i));
    value_w = std::max(value_w, font_->width(tiny, r.value));
    rows.push_back(r);
  }
  std::vector<std::string> desc;
  if (listening) desc = wrap(tiny, "Press a button, or hold " + mod + " and press one", w, 2);
  else if (short_armed_) desc.push_back("Y again restores this device's shortcuts");
  else if (short_refused_) desc = wrap(tiny, "Pause menu or its alternate must stay bound: nothing else opens this menu.", w, 2);
  else if (sel == 0) {
    bool ds = false;
    for (const Modifier& k : kModifiers) if (modifier() == k.name) ds = k.ds_button;
    desc = wrap(tiny, ds ? mod + " is a DS button too: the game only gets it as a tap, and not while a tap opens this menu."
                         : "Held with a second button for every shortcut below.", w, 2);
  } else {
    desc.push_back("A Set   X Clear   Y Defaults");
  }
  const std::vector<std::string> clash = host_->collisions();
  if (!clash.empty()) desc.push_back(clash[0]);
  else if (sel > 0 && !listening && !short_armed_ && !short_refused_) desc.push_back("A single button, or " + mod + "+button");
  draw_list(p, m, "Shortcuts", rows, sel, short_top_, desc, value_w);
}

// ---------------------------------------------------------------------------
// The notice

void MinUiMenu::notify(const std::string& text, u32 now_ms) {
  if (!active_) return;
  toast_ = text;
  toast_ms_ = now_ms;
}

bool MinUiMenu::notice_on(u32 now_ms) const {
  return active_ && !toast_.empty() && now_ms - toast_ms_ < kNoticeHoldMs + kNoticeFadeMs;
}

Rect MinUiMenu::draw_notice(const Canvas& d, u32 now_ms) const {
  if (!notice_on(now_ms) || d.w <= 0 || d.h <= 0 || !d.px) return {};
  const u32 age = now_ms - toast_ms_;
  const u32 alpha = age <= kNoticeHoldMs ? 255 : 255 * (kNoticeHoldMs + kNoticeFadeMs - age) / kNoticeFadeMs;
  Painter p(d);
  p.set_alpha(alpha);
  const Metrics m = Metrics::for_canvas(d.w, d.h);
  const int px = m.u(Metrics::FONT_LARGE), bp = m.u(Metrics::BUTTON_PADDING), h = m.u(Metrics::PILL_SIZE);
  const int max_w = d.w - m.u(m.padding * 2);
  if (max_w <= 2 * bp) return {};
  // The title pill's look, centred at the top: clear of upstream's slot
  // label in the top left corner and its FPS counter in the top right.
  const std::string shown = font_->fit(px, toast_, max_w - 2 * bp);
  const int w = std::min(max_w, font_->width(px, shown) + 2 * bp);
  const int x = (d.w - w) / 2, y = m.u(m.padding);
  p.pill(x, y, w, h, theme_.title_pill());
  font_->draw(p, x + bp, y + (h - font_->line_height(px)) / 2, px, theme_.title_text(), shown);
  const int x0 = std::max(0, x), y0 = std::max(0, y);
  return Rect{x0, y0, std::min(d.w, x + w) - x0, std::min(d.h, y + h) - y0};
}

bool MinUiMenu::slow_on_device(const char* key, const std::string& value) {
  const char* list = std::getenv("MINUI_SLOW_SHADERS");
  if (!list || !*list || value.empty() || std::strncmp(key, "video.", 6) != 0) return false;
  const std::string want = std::string(key + 6) + ":" + value;
  std::istringstream in(list);
  for (std::string word; in >> word;) if (word == want) return true;
  return false;
}

std::string MinUiMenu::setting_notice(const char* key, const std::string& value) {
  for (const Setting* t : {kEmuSettings, kVideoSettings, kLayoutSettings, kUserSettings, kShaderSettings})
    for (int i = 0, n = settings_count(t); i < n; ++i)
      if (std::strcmp(t[i].key, key) == 0) return std::string(t[i].label) + ": " + display_value(t[i], value);
  return value;
}

void MinUiMenu::draw_cheats(Painter& p, const Metrics& m) const {
  std::vector<Row> rows;
  size_t on = 0;
  if (codes_) for (const cheat::Code& c : *codes_) if (c.enabled) ++on;
  for (const Line& l : lines_) {
    Row r;
    if (l.kind == Line::Heading) {
      r.label = (*groups_)[static_cast<size_t>(l.at)].name;
      r.kind = Row::Heading;
    } else {
      const cheat::Code& c = (*codes_)[static_cast<size_t>(l.at)];
      r.label = c.name;
      if (l.kind == Line::Note) r.kind = Row::Note;
      else r.value = c.enabled ? "On" : "Off";
    }
    rows.push_back(r);
  }
  const bool any = !lines_.empty() && lines_[static_cast<size_t>(std::clamp(cheat_row_, 0, static_cast<int>(lines_.size()) - 1))].kind == Line::Toggle;
  const std::string title = on ? "Cheats  " + std::to_string(on) + " on" : std::string("Cheats");
  std::vector<std::string> desc;
  if (lines_.empty()) desc.push_back("No cheats for this game");
  draw_list(p, m, title, rows, any ? cheat_row_ : -1, cheat_top_, desc);
}

void MinUiMenu::draw_account(Painter& p, const Metrics& m) const {
  static const char* const kLabels[] = {"Sign In", "Sign Out", "View Achievements", "Unlock Notices",
                                        "Screenshot On Unlock", "Encore (Next Launch)"};
  std::vector<Row> rows;
  const std::string prog = cheevos_ ? cheevos_->progress() : std::string();
  if (!prog.empty()) rows.push_back(Row{prog, "", Row::Heading, true});
  const int first = static_cast<int>(rows.size());
  const std::vector<AccountRow> acc = account_rows();
  for (AccountRow a : acc) {
    Row r;
    r.label = kLabels[static_cast<int>(a)];
    if (a == AccountRow::Toasts) r.value = cheevos_->option(CheevosHost::Option::Toasts) ? "On" : "Off";
    if (a == AccountRow::Screenshot) r.value = cheevos_->option(CheevosHost::Option::Screenshot) ? "On" : "Off";
    if (a == AccountRow::Encore) r.value = cheevos_->option(CheevosHost::Option::Encore) ? "On" : "Off";
    rows.push_back(r);
  }
  const int sel = acc.empty() ? -1 : first + std::clamp(account_row_, 0, static_cast<int>(acc.size()) - 1);
  const std::string status = cheevos_ ? cheevos_->status() : std::string();
  const std::vector<std::string> desc = wrap(m.u(Metrics::FONT_TINY), status, p.canvas().w - m.u(m.padding * 2), 2);
  draw_list(p, m, "RetroAchievements", rows, sel, account_top_, desc);
}

void MinUiMenu::draw_cheevos(Painter& p, const Metrics& m) const {
  const int n = cheevos_ ? cheevos_->row_count() : 0;
  std::vector<Row> rows;
  int unlocked = 0;
  for (int i = 0; i < n; ++i) {
    const CheevosHost::Row a = cheevos_->row(i);
    if (a.unlocked) ++unlocked;
    Row r;
    r.label = a.title;
    if (a.points) r.label += "  " + std::to_string(a.points) + "p";
    r.value = a.unsupported ? "Unsupported" : a.unlocked ? "Unlocked" : "";
    // Not something the player can do anything about, so it reads dimmest.
    r.enabled = !a.unsupported;
    rows.push_back(r);
  }
  const int sel = n > 0 ? std::clamp(cheevos_row_, 0, n - 1) : -1;
  std::vector<std::string> desc;
  if (n == 0) desc.push_back("No achievements loaded");
  else {
    const CheevosHost::Row a = cheevos_->row(sel);
    const std::string detail = a.unsupported ? "Not supported by this emulator yet" : a.detail;
    desc = wrap(m.u(Metrics::FONT_TINY), detail, p.canvas().w - m.u(m.padding * 2), 2);
  }
  const std::string title = n ? "Achievements  " + std::to_string(unlocked) + "/" + std::to_string(n) : std::string("Achievements");
  draw_list(p, m, title, rows, sel, cheevos_top_, desc);
}

void MinUiMenu::draw_slots(Painter& p, const Metrics& m) const {
  std::vector<Row> rows;
  for (int i = 0; i <= kAutoSlot; ++i) {
    Row r;
    r.label = i == kAutoSlot ? std::string("Auto") : "Slot " + std::to_string(i);
    r.value = slots_armed_ && i == slots_row_ ? "Sure?" : used_[i] ? "Used" : "Empty";
    r.enabled = used_[i];
    rows.push_back(r);
  }
  std::vector<std::string> desc;
  desc.push_back(slots_armed_ ? "A again deletes this state" : "A Delete   B Back");
  draw_list(p, m, "Manage Save Slots", rows, slots_row_, slots_top_, desc);
}

void MinUiMenu::draw_text_edit(Painter& p, const Metrics& m) const {
  const Canvas& c = p.canvas();
  p.clear(theme_.nextui ? theme_.color[6] : kBlack);
  const int ow = draw_hardware(p, m);
  draw_title(p, m, edit_label_, c.w - m.u(m.padding * 2) - (ow ? ow + m.u(m.padding) : 0));

  // A long field wraps over two lines, as the console's own settings do.
  const int cols = std::max(1, edit_max_ > kEditCols ? (edit_max_ + 1) / 2 : edit_max_);
  const int lines = (std::max(1, edit_max_) + cols - 1) / cols;
  int px = m.u(Metrics::FONT_LARGE);
  int cell = font_->width(px, "W") + m.u(4);
  while (px > 6 && cols * cell > c.w - m.u(m.padding * 2)) {
    --px;
    cell = font_->width(px, "W") + m.u(4);
  }
  const int lh = font_->line_height(px), row_h = lh + m.u(8);
  const int x0 = (c.w - cols * cell) / 2;
  const int y0 = (c.h - lines * row_h) / 2 - m.u(10);
  for (int i = 0; i < static_cast<int>(edit_buf_.size()); ++i) {
    const int cx = x0 + (i % cols) * cell, cy = y0 + (i / cols) * row_h;
    const bool at = i == edit_pos_;
    if (at) p.rounded(cx, cy, cell, row_h, m.u(6), theme_.selected_pill());
    // A password shows only the character being chosen. A space is not
    // masked: it is a slot nothing was chosen for, not a character.
    const char ch = edit_buf_[static_cast<size_t>(i)];
    const bool mask = edit_dest_ == EditDest::CheevosPassword && !at && ch != ' ';
    const std::string one(1, mask ? '*' : ch);
    font_->draw(p, cx + (cell - font_->width(px, one)) / 2, cy + (row_h - lh) / 2, px,
                at ? theme_.selected_text() : theme_.text(), one, true);
  }
  const int small = m.u(Metrics::FONT_SMALL);
  const std::string set = kCharTableNames[edit_table_];
  font_->draw(p, (c.w - font_->width(small, set)) / 2, y0 + lines * row_h + m.u(8), small, kGray, set);

  const int tiny = m.u(Metrics::FONT_TINY), leading = m.u(12);
  const char* const help[] = {"Up/Down: letter   L/R: letter set", "Left/Right: move   A: done   B: cancel"};
  int y = c.h - m.u(m.padding) - 2 * leading;
  for (const char* line : help) {
    const std::string shown = font_->fit(tiny, line, c.w - m.u(m.padding * 2));
    font_->draw(p, (c.w - font_->width(tiny, shown)) / 2, y, tiny, theme_.hint_text(), shown);
    y += leading;
  }
}

} // namespace ds::sdl
