/*
 * Session-lifetime host services.
 *
 * This process is deliberately independent of the setup UI. It owns the
 * lightweight discovery beacon and compatibility listener until the shared session
 * disappears or it receives SIGTERM, and publishes a small status snapshot for
 * any newly opened Netplay.pak process.
 */

#include "netsetup.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;

static void stop_signal(int sig) {
	(void)sig;
	stopping = 1;
}

static bool write_pid(const char* path) {
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) return false;
	char pid[32];
	int n = snprintf(pid, sizeof(pid), "%d\n", getpid());
	bool ok = write(fd, pid, (size_t)n) == n;
	close(fd);
	if (!ok) remove(path);
	return ok;
}

static void publish_status(const char* status_path, const NS_SessionInfo* session) {
	NS_Client clients[NS_MAX_CLIENTS];
	int count = NS_hotspotClients(clients, NS_MAX_CLIENTS);

	/* LAN hosts do not have a station table owned by hostapd. Compatibility
	 * negotiation still gives us the admitted peer's address. */
	const char* negotiated = NS_compatLastPeer();
	if (count == 0 && negotiated && negotiated[0]) {
		memset(&clients[0], 0, sizeof(clients[0]));
		snprintf(clients[0].ip, sizeof(clients[0].ip), "%s", negotiated);
		snprintf(clients[0].mac, sizeof(clients[0].mac), "compatibility peer");
		count = 1;
	}

	char temporary[600];
	snprintf(temporary, sizeof(temporary), "%s.tmp.%d", status_path, getpid());
	FILE* f = fopen(temporary, "w");
	if (!f) return;
	fprintf(f, "pid=%d\nnetwork=%s\nguest_count=%d\n",
	        getpid(), session->network, count);
	for (int i = 0; i < count; i++) {
		fprintf(f, "guest_%d_id=%s\nguest_%d_ip=%s\n",
		        i, clients[i].mac, i, clients[i].ip);
	}
	if (fclose(f) == 0) rename(temporary, status_path);
	else remove(temporary);
}

int main(void) {
	NS_init();

	char pid_path[512], status_path[512];
	snprintf(pid_path, sizeof(pid_path), "%s/broker.pid", NS_statePath());
	snprintf(status_path, sizeof(status_path), "%s/broker.status", NS_statePath());
	if (!write_pid(pid_path)) {
		fprintf(stderr, "netplay broker already owned: %s\n", strerror(errno));
		return 1;
	}

	signal(SIGINT, stop_signal);
	signal(SIGTERM, stop_signal);
	signal(SIGHUP, stop_signal);

	NS_SessionInfo session;
	if (!NS_sessionInfo(&session) || session.role != NS_ROLE_HOST) {
		fprintf(stderr, "netplay broker started without a host session\n");
		remove(pid_path);
		return 1;
	}

	NS_announceHotspot(session.network[0] ? session.network : NULL,
	                   session.psk[0] ? session.psk : NULL);
	NS_announceStart(NS_MODE_NETPLAY);
	if (!NS_compatServeStart()) {
		fprintf(stderr, "netplay broker could not open compatibility listener\n");
		remove(pid_path);
		return 1;
	}

	int status_tick = 0;
	while (!stopping) {
		if (!NS_isArmed()) break;
		NS_announceTick();
		NS_compatServeTick();
		/* Read only when someone opens Netplay.pak mid-session, so a few
		 * seconds stale is fine; each refresh shells out to iw/pidof. */
		if (status_tick++ % 50 == 0) publish_status(status_path, &session);
		usleep(100 * 1000);
	}

	NS_compatServeStop();
	NS_announceStop();
	remove(status_path);
	remove(pid_path);
	return 0;
}
