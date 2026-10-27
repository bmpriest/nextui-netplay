// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. See minui.h.
#include "minui.h"
#include "minui_pad.h"

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef __linux__
#include <dirent.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/input.h>
#endif

namespace ds::sdl {

namespace {

// Held this long and it is a shutdown rather than a sleep. Fires while still
// held so the user gets feedback without having to guess when to let go.
constexpr u32 LONG_PRESS_MS = 1000;
// How often the sleep loop looks for the wake press.
constexpr u32 WAKE_POLL_MS = 200;
// Light sleep first, suspend-to-RAM only after this. Two stages because the
// common case is a few seconds of hiding the screen, where a real suspend
// would cost seconds of resume and put the display pipeline at risk for no
// benefit; the panel backlight is the dominant draw and is already off.
constexpr u32 DEEP_SLEEP_MS = 120000;
// Allwinner display engine, the same call NextUI's libmsettings makes on both
// tg5040 and h700.
constexpr unsigned long DISP_LCD_SET_BRIGHTNESS = 0x102;
constexpr const char* BACKLIGHT_SYSFS = "/sys/class/backlight/backlight0/brightness";
constexpr int BACKLIGHT_FALLBACK = 200;

std::string env_str(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? std::string(v) : std::string();
}

// fputs, not fprintf with a newline: minarch reads these back with a bare
// concat and never trims, so one stray '\n' makes the launcher silently skip
// the resume. See TECHNICAL.md.
bool put_text(const std::string& path, const std::string& text) {
  if (path.empty()) return false;
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  const bool ok = std::fputs(text.c_str(), f) >= 0;
  std::fclose(f);
  return ok;
}

void run_quiet(const char* cmd) {
  // Every helper here is optional, so a missing binary must not print.
  if (std::system(cmd) != 0) { /* best effort */ }
}

} // namespace

void MinUi::open() {
  rom_path_ = env_str("MINUI_ROM_PATH");
  rom_file_ = env_str("MINUI_ROM_FILE");
  dir_ = env_str("MINUI_DIR");
  shared_dir_ = env_str("MINUI_SHARED_DIR");

#ifdef __linux__
  DIR* d = opendir("/dev/input");
  if (!d) return;
  while (dirent* e = readdir(d)) {
    if (std::strncmp(e->d_name, "event", 5) != 0) continue;
    const std::string path = std::string("/dev/input/") + e->d_name;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
    const auto has = [&](int code) {
      return ((keys[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1) != 0;
    };
    const bool got_keys = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0;
    char name[64] = "?";
    if (got_keys && fd_ < 0 && has(KEY_POWER)) {
      ioctl(fd, EVIOCGNAME(sizeof name), name);
      fd_ = fd;
      std::fprintf(stderr, "minui: power button on %s (%s)\n", path.c_str(), name);
      continue;
    }
    if (got_keys && (has(KEY_VOLUMEUP) || has(KEY_VOLUMEDOWN))) {
      ioctl(fd, EVIOCGNAME(sizeof name), name);
      volume_fds_.push_back(fd);
      std::fprintf(stderr, "minui: volume keys on %s (%s)\n", path.c_str(), name);
      continue;
    }
    ::close(fd);
  }
  closedir(d);
  if (fd_ < 0) std::fprintf(stderr, "minui: no KEY_POWER device; sleep is unavailable\n");
#endif
}

void MinUi::close() {
#ifdef __linux__
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  for (int fd : volume_fds_) ::close(fd);
  volume_fds_.clear();
#endif
}

void MinUi::add_pad_mapping() {
  const char* body = std::getenv("MINUI_PAD_MAP");
  if (!body || !*body) return;
  if (SDL_NumJoysticks() < 1) {
    std::fprintf(stderr, "pad mapping: no pad found, so there will be no controller input\n");
    return;
  }
  char guid[33] = {};
  SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(0), guid, sizeof guid);
  const char* name = SDL_JoystickNameForIndex(0);
  const char* fixed_body = std::getenv("MINUI_PAD_MAP_FIXED");
  const char* fixed_guid = std::getenv("MINUI_PAD_FIXED_GUID");
  const minui_pad::Mapping m = minui_pad::choose(guid, name ? name : "", body, fixed_body ? fixed_body : "",
                                                 fixed_guid ? fixed_guid : "");
  if (m.sdl.empty()) return;
  if (m.fixed) std::fprintf(stderr, "NextUI fixed h700 pad layout, using the TrimUI button numbers\n");
  if (SDL_GameControllerAddMapping(m.sdl.c_str()) < 0) {
    std::fprintf(stderr, "pad mapping: SDL refused it: %s\n", SDL_GetError());
    return;
  }
  std::fprintf(stderr, "pad mapping: %s (%s)\n", guid, name ? name : "Controller");
  setenv("MINUI_PAD_BUTTONS", m.buttons.c_str(), 1);
}

void MinUi::mark(const char* what) const {
  if (!active()) return;
#ifdef __linux__
  timespec ts{};
  clock_gettime(CLOCK_BOOTTIME, &ts);
  std::fprintf(stderr, "timing: %.2f %s\n", static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9, what);
#else
  std::fprintf(stderr, "timing: ? %s\n", what);
#endif
}

bool MinUi::take_volume() {
#ifdef __linux__
  // Drained here as well as in poll(): nothing else reads these nodes, and
  // a backlog would report a chord long since over.
  input_event ev;
  for (int fd : volume_fds_)
    while (read(fd, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev))
      if (ev.type == EV_KEY && (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN) && ev.value == 1) volume_seen_ = true;
#endif
  const bool seen = volume_seen_;
  volume_seen_ = false;
  return seen;
}

MinUi::Event MinUi::poll() {
#ifdef __linux__
  if (fd_ < 0) return Event::None;

  // Drain the burst; only the last transition matters.
  input_event ev;
  while (read(fd_, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev)) {
    if (ev.type == EV_KEY && (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN) && ev.value == 1) volume_seen_ = true;
    if (ev.type != EV_KEY || ev.code != KEY_POWER) continue;
    if (ev.value == 1) {                       // press (2 is autorepeat)
      down_ = true;
      pressed_at_ = SDL_GetTicks();
    } else if (ev.value == 0) {                // release
      const bool was = down_;
      down_ = false;
      if (was && pressed_at_) { pressed_at_ = 0; return Event::Sleep; }
    }
  }

  if (down_ && pressed_at_ && SDL_GetTicks() - pressed_at_ >= LONG_PRESS_MS) {
    pressed_at_ = 0;                           // one shot; the release is ignored
    return Event::PowerOff;
  }
#endif
  return Event::None;
}

void MinUi::set_backlight(int level) const {
  if (FILE* f = std::fopen(BACKLIGHT_SYSFS, "w")) {
    std::fprintf(f, "%d", level);
    std::fclose(f);
    return;
  }
#ifdef __linux__
  const int fd = ::open("/dev/disp", O_RDWR);
  if (fd >= 0) {
    unsigned long param[4] = {0, static_cast<unsigned long>(level), 0, 0};
    ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &param);
    ::close(fd);
  }
#endif
}

int MinUi::read_backlight() const {
  if (FILE* f = std::fopen(BACKLIGHT_SYSFS, "r")) {
    int v = 0;
    const bool ok = std::fscanf(f, "%d", &v) == 1;
    std::fclose(f);
    if (ok) return v;
  }
  // There is no read side to the /dev/disp ioctl, so on those panels recover
  // the level the launcher last set rather than waking to a hardcoded guess
  // the user never chose.
  if (!shared_dir_.empty()) {
    const std::string settings = shared_dir_ + "/../minuisettings.txt";
    if (FILE* f = std::fopen(settings.c_str(), "r")) {
      char line[128];
      int v = -1;
      while (std::fgets(line, sizeof line, f)) {
        if (std::sscanf(line, "brightness=%d", &v) == 1) break;
        v = -1;
      }
      std::fclose(f);
      // minuisettings stores the UI's 0..10 step, not the panel's 0..255.
      if (v >= 0 && v <= 10) return v == 0 ? 1 : v * 255 / 10;
      if (v > 10 && v <= 255) return v;
    }
  }
  return BACKLIGHT_FALLBACK;
}

void MinUi::sleep(const std::function<void()>& before, const std::function<void()>& after) {
  if (before) before();

  run_quiet("command -v gametimectl.elf >/dev/null 2>&1 && gametimectl.elf stop_all");
  // TrimUI-only node; h700 has no amp mute and simply skips this.
  run_quiet("echo 1 > /sys/class/speaker/mute 2>/dev/null");

  const int level = read_backlight();
  set_backlight(0);

  u32 since = SDL_GetTicks();
  for (;;) {
    SDL_Delay(WAKE_POLL_MS);

    bool wake = false;
#ifdef __linux__
    input_event ev;
    while (fd_ >= 0 && read(fd_, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev)) {
      if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1) wake = true;
    }
#endif
    if (wake) break;

    if (SDL_GetTicks() - since >= DEEP_SLEEP_MS) {
#ifdef __linux__
      const int fd = ::open("/sys/power/state", O_WRONLY);
      if (fd >= 0) { const ssize_t n = write(fd, "mem", 3); (void)n; ::close(fd); }
#endif
      // Control returns here once the kernel has resumed. Re-arm rather than
      // spinning: a spurious wake goes back down after another full interval.
      since = SDL_GetTicks();
    }
  }

