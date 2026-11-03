// SPDX-License-Identifier: GPL-3.0-or-later
//
// The C face of the DS radio for the DraStic hook. DSperate's frontend runs
// the same pieces from its frame loop (src/frontend/sdl/main.cpp): a LanMp
// session, attached to the Wi-Fi block once it is live, serviced once a frame.
// A guest that starts before its host keeps re-trying the join from the frame
// loop (DSP.pak's patch 0003), and a transport attached while the radio is
// already on announces itself (patch 0006): both orderings are ordinary under
// Netplay, which starts the two devices independently.
#include "drastic_wifi_radio.h"

#include "lan_mp.h"
#include "wifi.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

using ds::u64;

struct Radio {
  ds::io::WifiHost host;
  std::unique_ptr<ds::io::Wifi> wifi;
  std::vector<ds::u8> firmware;
  std::unique_ptr<ds::net::LanMp> lan;
  bool want_host = false;
  std::string peer, name;
  int timeout_ms = 0;
  bool started = false, live = false, join_pending = false;
  std::chrono::steady_clock::time_point next_join{};
  u64 frames = 0;
  // pacing window
  std::chrono::steady_clock::time_point window_start{};
  unsigned last_waits = 0, last_timeouts = 0;
  double last_wait_ms = 0;
};

Radio* radio;

using Clock = std::chrono::steady_clock;

void go_live() {
  radio->live = true;
  radio->join_pending = false;
  radio->wifi->set_transport(radio->lan.get());
  std::fprintf(stderr, "lan: %s, player %d\n", radio->lan->is_host() ? "hosting" : "joined", radio->lan->my_id());
}

void start_session() {
  radio->started = true;
  radio->lan = std::make_unique<ds::net::LanMp>();
  if (!radio->lan->ok()) {
    std::fprintf(stderr, "lan: %s\n", radio->lan->error().c_str());
    radio->lan.reset();
    return;
  }
  if (radio->timeout_ms > 0) {
    radio->lan->set_recv_timeout_ms(radio->timeout_ms);
    std::fprintf(stderr, "lan: MP waits bounded at %d ms\n", radio->lan->recv_timeout_ms());
  }
  if (radio->want_host) {
    if (radio->lan->start_host(radio->name, 16)) go_live();
    else { std::fprintf(stderr, "lan: %s\n", radio->lan->error().c_str()); radio->lan.reset(); }
    return;
  }
  // The host's emulator may not be up yet: keep the attempt going from the
  // frame loop rather than blocking DraStic's start.
  if (radio->lan->begin_join(radio->name, radio->peer) && radio->lan->step_join(0) == 1) { go_live(); return; }
  radio->join_pending = true;
  std::fprintf(stderr, "lan: %s not answering yet; still trying\n", radio->peer.c_str());
}

void step_join() {
  ds::net::LanMp& lan = *radio->lan;
  if (!lan.joining()) {
    if (Clock::now() >= radio->next_join && !lan.begin_join(radio->name, radio->peer))
      radio->next_join = Clock::now() + std::chrono::seconds(1);
    return;
  }
  const int r = lan.step_join(0);
  if (r > 0) go_live();
  else if (r < 0) radio->next_join = Clock::now() + std::chrono::seconds(1);
}

// Every 600 emulated frames, the same line DSperate writes under DS_VERBOSE,
// so testing/devbridge/device/check.sh judges either emulator's log.
void pacing_log() {
  const auto now = Clock::now();
  ds::net::LanMp& lan = *radio->lan;
  if (radio->window_start != Clock::time_point{}) {
    const unsigned wall = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::milliseconds>(now - radio->window_start).count());
    std::fprintf(stderr, "lan pacing: 600 frames in %ums (%.1f fps), %u waits, %.1f ms waiting, %u timeouts (max %.1f ms so far), radios %04x\n",
                 wall, wall ? 600000.0 / wall : 0.0, lan.wait_count() - radio->last_waits, lan.wait_total_ms() - radio->last_wait_ms,
                 lan.wait_timeouts() - radio->last_timeouts, lan.wait_max_ms(), lan.connected_mask());
  }
  radio->window_start = now;
  radio->last_waits = lan.wait_count();
  radio->last_wait_ms = lan.wait_total_ms();
  radio->last_timeouts = lan.wait_timeouts();
}

} // namespace

extern "C" {

int dwr_init(const struct dwr_config* config) {
  if (radio || !config || (!config->host && (!config->peer || !*config->peer))) return -1;
  radio = new Radio;
  radio->want_host = config->host != 0;
  radio->peer = config->peer ? config->peer : "";
  radio->name = config->name && *config->name ? config->name : "DraStic";
  radio->timeout_ms = config->timeout_ms;
  if (config->firmware && config->firmware_size)
    radio->firmware.assign(config->firmware, config->firmware + config->firmware_size);
  radio->host.firmware = radio->firmware.empty() ? nullptr : radio->firmware.data();
  radio->host.firmware_size = radio->firmware.size();
  radio->wifi = std::make_unique<ds::io::Wifi>(radio->host);
  radio->wifi->reset();
  return 0;
}

void dwr_bind(void* opaque, uint8_t* frame_ram, int (*powcnt2_on)(void*), void (*raise_irq)(void*)) {
  if (!radio) return;
  radio->host.opaque = opaque;
  radio->host.powcnt2_on = powcnt2_on;
  radio->host.raise_irq = raise_irq;
  radio->wifi->set_host(radio->host);
  radio->wifi->set_ram(frame_ram);
}

uint16_t dwr_read16(uint32_t address) { return radio ? radio->wifi->read16(address) : 0xFFFF; }
void dwr_write16(uint32_t address, uint16_t value) { if (radio) radio->wifi->write16(address, value); }
void dwr_advance_us(uint64_t elapsed_us) { if (radio) radio->wifi->advance_us(elapsed_us); }

void dwr_frame(void) {
  if (!radio) return;
  if (!radio->started) start_session();
  if (!radio->lan) return;
  radio->lan->process();
  if (radio->join_pending) step_join();
  if (radio->live && ++radio->frames % 600 == 0) pacing_log();
}

void dwr_shutdown(void) {
  if (!radio) return;
  if (radio->lan) {
    std::fprintf(stderr, "lan: session over: %u MP waits, %.1f ms waiting, %u timeouts, %u frames on the air\n",
                 radio->lan->wait_count(), radio->lan->wait_total_ms(), radio->lan->wait_timeouts(), radio->wifi->tx_frames());
    radio->wifi->set_transport(nullptr);
    radio->lan->cancel_join();
    radio->lan->end_session();
  }
}

} // extern "C"
