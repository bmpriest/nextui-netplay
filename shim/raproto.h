/*
 * raproto - RetroArch's netplay wire protocol (versions 5 to 7, as spoken by
 * RetroArch 1.22.x), as a two-player peer for the shim.
 *
 * Netplay's own protocol is netlink.c. This is a second transport the shim
 * selects when the other device runs stock RetroArch netplay (spruce, Onion,
 * Allium, muOS, ROCKNIX, Knulli). The shim stays a lockstep peer: it never
 * rolls back. RetroArch's rollback is local to each RetroArch and only needs
 * every player's input for every frame, in order, which is exactly what a
 * lockstep peer sends. The design and the RetroArch rules it has to satisfy
 * are in docs/analysis/retroarch-protocol.md.
 *
 * Threads. A worker thread owns the socket's receive side: it connects or
 * accepts, runs the header/NICK/INFO handshake, then decodes commands into a
 * remote-input ring and an ordered event queue. The emulator thread drains the
 * queue at frame boundaries and sends everything that has a position in the
 * frame stream (SYNC, INPUT, MODE, LOAD_SAVESTATE, CRC, RESET), so the order
 * of those relative to INPUT is decided by one thread. The worker also sends
 * replies that have no frame position (PING).
 */

#ifndef RAPROTO_H
#define RAPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAPROTO_DEFAULT_PORT 55435
#define RAPROTO_NICK_LEN     32
#define RAPROTO_MAX_DEVICES  16
#define RAPROTO_MAX_STATE    (16u * 1024u * 1024u)

/* The RetroArch release whose implementation magic we present. A mismatch
 * is only a warning in RetroArch (netplay_handshake_init), but presenting
 * the version the peer runs keeps its log and notifications quiet. */
#define RAPROTO_DEFAULT_RA_VERSION "1.22.2"

typedef enum {
	RAPROTO_ROLE_NONE = 0,
	RAPROTO_ROLE_HOST,   /* RetroArch "server": we listen, clients join */
	RAPROTO_ROLE_CLIENT, /* we connect to a RetroArch host */
} RaProtoRole;

typedef struct {
	RaProtoRole role;
	char peer[64];        /* client only */
	uint16_t port;        /* 0 = 55435 */
	char nick[RAPROTO_NICK_LEN];
	/* The core's own retro_system_info strings. RetroArch refuses a peer
	 * whose core name differs (case-insensitively) and only warns on version. */
	char core_name[RAPROTO_NICK_LEN];
	char core_version[RAPROTO_NICK_LEN];
	/* CRC32 of the content, as RetroArch's content_get_crc() computes it.
	 * A mismatch is a warning only. */
	uint32_t content_crc;
	const char* ra_version; /* NULL = RAPROTO_DEFAULT_RA_VERSION */
	/* Host settings sent after SYNC (protocol 6+). */
	bool allow_pausing;
	int32_t latency_min, latency_max;
	/* Test hook: highest protocol to offer or accept (0 = 7). */
	uint32_t max_protocol;
} RaProtoConfig;

typedef enum {
	RAPROTO_EV_NONE = 0,
	/* Host: a client finished header/NICK/INFO. The emulator thread answers
	 * with RaProto_sendSync at a frame boundary. text = the client's nick. */
	RAPROTO_EV_CLIENT_READY,
	/* Client: the host's SYNC. frame = the host's frame counter, a = our
	 * client number, data/len = the host's SRAM (may be empty). */
	RAPROTO_EV_SYNC,
	/* MODE: frame, a = mode word (RAPROTO_MODE_* bits | client number),
	 * b = devices bitmask. text = nick. */
	RAPROTO_EV_MODE,
	/* Client: the host refused our PLAY. a = reason. */
	RAPROTO_EV_MODE_REFUSED,
	/* Host: the client asked to play. a = PLAY payload (devices | share<<16). */
	RAPROTO_EV_PLAY,
	/* Host: the client went back to spectating. */
	RAPROTO_EV_SPECTATE,
	/* Client: LOAD_SAVESTATE for frame. data/len = the core's serialized
	 * bytes, already decompressed and taken out of RetroArch's container. */
	RAPROTO_EV_STATE,
	/* Client: RESET at frame. */
	RAPROTO_EV_RESET,
	/* CRC of the core state before frame runs. a = crc. */
	RAPROTO_EV_CRC,
	/* Host: the client asked for a savestate. */
	RAPROTO_EV_REQUEST_STATE,
	RAPROTO_EV_PAUSE,   /* text = who */
	RAPROTO_EV_RESUME,
	/* Client: the host asked us to stall a = frames. */
	RAPROTO_EV_STALL,
	/* The connection ended. text = why. A host keeps listening. */
	RAPROTO_EV_DISCONNECT,
} RaProtoEventType;