  // Swallow the release of the press that woke us, so it is not read as a
  // fresh short press and does not put the device straight back to sleep.
  down_ = false;
  pressed_at_ = 0;
#ifdef __linux__
  for (int i = 0; i < 20; ++i) {
    SDL_Delay(50);
    bool held = false;
    input_event ev;
    while (fd_ >= 0 && read(fd_, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev)) {
      if (ev.type == EV_KEY && ev.code == KEY_POWER) held = ev.value != 0;
    }
    if (!held) break;
  }
#endif

  set_backlight(level);
  run_quiet("echo 0 > /sys/class/speaker/mute 2>/dev/null");
  run_quiet("command -v gametimectl.elf >/dev/null 2>&1 && gametimectl.elf resume");

  if (after) after();
}

void MinUi::write_auto_resume() const {
  if (shared_dir_.empty() || rom_path_.empty()) return;
  put_text(shared_dir_ + "/auto_resume.txt", rom_path_);
}

void MinUi::clear_auto_resume() const {
  if (shared_dir_.empty()) return;
  std::remove((shared_dir_ + "/auto_resume.txt").c_str());
}

void MinUi::write_slot_marker(int slot) const {
  if (dir_.empty() || rom_file_.empty()) return;
  if (slot < 0 || slot > MAX_SLOT) return;
  put_text(dir_ + "/" + rom_file_ + ".txt", std::to_string(slot));
}

void MinUi::forget_slot(int slot) const {
  const std::string thumb = slot_thumbnail_path(slot);
  if (thumb.empty()) return;
  std::remove(thumb.c_str());
  const std::string marker = dir_ + "/" + rom_file_ + ".txt";
  FILE* f = std::fopen(marker.c_str(), "r");
  if (!f) return;
  char buf[16] = {};
  const bool read = std::fgets(buf, sizeof buf, f) != nullptr;
  std::fclose(f);
  if (read && std::strtol(buf, nullptr, 10) == slot) std::remove(marker.c_str());
}

void MinUi::write_game_switcher() const {
  if (shared_dir_.empty()) return;
  // Only existence is read; the frontend writes the literal "unused" itself.
  put_text(shared_dir_ + "/game_switcher.txt", rom_path_.empty() ? "unused" : rom_path_);
}

std::string MinUi::slot_thumbnail_path(int slot) const {
  if (dir_.empty() || rom_file_.empty()) return {};
  if (slot < 0 || slot > MAX_SLOT) return {};
  return dir_ + "/" + rom_file_ + "." + std::to_string(slot) + ".bmp";
}

} // namespace ds::sdl
