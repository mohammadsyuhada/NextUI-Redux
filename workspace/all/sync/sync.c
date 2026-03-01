#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "ui_components.h"
#include "utils.h"

// Sync configuration
#define SYNC_UDP_PORT 19999
#define SYNC_RSYNC_PORT 18730
#define HELLO_MSG "HELLO_TRIMUI_SYNC:"
#define ACK_MSG "TRIMUI_SYNC_ACK:"
#define READY_MSG "TRIMUI_SYNC_READY"
#define DONE_MSG "TRIMUI_SYNC_DONE"
#define BROADCAST_INTERVAL_MS 1000
#define RSYNC_CONF_PATH "/tmp/rsyncd.conf"
#define RSYNC_PID_PATH "/tmp/rsyncd.pid"

// Paths to sync
#define SAVES_PATH SDCARD_PATH "/Saves"
#define SHARED_DATA_PATH SHARED_USERDATA_PATH
// ROMS_PATH already defined in defines.h

// rsync binary path (shared across platforms)
#define RSYNC_BIN SHARED_BIN_PATH "/rsync"

// Log buffer for terminal-like display
#define LOG_MAX_LINES 20
#define LOG_LINE_LEN 128

static char log_lines[LOG_MAX_LINES][LOG_LINE_LEN];
static int log_count = 0;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// States
enum SyncState {
	STATE_INIT,
	STATE_NO_WIFI,
	STATE_READY,
	STATE_WAITING,
	STATE_SYNCING,
	STATE_ERROR,
	STATE_DONE,
};

static SDL_Surface* screen;
static enum SyncState state = STATE_INIT;

// Network state
static char own_ip[64] = {0};
static char peer_ip[64] = {0};
static char broadcast_ip[64] = {0};
static volatile bool peer_found = false;
static volatile bool discovery_running = false;
static pthread_t discovery_thread;
static bool discovery_thread_active = false;
static int udp_sock = -1;

// Sync state
static volatile bool sync_cancel = false;
static volatile bool sync_done = false;
static volatile int sync_result = 0; // 0=success, -1=error
static volatile int sync_phase = 0;	 // 1-4 for progress display
static pthread_t sync_thread;
static bool sync_thread_active = false;
static bool is_server = false;

// Sync options
static volatile bool sync_roms = false;
static volatile bool peer_sync_roms = false;

// Animation state
static int dot_count = 0;
static uint32_t last_dot_time = 0;

static void log_clear(void) {
	pthread_mutex_lock(&log_mutex);
	log_count = 0;
	pthread_mutex_unlock(&log_mutex);
}

static void log_add(const char* line) {
	pthread_mutex_lock(&log_mutex);

	// Shift lines up if buffer is full
	if (log_count >= LOG_MAX_LINES) {
		memmove(log_lines[0], log_lines[1], (LOG_MAX_LINES - 1) * LOG_LINE_LEN);
		log_count = LOG_MAX_LINES - 1;
	}

	// Copy line, truncate if needed
	strncpy(log_lines[log_count], line, LOG_LINE_LEN - 1);
	log_lines[log_count][LOG_LINE_LEN - 1] = '\0';

	// Strip trailing newline
	int len = strlen(log_lines[log_count]);
	if (len > 0 && log_lines[log_count][len - 1] == '\n')
		log_lines[log_count][len - 1] = '\0';

	log_count++;
	pthread_mutex_unlock(&log_mutex);
}

static int get_own_ip(void) {
	struct ifaddrs* ifaddr;
	struct ifaddrs* ifa;

	if (getifaddrs(&ifaddr) == -1)
		return -1;

	int found = 0;
	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL)
			continue;
		if (ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (!(ifa->ifa_flags & IFF_UP))
			continue;
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;

		struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
		inet_ntop(AF_INET, &sa->sin_addr, own_ip, sizeof(own_ip));

		if (!ifa->ifa_netmask)
			continue;
		struct sockaddr_in* mask = (struct sockaddr_in*)ifa->ifa_netmask;
		uint32_t ip_n = sa->sin_addr.s_addr;
		uint32_t mask_n = mask->sin_addr.s_addr;
		uint32_t bcast_n = ip_n | ~mask_n;
		struct in_addr bcast_addr;
		bcast_addr.s_addr = bcast_n;
		inet_ntop(AF_INET, &bcast_addr, broadcast_ip, sizeof(broadcast_ip));

		found = 1;

		if (strcmp(ifa->ifa_name, "wlan0") == 0)
			break;
	}

	freeifaddrs(ifaddr);
	return found ? 0 : -1;
}

