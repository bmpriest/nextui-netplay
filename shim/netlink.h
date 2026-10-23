/*
 * netlink - the two-player TCP transport under the shim's netpacket interface.
 *
 * The frontend knows nothing about any of this. The core asks for a netpacket
 * interface, the shim answers, and this is what sits underneath: host listens,
 * client connects, packets are framed over TCP.
 *
 * Receiving happens on its own thread, which is what lets a link survive the
 * frontend blocking - a menu, a sleep, a long save. That is the reason the
 * patched minarch needed hooks in Menu_loop and the sleep path; here it costs
 * nothing.
 */

#ifndef NETLINK_H
#define NETLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETLINK_DEFAULT_PORT 55437
#define NETLINK_MAX_PACKET   2048

/* Upper bound on a peer-declared state size. Generous - picodrive's is 678KB -
 * but finite, so a malformed or hostile header cannot ask for a 4GB malloc. */
#define NETLINK_MAX_STATE    (16u * 1024u * 1024u)

typedef enum {
	NETLINK_ROLE_NONE = 0,
	NETLINK_ROLE_HOST,
	NETLINK_ROLE_CLIENT,
} NetLinkRole;

/* Read a session description. Returns false when no session is configured,
 * which is the normal passthrough case and not an error.
 *
 * Format is one key=value per line:
 *   role=host|client
 *   port=55437        (optional)
 *   peer=192.168.1.42 (clients only)
 */
bool NetLink_configure(const char* session_path);

bool NetLink_isConfigured(void);
NetLinkRole NetLink_getRole(void);

/* Bring the link up. Non-blocking: connecting happens on the worker thread, so
 * the first frames run before a peer is present. */
bool NetLink_start(void);
void NetLink_stop(void);

bool NetLink_isConnected(void);

/* libretro netpacket assigns the host client_id 0 and clients ids above it.
 * Two players, so the remote is simply the other one. */
uint16_t NetLink_localClientId(void);
uint16_t NetLink_remoteClientId(void);

/* Edge-triggered: each transition is reported exactly once, so the caller can
 * drive the core's start/connected and stop/disconnected callbacks. */
bool NetLink_consumeConnectEvent(void);
bool NetLink_consumeDisconnectEvent(void);

/* Increments after every successful TCP greeting. The emulator thread uses it
 * to distinguish a replacement process from the peer it originally synced. */
uint32_t NetLink_connectionGeneration(void);

bool NetLink_send(int flags, const void* buf, size_t len, uint16_t client_id);

/* Called once per frame from retro_run. When the frontend stops calling it -
 * a menu, a sleep, a long save - we tell the peer, so it can stall its own core
 * instead of running ahead and filling a buffer that nobody is draining.
 *
 * Deliberately not inferred from "no game data received": a turn-based game
 * legitimately goes quiet while a player thinks, and pausing then would be
 * wrong. Frontend liveness is the honest signal, and only we can observe it. */
void NetLink_markFrame(void);

/* Bracket the call into the core. Time spent inside it is not a frontend stall,
 * however long it lasts: a link-capable core may legitimately block there
 * waiting on its peer. Reporting that as a stall makes the peer pause, which
 * removes the very data the blocked core is waiting for - a deadlock. */
void NetLink_setCoreRunning(bool running);

/* Round-trip time over the last few seconds, in microseconds; false until a
 * probe has completed. Measured continuously on the transport thread, so it
 * reflects the link rather than either side's frame loop.
 *
 * Size an input-delay window from the maximum, not the median: the median says
 * what the link usually does, and it is the spikes that a lockstep timeline
 * actually has to survive. */
bool NetLink_rttStats(uint32_t* median_us, uint32_t* max_us, unsigned* samples);

/* Why we are asking the peer to hold. Only a frontend that has stopped calling
 * retro_run counts: a core blocked *inside* retro_run is working, not absent,
 * and pausing its peer would starve it of what it is waiting for. */
typedef enum {
	NETLINK_PAUSE_FRONTEND = 0, /* menu, sleep, long save */
} NetLinkPauseReason;

/* True while the peer has told us it is stalled. The caller should skip running
 * the core; returning from retro_run without advancing is a legal frame skip
 * and keeps the frontend responsive. */
bool NetLink_isPeerPaused(void);

/* --- shared-screen netplay ---------------------------------------------
 *
 * One instance per device running the same game in lockstep, each player
 * owning a controller port. Only inputs cross the wire, buffered a few frames
 * ahead so the peer's arrive before they are needed - which is what makes this
 * tolerate latency that a synchronous link cable cannot.
 */

/* Inputs are addressed by frame number, so a late packet is still usable and a
 * duplicate is harmless. */
bool NetLink_sendInput(uint32_t frame, uint32_t buttons);
bool NetLink_getRemoteInput(uint32_t frame, uint32_t* buttons);
/* A new connection needs the full reset. Moving to a committed authoritative
 * timeline must retain ACK/COMMIT controls which may already be in flight. */
void NetLink_resetSync(void);
void NetLink_resetTimeline(void);

