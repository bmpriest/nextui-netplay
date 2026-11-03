// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. Compiled into DSperate (GPLv3) by
// patches/0003-minui-integration.patch.
//
// The built-in pad's SDL GameController mapping. DSperate only takes input
// from a pad SDL calls a game controller, and neither launcher ships a mapping
// for its own pad, so the pak brings one per device (config/platform.sh).
// SDL only applies a mapping to the GUID it names, and builds that GUID from
// the pad's kernel ids differently from one SDL release to the next, so the
// GUID is read from the running SDL as DSperate starts, and put in front of
// the body launch.sh passes on. No SDL here, so the tests can reach it.
#pragma once

#include <string>

namespace ds::sdl::minui_pad {

struct Mapping {
  std::string sdl;       // for SDL_GameControllerAddMapping; empty for none
  std::string buttons;   // the controls it names, space separated: "a b x ..."
  bool fixed = false;    // NextUI's fixed h700 layout picked the other body
};

// `body` is the device's mapping without its GUID and name. NextUI h700-rc11
// and later give every H700 model the TrimUI button numbers and say so with
// `fixed_guid`, which then takes `fixed_body` instead; both are empty where
// that cannot happen. Nothing at all without a body or a GUID.
inline Mapping choose(const std::string& guid, const std::string& name, const std::string& body,
                      const std::string& fixed_body, const std::string& fixed_guid) {
  Mapping m;
  if (guid.empty() || body.empty()) return m;
  std::string b = body;
  if (!fixed_body.empty() && !fixed_guid.empty() && guid == fixed_guid) {
    b = fixed_body;
    m.fixed = true;
  }
  // A comma would end the name field early.
  std::string n;
  for (char c : name) if (c != ',') n += c;
  if (n.empty()) n = "Controller";
  m.sdl = guid + "," + n + "," + b + ",platform:Linux";
  for (size_t at = 0; at < b.size();) {
    size_t end = b.find(',', at);
    if (end == std::string::npos) end = b.size();
    const std::string field = b.substr(at, end - at);
    const std::string control = field.substr(0, field.find(':'));
    if (!control.empty()) m.buttons += (m.buttons.empty() ? "" : " ") + control;
    at = end + 1;
  }
  return m;
}

} // namespace ds::sdl::minui_pad