static int compare_ips(const char* ip_a, const char* ip_b) {
	struct in_addr a, b;
	inet_pton(AF_INET, ip_a, &a);
	inet_pton(AF_INET, ip_b, &b);
	uint32_t na = ntohl(a.s_addr);
	uint32_t nb = ntohl(b.s_addr);
	if (na < nb)
		return -1;
	if (na > nb)
		return 1;
	return 0;
}

static int create_send_socket(void) {
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;

	int yes = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

	return sock;
}

static int create_recv_socket(void) {
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;

	int yes = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
	setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(SYNC_UDP_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		close(sock);
		return -1;
	}

	return sock;
}

static void send_udp_message(int sock, const char* msg, const char* dest_ip) {
	struct sockaddr_in dest;
	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(SYNC_UDP_PORT);
	inet_pton(AF_INET, dest_ip, &dest.sin_addr);
	sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&dest, sizeof(dest));
}

// Parse IP and roms flag from a discovery message payload.
// Format: "<ip>" or "<ip>:R" (R = roms enabled)
static void parse_discovery_payload(const char* payload, char* ip_out, size_t ip_len, bool* roms_out) {
	*roms_out = false;
	const char* colon = strrchr(payload, ':');
	if (colon && strlen(colon + 1) <= 1) {
		// Has a flag suffix like ":R" or ":0"
		size_t ip_part_len = colon - payload;
		if (ip_part_len >= ip_len)
			ip_part_len = ip_len - 1;
		strncpy(ip_out, payload, ip_part_len);
		ip_out[ip_part_len] = '\0';
		if (*(colon + 1) == 'R')
			*roms_out = true;
	} else {
		// No flag, just IP (backwards compatible)
		strncpy(ip_out, payload, ip_len - 1);
		ip_out[ip_len - 1] = '\0';
	}
}

static void* discovery_thread_func(void* arg) {
	(void)arg;
	char send_buf[128];
	char recv_buf[256];

	uint32_t last_broadcast = 0;

	while (discovery_running && !app_quit) {
		// Build message each time so it reflects current sync_roms toggle
		snprintf(send_buf, sizeof(send_buf), "%s%s:%s", HELLO_MSG, own_ip, sync_roms ? "R" : "0");

		uint32_t now = SDL_GetTicks();
		if (now - last_broadcast >= BROADCAST_INTERVAL_MS) {
			send_udp_message(udp_sock, send_buf, broadcast_ip);
			last_broadcast = now;
		}

		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(udp_sock, &readfds);
		struct timeval tv = {0, 200000};

		int ret = select(udp_sock + 1, &readfds, NULL, NULL, &tv);
		if (ret > 0 && FD_ISSET(udp_sock, &readfds)) {
			struct sockaddr_in from;
			socklen_t fromlen = sizeof(from);
			ssize_t n = recvfrom(udp_sock, recv_buf, sizeof(recv_buf) - 1, 0,
								 (struct sockaddr*)&from, &fromlen);
			if (n > 0) {
				recv_buf[n] = '\0';

				char sender_ip[64] = {0};
				bool sender_roms = false;

				if (strncmp(recv_buf, HELLO_MSG, strlen(HELLO_MSG)) == 0) {
					parse_discovery_payload(recv_buf + strlen(HELLO_MSG), sender_ip, sizeof(sender_ip), &sender_roms);
				} else if (strncmp(recv_buf, ACK_MSG, strlen(ACK_MSG)) == 0) {
					parse_discovery_payload(recv_buf + strlen(ACK_MSG), sender_ip, sizeof(sender_ip), &sender_roms);
				}

				if (sender_ip[0] && strcmp(sender_ip, own_ip) != 0) {
					if (strncmp(recv_buf, HELLO_MSG, strlen(HELLO_MSG)) == 0) {
						char ack_buf[128];
						snprintf(ack_buf, sizeof(ack_buf), "%s%s:%s", ACK_MSG, own_ip, sync_roms ? "R" : "0");
						send_udp_message(udp_sock, ack_buf, sender_ip);
					}

					if (!peer_found) {
						strncpy(peer_ip, sender_ip, sizeof(peer_ip) - 1);
						peer_sync_roms = sender_roms;
						__sync_synchronize();
						peer_found = true;
						discovery_running = false;
					}
				}
			}
		}
	}

	return NULL;
}