/* Both sides must start from bit-identical state, so the host ships one.
 * Chunked: a save state is far larger than a packet.
 *
 * Every transfer is tagged. The instanced-link bootstrap sends two payloads
 * back to back and the consumer reads at most one per frontend frame, so a
 * finished transfer is queued rather than overwritten and a take of the wrong
 * kind returns false instead of handing over a payload that would be rejected
 * and fail the session. */
typedef enum {
	NETLINK_STATE_AUTHORITATIVE = 0, /* shared-screen core state + raw SRAM/RTC */
	NETLINK_STATE_CONSOLE_MEMORY = 1, /* one logical console's raw SRAM/RTC */
	NETLINK_STATE_PAIRED_CHECKPOINT = 2, /* both emulators + link coordinator */
} NetLinkStateKind;

bool NetLink_sendState(NetLinkStateKind kind, const void* data, size_t len);
bool NetLink_takeState(NetLinkStateKind kind, void** data, size_t* len); /* caller frees */

/* Instanced-link agreement. Each side reports whether it can host both logical
 * consoles locally; pairing proceeds only when both say yes. Reason codes are
 * NetLinkLinkReason and exist so the peer's overlay can explain the demotion. */
typedef enum {
	NETLINK_LINK_OK = 0,
	NETLINK_LINK_NO_PEER_ROM = 1,   /* peer's cartridge is not installed here */
	NETLINK_LINK_NO_PAIRED_CORE = 2,/* paired core or ABI unavailable here */
	NETLINK_LINK_CORE_MISMATCH = 3, /* builds differ */
} NetLinkLinkReason;

bool NetLink_sendLinkVerdict(bool can_pair, uint32_t reason);
bool NetLink_takeLinkVerdict(bool* can_pair, uint32_t* reason);

/* Periodic agreement check. Without it a divergence is silent and the two
 * games quietly tell different stories. */
bool NetLink_sendHash(uint32_t frame, uint32_t hash);
bool NetLink_takeHash(uint32_t* frame, uint32_t* hash);

typedef struct {
	uint8_t  rom_sha256[32];
	uint32_t mode;          /* 1 = shared-screen, 2 = instanced link */
	uint32_t input_delay;   /* timeline priming must agree exactly */
	uint32_t core_identity;
	uint32_t state_size;
	uint32_t sram_size;
	uint32_t rtc_size;
	/* Instanced link only. Linked cartridges may legitimately differ (Red/Blue,
	 * Seasons/Ages), so the peer's ROM has to be findable locally rather than
	 * merely equal to ours - and a size narrows that search to a hash or two. */
	uint32_t rom_size;
	/* CRC32 of the same uncompressed bytes rom_sha256 covers. A zipped library
	 * records this for its contents, so a peer can reject nearly every
	 * cartridge it holds without decompressing any of them; only a candidate
	 * matching both size and CRC32 is inflated, and rom_sha256 still decides. */
	uint32_t rom_crc32;
	/* This device's time of day, seconds since the Unix epoch. Two handhelds
	 * are rarely set to the same second - nine seconds apart on the pair this
	 * was found on - and a cartridge with an RTC turns that difference into
	 * emulated state as soon as a game latches it. Exchanging both clocks lets
	 * each device hand the paired core the same pair of epochs, so each
	 * cartridge shows its own owner's time while both replicas agree. */
	uint64_t wall_clock_utc;
} NetLinkSessionIdentity;

/* Sent afresh on each TCP connection, before any serialized state is accepted. */
bool NetLink_sendSessionIdentity(const NetLinkSessionIdentity* identity);
bool NetLink_takeSessionIdentity(NetLinkSessionIdentity* identity);

/* The guest confirms the result of each periodic comparison. The host only
 * promotes the candidate snapshot after a matching acknowledgement. */
bool NetLink_ackCheckpoint(uint32_t frame, uint32_t hash, bool matched);
bool NetLink_takeCheckpointAck(uint32_t* frame, uint32_t* hash, bool* matched);

/* Authoritative-state recovery. The client requests it after a confirmed hash
 * mismatch. The host brackets a state transfer with BEGIN, waits for the
 * client's load ACK, then releases both timelines with COMMIT. Epochs make a
 * delayed control packet from an abandoned attempt harmless. */
bool NetLink_requestResync(uint32_t frame);
bool NetLink_takeResyncRequest(uint32_t* frame);
typedef enum {
	NETLINK_RECOVERY_SYNC = 0,
	NETLINK_RECOVERY_RESET = 1,
} NetLinkRecoveryKind;
bool NetLink_beginResync(uint32_t epoch, uint32_t resume_frame, NetLinkRecoveryKind kind);
bool NetLink_takeResyncBegin(uint32_t* epoch, uint32_t* resume_frame, NetLinkRecoveryKind* kind);
bool NetLink_ackResync(uint32_t epoch, bool loaded);
bool NetLink_takeResyncAck(uint32_t* epoch, bool* loaded);
bool NetLink_commitResync(uint32_t epoch);
bool NetLink_takeResyncCommit(uint32_t* epoch);

/* Copy the oldest queued packet out. False when the queue is empty. */
bool NetLink_popPacket(void* out, size_t out_cap, size_t* out_len);

/* Number of packets dropped because the queue was full - a desync warning
 * sign worth logging rather than hiding. */
unsigned NetLink_droppedPackets(void);

#endif