#define RAPROTO_MODE_YOU     (1u << 31)
#define RAPROTO_MODE_PLAYING (1u << 30)
#define RAPROTO_MODE_SLAVE   (1u << 29)

typedef struct {
	RaProtoEventType type;
	uint32_t frame;
	uint32_t a, b;
	void* data;    /* caller frees */
	size_t len;
	char text[64];
} RaProtoEvent;

/* Pure helpers, exposed for tests. */
uint32_t RaProto_implMagic(const char* ra_version, uint32_t protocol);
uint32_t RaProto_platformMagic(void);
/* Words of INPUT payload a device type takes (RetroArch's
 * netplay_expected_input_size): joypad 1, mouse/lightgun 2, analog 3,
 * keyboard 5, anything else 0. */
uint32_t RaProto_deviceWords(uint32_t device);
/* RetroArch's savestate container (protocol 7): "NETPLAY1", a MEM block
 * holding the core state, an END block. Build returns a malloc'd buffer.
 * Unwrap finds the MEM block in a container, or returns the input unchanged
 * when it has no container (protocols 5 and 6 send raw core state). */
void* RaProto_buildContainer(const void* core_state, size_t len, size_t* out_len);
bool RaProto_unwrapContainer(const void* data, size_t len, const void** core, size_t* core_len);

bool RaProto_start(const RaProtoConfig* config);
void RaProto_stop(void);

bool RaProto_isConnected(void);  /* handshake complete, peer present */
uint32_t RaProto_protocol(void); /* negotiated, 0 before the header */
uint32_t RaProto_selfClient(void); /* 0 when hosting */
/* RetroArch's per-port device types, from SYNC (client) or ours (host). */
uint32_t RaProto_configDevice(unsigned port);
bool RaProto_hostAllowsPausing(void);
const char* RaProto_peerNick(void);
/* Round trip from PING_REQUEST to PING_RESPONSE, ms; -1 before one. */
int32_t RaProto_pingMs(void);

/* Oldest event first. False when the queue is empty. */
bool RaProto_pollEvent(RaProtoEvent* out);
void RaProto_freeEvent(RaProtoEvent* ev);

/* The peer's input, by frame. As a client the peer is the host (client 0),
 * whose NOINPUT frames read as 0. As a host it is client 1. */
bool RaProto_getRemoteInput(uint32_t frame, uint32_t* buttons);
/* The first frame whose peer input has not arrived. */
uint32_t RaProto_remoteHorizon(void);
/* Expect the peer's input stream to continue at frame (MODE, a fresh SYNC). */
void RaProto_expectRemoteFrom(uint32_t frame);

/* --- sent by the emulator thread, in frame-stream order ----------------- */

/* Our INPUT for frame. devices = the devices we own (MODE's bitmask); one
 * joypad word per device, padded to that device type's size. */
bool RaProto_sendInput(uint32_t frame, uint32_t devices, uint32_t buttons);
/* Host: SYNC + settings. client_devices = device bitmask per client, as
 * RetroArch's device_clients (bit c set for client c on device i). */
bool RaProto_sendSync(uint32_t frame, const uint32_t device_clients[RAPROTO_MAX_DEVICES],
                      const void* sram, size_t sram_len);
bool RaProto_sendMode(uint32_t frame, uint32_t mode_word, uint32_t devices, const char* nick);
bool RaProto_sendModeRefused(uint32_t reason);
/* Host: core state for frame, wrapped (protocol 7) and compressed as
 * negotiated. */
bool RaProto_sendState(uint32_t frame, const void* core_state, size_t len);
bool RaProto_sendCrc(uint32_t frame, uint32_t crc);
bool RaProto_sendReset(uint32_t frame);
bool RaProto_sendStall(uint32_t frames);
bool RaProto_requestState(void);

/* Called once per retro_run: turns away a second client. Frontend stalls are
 * deliberately not reported to RetroArch (no PAUSE): see raproto.c. */
void RaProto_markFrame(void);

#endif