static void start_discovery(void) {
	peer_found = false;
	peer_sync_roms = false;
	peer_ip[0] = '\0';
	discovery_running = true;

	// Prevent sleep and power off during discovery and sync
	PWR_disableSleep();
	PWR_disableAutosleep();
	PWR_disablePowerOff();

	udp_sock = create_recv_socket();
	if (udp_sock < 0) {
		PWR_enableSleep();
		PWR_enableAutosleep();
		state = STATE_ERROR;
		return;
	}

	if (pthread_create(&discovery_thread, NULL, discovery_thread_func, NULL) != 0) {
		close(udp_sock);
		udp_sock = -1;
		PWR_enableSleep();
		PWR_enableAutosleep();
		state = STATE_ERROR;
		return;
	}
	discovery_thread_active = true;
	state = STATE_WAITING;
}

static void stop_discovery(void) {
	discovery_running = false;
	if (discovery_thread_active) {
		pthread_join(discovery_thread, NULL);
		discovery_thread_active = false;
	}
	if (udp_sock >= 0) {
		close(udp_sock);
		udp_sock = -1;
	}
	// Note: sleep is re-enabled later when sync completes/cancels/errors,
	// or immediately if discovery is stopped without syncing (B to exit).
}

static int wait_for_udp_message(const char* expected, int timeout_sec) {
	int sock = create_recv_socket();
	if (sock < 0)
		return -1;

	char buf[256];
	time_t start = time(NULL);

	while (!sync_cancel && !app_quit) {
		if (time(NULL) - start > timeout_sec) {
			close(sock);
			return -1;
		}

		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(sock, &readfds);
		struct timeval tv = {1, 0};

		int ret = select(sock + 1, &readfds, NULL, NULL, &tv);
		if (ret > 0) {
			ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
			if (n > 0) {
				buf[n] = '\0';
				if (strcmp(buf, expected) == 0) {
					close(sock);
					return 0;
				}
			}
		}
	}

	close(sock);
	return -1;
}

static void write_rsync_config(void) {
	FILE* fp = fopen(RSYNC_CONF_PATH, "w");
	if (!fp)
		return;

	fprintf(fp, "pid file = %s\n", RSYNC_PID_PATH);
	fprintf(fp, "port = %d\n", SYNC_RSYNC_PORT);
	fprintf(fp, "use chroot = no\n");
	fprintf(fp, "read only = no\n");
	fprintf(fp, "uid = 0\n");
	fprintf(fp, "gid = 0\n");
	fprintf(fp, "log file = /tmp/rsyncd.log\n");
	fprintf(fp, "transfer logging = yes\n");
	fprintf(fp, "log format = %%o %%f (%%l bytes)\n\n");
	fprintf(fp, "[shared]\n");
	fprintf(fp, "  path = %s\n", SHARED_DATA_PATH);
	fprintf(fp, "  read only = no\n\n");
	fprintf(fp, "[saves]\n");
	fprintf(fp, "  path = %s\n", SAVES_PATH);
	fprintf(fp, "  read only = no\n\n");
	fprintf(fp, "[roms]\n");
	fprintf(fp, "  path = %s\n", ROMS_PATH);
	fprintf(fp, "  read only = no\n");

	fclose(fp);
}

static int start_rsync_daemon(void) {
	if (access(RSYNC_BIN, X_OK) != 0)
		return -1;

	write_rsync_config();

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "%s --daemon --config=%s", RSYNC_BIN, RSYNC_CONF_PATH);
	int ret = system(cmd);
	if (ret != 0)
		return -1;

	usleep(500000);
	if (access(RSYNC_PID_PATH, F_OK) != 0)
		return -1;

	return 0;
}

static void stop_rsync_daemon(void) {
	FILE* fp = fopen(RSYNC_PID_PATH, "r");
	if (fp) {
		int pid;
		if (fscanf(fp, "%d", &pid) == 1 && pid > 0) {
			kill(pid, SIGTERM);
		}
		fclose(fp);
		unlink(RSYNC_PID_PATH);
	}
	system("killall rsync 2>/dev/null");
	unlink(RSYNC_CONF_PATH);
}

// Progress tracking for current phase
static volatile int phase_files_done = 0;
static volatile int phase_files_total = 0;
static volatile int phase_files_transferred = 0;

// Parse "xfr#N, to-chk=X/Y" from rsync --info=progress2 output.
static void parse_progress2(const char* line) {
	// Parse xfr#N (actual files transferred)
	const char* x = strstr(line, "xfr#");
	if (x) {
		int n = 0;
		if (sscanf(x + 4, "%d", &n) == 1)
			phase_files_transferred = n;
	}

	// Parse to-chk=X/Y (remaining/total scanned)
	const char* p = strstr(line, "to-chk=");
	if (!p)
		p = strstr(line, "to-check=");
	if (!p)
		return;

	p = strchr(p, '=');
	if (!p)
		return;
	p++;

	int remaining = 0, total = 0;
	if (sscanf(p, "%d/%d", &remaining, &total) == 2 && total > 0) {
		phase_files_total = total;
		phase_files_done = total - remaining;
	}
}

// Read a line delimited by \r or \n from a FILE stream.
// rsync --info=progress2 uses \r to update progress in-place.
static char* read_line_cr(char* buf, int size, FILE* fp) {
	int i = 0;
	int c;
	while (i < size - 1) {
		c = fgetc(fp);
		if (c == EOF) {
			if (i == 0)
				return NULL;
			break;
		}
		if (c == '\r' || c == '\n') {
			if (i > 0)
				break;
			continue; // skip leading \r\n
		}
		buf[i++] = (char)c;
	}
	buf[i] = '\0';
	return buf;
}

// Run rsync via popen() and capture output line by line into the log buffer
static int run_rsync_phase(int phase) {
	char cmd[1024];
	const char* rsync_opts = "-rtv --update --inplace --no-perms --omit-dir-times --info=progress2";
	const char* shared_excludes =
		"--exclude=battery_logs.sqlite "
		"--exclude=game_logs.sqlite "
		"--exclude=ledsettings.txt "
		"--exclude=ledsettings_brick.txt "
		"--exclude=minuisettings.txt";

	phase_files_done = 0;
	phase_files_total = 0;
	phase_files_transferred = 0;

	switch (phase) {
	case 1:
		snprintf(cmd, sizeof(cmd), "%s %s %s %s/ rsync://%s:%d/shared/ 2>&1",
				 RSYNC_BIN, rsync_opts, shared_excludes, SHARED_DATA_PATH, peer_ip, SYNC_RSYNC_PORT);
		break;
	case 2:
		snprintf(cmd, sizeof(cmd), "%s %s %s/ rsync://%s:%d/saves/ 2>&1",
				 RSYNC_BIN, rsync_opts, SAVES_PATH, peer_ip, SYNC_RSYNC_PORT);
		break;
	case 3:
		snprintf(cmd, sizeof(cmd), "%s %s %s rsync://%s:%d/shared/ %s/ 2>&1",
				 RSYNC_BIN, rsync_opts, shared_excludes, peer_ip, SYNC_RSYNC_PORT, SHARED_DATA_PATH);
		break;
	case 4:
		snprintf(cmd, sizeof(cmd), "%s %s rsync://%s:%d/saves/ %s/ 2>&1",
				 RSYNC_BIN, rsync_opts, peer_ip, SYNC_RSYNC_PORT, SAVES_PATH);
		break;
	case 5:
		snprintf(cmd, sizeof(cmd), "%s %s %s/ rsync://%s:%d/roms/ 2>&1",
				 RSYNC_BIN, rsync_opts, ROMS_PATH, peer_ip, SYNC_RSYNC_PORT);
		break;
	case 6:
		snprintf(cmd, sizeof(cmd), "%s %s rsync://%s:%d/roms/ %s/ 2>&1",
				 RSYNC_BIN, rsync_opts, peer_ip, SYNC_RSYNC_PORT, ROMS_PATH);
		break;
	default:
		return -1;
	}

	FILE* fp = popen(cmd, "r");
	if (!fp)
		return -1;

	char line[512];
	while (read_line_cr(line, sizeof(line), fp) != NULL) {
		if (sync_cancel)
			break;

		// Skip blank lines and rsync noise
		if (line[0] == '\0')
			continue;
		if (strncmp(line, "sending incremental", 19) == 0)
			continue;
		if (strncmp(line, "receiving incremental", 20) == 0)
			continue;
		if (strncmp(line, "sent ", 5) == 0)
			continue;
		if (strncmp(line, "total size", 10) == 0)
			continue;

		// Check for progress2 info (contains "to-chk=" or "xfr#")
		if (strstr(line, "to-chk=") || strstr(line, "to-check=")) {
			parse_progress2(line);
			continue; // don't add raw progress lines to log
		}

		// Skip lines that are just progress percentages (e.g. "  1,234,567  50%  ...")
		if (line[0] == ' ' && strstr(line, "%"))
			continue;

		log_add(line);
	}

	int status = pclose(fp);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void* sync_thread_func(void* arg) {
	(void)arg;

	mkdir_p(SHARED_DATA_PATH);
	mkdir_p(SAVES_PATH);
	if (sync_roms)
		mkdir_p(ROMS_PATH);

	if (is_server) {
		log_add("Starting as server...");

		// Truncate daemon log before starting daemon so we read a clean file
		FILE* trunc = fopen("/tmp/rsyncd.log", "w");
		if (trunc)
			fclose(trunc);

		if (start_rsync_daemon() != 0) {
			log_add("ERROR: Failed to start rsync daemon");
			sync_result = -1;
			sync_done = true;
			return NULL;
		}

		log_add("rsync daemon started, waiting for client...");
		sync_phase = 0; // 0 = no phase yet; UI shows "Waiting for client..."

		int notify_sock = create_send_socket();
		int done_sock = create_recv_socket();
		if (done_sock < 0) {
			if (notify_sock >= 0)
				close(notify_sock);
			stop_rsync_daemon();
			log_add("ERROR: Failed to create socket");
			sync_result = -1;
			sync_done = true;
			return NULL;
		}

		char buf[256];
		time_t start = time(NULL);
		int ret = -1;
		uint32_t last_ready = 0;

		FILE* daemon_log = fopen("/tmp/rsyncd.log", "r");

		while (!sync_cancel && !app_quit) {
			if (time(NULL) - start > 3600) {
				log_add("ERROR: Timeout waiting for client");
				break;
			}

			uint32_t now = SDL_GetTicks();
			if (notify_sock >= 0 && now - last_ready >= 1000) {
				send_udp_message(notify_sock, READY_MSG, peer_ip);
				last_ready = now;
			}

			// Tail the rsync daemon log for new lines
			if (!daemon_log)
				daemon_log = fopen("/tmp/rsyncd.log", "r");
			if (daemon_log) {
				char logline[512];
				while (fgets(logline, sizeof(logline), daemon_log) != NULL) {
					// Strip trailing newline
					int ll = strlen(logline);
					if (ll > 0 && logline[ll - 1] == '\n')
						logline[ll - 1] = '\0';
					if (!logline[0])
						continue;

					// Strip "YYYY/MM/DD HH:MM:SS [PID] " prefix
					const char* msg = logline;
					const char* bracket = strchr(msg, ']');
					if (bracket && *(bracket + 1) == ' ')
						msg = bracket + 2;

					// For transfer lines ("send X" / "recv X"), show just the path
					if (strncmp(msg, "send ", 5) == 0)
						msg += 5;
					else if (strncmp(msg, "recv ", 5) == 0)
						msg += 5;

					if (msg[0])
						log_add(msg);
				}
				// fgets returns NULL at EOF; file handle stays valid for subsequent reads
			}

			fd_set readfds;
			FD_ZERO(&readfds);
			FD_SET(done_sock, &readfds);
			struct timeval tv = {0, 200000};

			int sel = select(done_sock + 1, &readfds, NULL, NULL, &tv);
			if (sel > 0) {
				ssize_t n = recvfrom(done_sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
				if (n > 0) {
					buf[n] = '\0';
					if (strcmp(buf, DONE_MSG) == 0) {
						log_add("Client finished sync");
						ret = 0;
						break;
					}
				}
			}
		}

		if (daemon_log)
			fclose(daemon_log);
		if (notify_sock >= 0)
			close(notify_sock);
		close(done_sock);
		stop_rsync_daemon();

		sync_result = (ret == 0) ? 0 : -1;
		sync_done = true;
	} else {
		log_add("Starting as client...");
		log_add("Waiting for server...");
		sync_phase = 0;

		int ret = wait_for_udp_message(READY_MSG, 30);
		if (ret != 0) {
			log_add("ERROR: Server not ready (timeout)");
			sync_result = -1;
			sync_done = true;
			return NULL;
		}

		log_add("Server ready!");
		usleep(500000);

		int total_phases = sync_roms ? 6 : 4;
		const char* phase_names_4[] = {
			"",
			"[1/4] Pushing settings",
			"[2/4] Pushing saves",
			"[3/4] Pulling settings",
			"[4/4] Pulling saves",
		};
		const char* phase_names_6[] = {
			"",
			"[1/6] Pushing settings",
			"[2/6] Pushing saves",
			"[3/6] Pulling settings",
			"[4/6] Pulling saves",
			"[5/6] Pushing ROMs",
			"[6/6] Pulling ROMs",
		};
		const char** phase_names = sync_roms ? phase_names_6 : phase_names_4;

		int total_transferred = 0;
		int total_scanned = 0;
		for (int phase = 1; phase <= total_phases && !sync_cancel; phase++) {
			sync_phase = phase;
			log_add(phase_names[phase]);
			int result = run_rsync_phase(phase);
			if (result != 0) {
				char err[LOG_LINE_LEN];
				snprintf(err, sizeof(err), "ERROR: Phase %d failed (exit %d)", phase, result);
				log_add(err);
				sync_result = -1;
				sync_done = true;
				return NULL;
			}
			total_transferred += phase_files_transferred;
			total_scanned += phase_files_total;

			char summary[LOG_LINE_LEN];
			snprintf(summary, sizeof(summary), "  Done: %d files changed out of %d",
					 phase_files_transferred, phase_files_total);
			log_add(summary);
		}

		{
			char final_summary[LOG_LINE_LEN];
			snprintf(final_summary, sizeof(final_summary),
					 "Sync complete: %d files changed out of %d total",
					 total_transferred, total_scanned);
			log_add(final_summary);
		}

		int notify_sock = create_send_socket();
		if (notify_sock >= 0) {
			for (int i = 0; i < 5; i++) {
				send_udp_message(notify_sock, DONE_MSG, peer_ip);
				usleep(200000);
			}
			close(notify_sock);
		}

		sync_result = 0;
		sync_done = true;
	}

	return NULL;
}

static void start_sync(void) {
	is_server = (compare_ips(own_ip, peer_ip) < 0);

	// Either device enabling ROMS sync activates it for both
	if (peer_sync_roms)
		sync_roms = true;

	sync_cancel = false;
	sync_done = false;
	sync_result = 0;
	sync_phase = 0;

	log_clear();
	char msg[LOG_LINE_LEN];
	snprintf(msg, sizeof(msg), "Syncing with %s", peer_ip);
	log_add(msg);
	snprintf(msg, sizeof(msg), "Role: %s", is_server ? "Server" : "Client");
	log_add(msg);
	if (sync_roms)
		log_add("ROMs sync: enabled");

	// Sleep already disabled from start_discovery(), ensure it stays disabled
	PWR_disableSleep();
	PWR_disableAutosleep();

	state = STATE_SYNCING;
	if (pthread_create(&sync_thread, NULL, sync_thread_func, NULL) != 0) {
		PWR_enableSleep();
		state = STATE_ERROR;
		return;
	}
	sync_thread_active = true;
}

static void cancel_sync(void) {
	sync_cancel = true;
	stop_rsync_daemon(); // kills daemon + killall rsync, unblocks popen in sync thread
	if (sync_thread_active) {
		pthread_join(sync_thread, NULL);
		sync_thread_active = false;
	}
	PWR_enableSleep();
	PWR_enableAutosleep();
}

static void render_log(int top_y, int bottom_y) {
	pthread_mutex_lock(&log_mutex);

	int line_h = SCALE1(FONT_SMALL + 2);
	int pad = SCALE1(PADDING * 2);
	int x = pad;
	int max_w = screen->w - pad * 2;
	int avail_h = bottom_y - top_y;
	int max_visible = avail_h / line_h;

	// Clip to padded area so long lines are truncated
	SDL_Rect clip = {x, top_y, max_w, avail_h};
	SDL_SetClipRect(screen, &clip);

	// Show the most recent lines that fit
	int start_idx = 0;
	if (log_count > max_visible)
		start_idx = log_count - max_visible;

	int y = top_y;
	for (int i = start_idx; i < log_count; i++) {
		GFX_blitText(font.small, log_lines[i], 0, COLOR_WHITE, screen,
					 &(SDL_Rect){x, y, max_w, line_h});
		y += line_h;
	}

	SDL_SetClipRect(screen, NULL);

	pthread_mutex_unlock(&log_mutex);
}

static void render_screen(void) {
	GFX_clear(screen);
	int menu_h = UI_renderMenuBar(screen, "Device Sync");

	switch (state) {
	case STATE_INIT:
		UI_renderCenteredMessage(screen, "Checking WiFi...");
		break;

	case STATE_NO_WIFI:
		UI_renderCenteredMessage(screen, "WiFi not connected.\nPlease enable WiFi and try again.");
		UI_renderButtonHintBar(screen, (char*[]){"B", "EXIT", NULL});
		break;

	case STATE_READY: {
		UI_renderButtonHintBar(screen, (char*[]){"B", "EXIT", "A", "START", "X", sync_roms ? "ROMS: ON" : "ROMS: OFF", NULL});
		int max_w = screen->w - SCALE1(PADDING * 2);
		int ll = TTF_FontLineSkip(font.large);
		int sl = TTF_FontLineSkip(font.small);
		int gap = SCALE1(PADDING * 2);

		int bullet_lines = sync_roms ? 3 : 2;

		// Calculate total content height
		int total_h = ll * 2			  // title (2 lines)
					  + gap				  // gap
					  + sl				  // "What will be synced:"
					  + SCALE1(PADDING)	  // small gap
					  + sl * bullet_lines // bullet items
					  + gap				  // gap
					  + sl * 3;			  // instructions (3 lines)

		// Center vertically on the full screen
		int y = (screen->h - total_h) / 2;

		GFX_blitText(font.large, "Sync saves and settings\nbetween two devices over WiFi.", ll,
					 COLOR_WHITE, screen,
					 &(SDL_Rect){SCALE1(PADDING), y, max_w, ll * 2});
		y += ll * 2 + gap;

		GFX_blitText(font.small, "What will be synced:", 0,
					 COLOR_WHITE, screen,
					 &(SDL_Rect){SCALE1(PADDING), y, max_w, sl});
		y += sl + SCALE1(PADDING);

		const char* items = sync_roms
								? "- Game saves (Saves/)\n- Shared settings and states (.userdata/shared/)\n- ROMs (Roms/)"
								: "- Game saves (Saves/)\n- Shared settings and states (.userdata/shared/)";
		GFX_blitText(font.small, items, sl,
					 COLOR_WHITE, screen,
					 &(SDL_Rect){SCALE1(PADDING * 2), y, max_w, sl * bullet_lines});
		y += sl * bullet_lines + gap;

		GFX_blitText(font.small, "Both devices must be on the same WiFi\nnetwork. Open Sync on both devices\nand press A to start.", sl,
					 COLOR_WHITE, screen,
					 &(SDL_Rect){SCALE1(PADDING), y, max_w, sl * 3});
	} break;

	case STATE_WAITING: {
		uint32_t now = SDL_GetTicks();
		if (now - last_dot_time > 500) {
			dot_count = (dot_count + 1) % 4;
			last_dot_time = now;
		}
		int large_leading = TTF_FontLineSkip(font.large);
		char msg[128];
		snprintf(msg, sizeof(msg), "Searching for device%.*s\n\nOpen Sync on the other device.\nIP: %s",
				 dot_count + 1, "...", own_ip);

		int y = screen->h / 2 - large_leading * 2;
		GFX_blitText(font.large, msg, large_leading, COLOR_WHITE, screen,
					 &(SDL_Rect){SCALE1(PADDING), y, screen->w - SCALE1(PADDING * 2), screen->h});
	}
		UI_renderButtonHintBar(screen, (char*[]){"B", "EXIT", NULL});
		break;

	case STATE_SYNCING: {
		UI_renderButtonHintBar(screen, (char*[]){"B", "CANCEL", NULL});

		int top_y = menu_h + SCALE1(PADDING);
		int bottom_y = screen->h - SCALE1(PILL_SIZE + PADDING);

		// Show phase progress header at top
		{
			int total_phases = sync_roms ? 6 : 4;
			const char* phase_labels[] = {
				"",
				"Pushing settings",
				"Pushing saves",
				"Pulling settings",
				"Pulling saves",
				"Pushing ROMs",
				"Pulling ROMs",
			};
			int p = sync_phase;
			if (p >= 1 && p <= total_phases) {
				char hdr[128];
				int done = phase_files_done;
				int total = phase_files_total;
				int xfr = phase_files_transferred;
				if (total > 0)
					snprintf(hdr, sizeof(hdr), "[%d/%d] %s - %d/%d files (%d changed)",
							 p, total_phases, phase_labels[p], done, total, xfr);
				else
					snprintf(hdr, sizeof(hdr), "[%d/%d] %s",
							 p, total_phases, phase_labels[p]);
				GFX_blitText(font.large, hdr, 0, COLOR_WHITE, screen,
							 &(SDL_Rect){SCALE1(PADDING), top_y,
										 screen->w - SCALE1(PADDING * 2), SCALE1(FONT_LARGE)});
			} else if (is_server) {
				GFX_blitText(font.large, "Waiting for client...", 0, COLOR_WHITE, screen,
							 &(SDL_Rect){SCALE1(PADDING), top_y,
										 screen->w - SCALE1(PADDING * 2), SCALE1(FONT_LARGE)});
			}
			top_y += TTF_FontLineSkip(font.large) + SCALE1(PADDING);
		}

		render_log(top_y, bottom_y);
		break;
	}

	case STATE_ERROR: {
		int top_y = menu_h + SCALE1(PADDING);
		int bottom_y = screen->h - SCALE1(PILL_SIZE + PADDING * 3 + FONT_LARGE);

		render_log(top_y, bottom_y);

		// Error message below log
		GFX_blitText(font.large, "Sync failed. Press A to retry.", 0, COLOR_WHITE, screen,
					 &(SDL_Rect){SCALE1(PADDING), bottom_y + SCALE1(PADDING),
								 screen->w - SCALE1(PADDING * 2), SCALE1(FONT_LARGE)});
	}
		UI_renderButtonHintBar(screen, (char*[]){"A", "RETRY", "B", "EXIT", NULL});
		break;

	case STATE_DONE: {
		int top_y = menu_h + SCALE1(PADDING);
		int bottom_y = screen->h - SCALE1(PILL_SIZE + PADDING * 3 + FONT_LARGE);

		render_log(top_y, bottom_y);

		GFX_blitText(font.large, "Sync complete!", 0, COLOR_WHITE, screen,
					 &(SDL_Rect){SCALE1(PADDING), bottom_y + SCALE1(PADDING),
								 screen->w - SCALE1(PADDING * 2), SCALE1(FONT_LARGE)});
	}
		UI_renderButtonHintBar(screen, (char*[]){"A", "SYNC AGAIN", "B", "EXIT", NULL});
		break;
	}

	GFX_flip(screen);
}

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	screen = GFX_init(MODE_MAIN);
	UI_showSplashScreen(screen, "Device Sync");

	PWR_pinToCores(CPU_CORE_EFFICIENCY);
	InitSettings();
	PAD_init();
	PWR_init();
	setup_signal_handlers();

	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	int is_online = 0;
	PLAT_getNetworkStatus(&is_online);
	if (!is_online || get_own_ip() != 0) {
		state = STATE_NO_WIFI;
	} else {
		state = STATE_READY;
	}

	while (!app_quit) {
		GFX_startFrame();
		PAD_poll();
		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (UI_statusBarChanged())
			dirty = true;

		switch (state) {
		case STATE_INIT:
			dirty = true;
			break;

		case STATE_NO_WIFI:
			if (PAD_justPressed(BTN_B))
				app_quit = true;
			break;

		case STATE_READY:
			if (PAD_justPressed(BTN_A)) {
				start_discovery();
				dirty = true;
			}
			if (PAD_justPressed(BTN_X)) {
				sync_roms = !sync_roms;
				dirty = true;
			}
			if (PAD_justPressed(BTN_B))
				app_quit = true;
			break;

		case STATE_WAITING:
			dirty = true;
			if (PAD_justPressed(BTN_B)) {
				stop_discovery();
				PWR_enableSleep();
				PWR_enableAutosleep();
				app_quit = true;
			} else if (peer_found) {
				__sync_synchronize();
				stop_discovery();
				start_sync();
				dirty = true;
			}
			break;

		case STATE_SYNCING:
			dirty = true;
			if (PAD_justPressed(BTN_B)) {
				cancel_sync();
				state = STATE_ERROR;
				dirty = true;
			} else if (sync_done) {
				if (sync_thread_active) {
					pthread_join(sync_thread, NULL);
					sync_thread_active = false;
				}
				stop_rsync_daemon();
				PWR_enableSleep();
				PWR_enableAutosleep();

				if (sync_roms) {
					unlink(EMULIST_CACHE_PATH);
					unlink(ROMINDEX_CACHE_PATH);
				}

				if (sync_result == 0) {
					state = STATE_DONE;
				} else {
					state = STATE_ERROR;
				}
				dirty = true;
			}
			break;

		case STATE_ERROR:
			if (PAD_justPressed(BTN_A)) {
				log_clear();
				if (get_own_ip() != 0) {
					state = STATE_NO_WIFI;
				} else {
					state = STATE_READY;
				}
				dirty = true;
			}
			if (PAD_justPressed(BTN_B))
				app_quit = true;
			break;

		case STATE_DONE:
			if (PAD_justPressed(BTN_A)) {
				log_clear();
				if (get_own_ip() != 0) {
					state = STATE_NO_WIFI;
				} else {
					state = STATE_READY;
				}
				dirty = true;
			}
			if (PAD_justPressed(BTN_B))
				app_quit = true;
			break;
		}

		if (dirty) {
			render_screen();
			dirty = false;
		} else {
			GFX_sync();
		}
	}

	if (discovery_thread_active) {
		discovery_running = false;
		pthread_join(discovery_thread, NULL);
	}
	if (sync_thread_active) {
		sync_cancel = true;
		pthread_join(sync_thread, NULL);
	}
	stop_rsync_daemon();

	if (udp_sock >= 0)
		close(udp_sock);

	PWR_enableSleep();
	PWR_enableAutosleep();

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
