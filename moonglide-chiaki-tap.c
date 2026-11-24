// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// moonglide-chiaki-tap: a headless PS5 Remote Play client built on libchiaki (chiaki-ng).
// It writes every compressed video frame into POSIX shared memory (src/rp_tap.h of moonglide),
// plays the sound through PulseAudio (optionally held back so it meets the picture moonglide shows
// later) and forwards SDL game controllers to the console. No window, no decoding.
//
// Stream start semantics: the shm stays "magic=0, state IDLE/WAKING/CONNECTING" until the FIRST
// video sample of a session arrives (chiaki delivers the stream's codec header NAL units as that
// first sample). At that moment: magic=0 -> fields, session++, write_seq=0, codec header -> magic
// last -> state STREAMING. CHIAKI_EVENT_CONNECTED only updates `status`.
//
// Copyright (C) the chiaki-ng authors; this program is a derived work of chiaki-ng and is
// licensed under the same terms, see LICENSE.

#define _GNU_SOURCE
#include <chiaki/common.h>
#include <chiaki/log.h>
#include <chiaki/session.h>
#include <chiaki/discovery.h>
#include <chiaki/controller.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/audio.h>
#include <chiaki/regist.h>
#include <chiaki/orientation.h>

#include <SDL2/SDL.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <curl/curl.h>
#include <json.h>
#include <openssl/evp.h>
#include <netinet/in.h>

#include "rp_tap.h"

/* ------------------------------------------------------------------ small utils */

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static volatile sig_atomic_t g_sig_stop;
static void on_signal(int s)
{
	(void)s;
	g_sig_stop = 1;
	alarm(2);   // hard exit (SIGALRM default) if the clean stop hangs; moonglide SIGKILLs at 3 s
}

static bool g_verbose;
static ChiakiLog g_log;
static atomic_int g_regist_http_code;     // last "Regist received HTTP code N" (register mode)
static atomic_bool g_regist_search_timeout, g_regist_search_refused;

/* Log to stderr. Hexdump lines (chiaki dumps raw control/session packets at INFO and up to
 * VERBOSE, which can carry key material) are dropped at every level. */
static bool is_hexdump_line(const char *m)
{
	size_t n = strlen(m);
	if(strncmp(m, "offset 0  1  2", 14) == 0)
		return true;
	if(n < 7 + 48 + 16 || m[6] != ' ')
		return false;
	for(int i = 0; i < 6; i++)
		if(!((m[i] >= '0' && m[i] <= '9') || (m[i] >= 'a' && m[i] <= 'f') || m[i] == ' '))
			return false;
	for(int i = 0; i < 16; i++)
		if(m[7 + 3 * i + 2] != ' ')
			return false;
	return true;
}

// The PS5 answers the login with an undocumented state when nobody is signed in on the console
// (the "Who's using this controller?" screen, after a cold boot or a log-out). The session can't
// work until someone picks a user there, so it's cut short instead of timing out after ~45 s.
static atomic_bool g_login_refused;

static void log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	(void)user;
	if(is_hexdump_line(msg))
		return;
	if(!g_verbose && (level & (CHIAKI_LOG_VERBOSE | CHIAKI_LOG_DEBUG)))
	{
		// Verbose is on only for the console's connection-quality reports (the bitrate it targets):
		// the first few, then one every 10 s.
		static int seen;
		if(strncmp(msg, "StreamConnection received connection quality:", 46) != 0 || (seen++ >= 3 && seen % 10))
			return;
	}
	if(strncmp(msg, "Ctrl received Login message with state:", 39) == 0)
		atomic_store(&g_login_refused, true);
	{
		int code;
		if(sscanf(msg, "Regist received HTTP code %d", &code) == 1)
			atomic_store(&g_regist_http_code, code);
		else if(strstr(msg, "Regist timed out waiting for search response"))
			atomic_store(&g_regist_search_timeout, true);
		else if(strstr(msg, "refused the registration search"))
			atomic_store(&g_regist_search_refused, true);
	}
	fprintf(stderr, "[%c] %s\n", chiaki_log_level_char(level), msg);
}

static void logf_(ChiakiLogLevel level, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	chiaki_log(&g_log, level, "tap: %s", buf);
}
#define TAP_INFO(...) logf_(CHIAKI_LOG_INFO, __VA_ARGS__)
#define TAP_WARN(...) logf_(CHIAKI_LOG_WARNING, __VA_ARGS__)
#define TAP_ERR(...) logf_(CHIAKI_LOG_ERROR, __VA_ARGS__)

/* ------------------------------------------------------------------ Qt INI config */

#define MAX_HOSTS 16

typedef struct
{
	char nickname[64];
	int target;
	uint8_t regist_key[16];
	size_t regist_key_len;
	uint8_t rp_key[16];
	size_t rp_key_len;
	uint8_t mac[6];
	size_t mac_len;
	unsigned rp_key_type;
	bool have_target;
} HostEntry;

typedef struct
{
	HostEntry hosts[MAX_HOSTS];
	int host_count;
	long bitrate_local_ps5;   // 0 = not set
} Conf;

/* Unescape a QSettings INI value: optional double quotes (they toggle, also used to end a greedy
 * \x escape), \0 and octal, \xHH, \\ \" \a \b \f \n \r \t \v. Returns the decoded length. */
static size_t qt_unescape(const char *s, size_t n, uint8_t *out, size_t cap)
{
	size_t o = 0;
	bool inq = false;
	(void)inq;
	for(size_t i = 0; i < n; i++)
	{
		char c = s[i];
		uint8_t v;
		if(c == '"')
		{
			inq = !inq;
			continue;
		}
		if(c != '\\' || i + 1 >= n)
		{
			v = (uint8_t)c;
		}
		else
		{
			c = s[++i];
			switch(c)
			{
				case 'a': v = '\a'; break;
				case 'b': v = '\b'; break;
				case 'f': v = '\f'; break;
				case 'n': v = '\n'; break;
				case 'r': v = '\r'; break;
				case 't': v = '\t'; break;
				case 'v': v = '\v'; break;
				case '\\': v = '\\'; break;
				case '"': v = '"'; break;
				case 'x':
				{
					unsigned val = 0;
					int digits = 0;
					while(i + 1 < n && ((s[i + 1] >= '0' && s[i + 1] <= '9') || (s[i + 1] >= 'a' && s[i + 1] <= 'f') || (s[i + 1] >= 'A' && s[i + 1] <= 'F')))
					{
						char h = s[++i];
						val = val * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
						digits++;
					}
					if(digits == 0)
						val = 'x';
					v = (uint8_t)val;
					break;
				}
				default:
					if(c >= '0' && c <= '7')
					{
						unsigned val = (unsigned)(c - '0');
						int digits = 1;
						while(digits < 3 && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '7')
						{
							val = val * 8 + (unsigned)(s[++i] - '0');
							digits++;
						}
						v = (uint8_t)val;
					}
					else
						v = (uint8_t)c;
					break;
			}
		}
		if(o < cap)
			out[o] = v;
		o++;
	}
	return o;
}

/* Decode a raw value into out (cap bytes), strip a @ByteArray(...) wrapper. Returns length. */
static size_t qt_value(const char *raw, uint8_t *out, size_t cap)
{
	uint8_t tmp[1024];
	size_t n = qt_unescape(raw, strlen(raw), tmp, sizeof(tmp));
	if(n > sizeof(tmp))
		n = sizeof(tmp);
	const uint8_t *p = tmp;
	if(n >= 12 && memcmp(p, "@ByteArray(", 11) == 0 && p[n - 1] == ')')
	{
		p += 11;
		n -= 12;
	}
	if(n > cap)
		n = cap;
	memcpy(out, p, n);
	explicit_bzero(tmp, sizeof(tmp));
	return n;
}

static bool conf_load(Conf *c, const char *path)
{
	memset(c, 0, sizeof(*c));
	FILE *f = fopen(path, "r");
	if(!f)
		return false;
	char *line = NULL;
	size_t cap = 0;
	char section[64] = "";
	ssize_t len;
	while((len = getline(&line, &cap, f)) >= 0)
	{
		while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if(line[0] == '[')
		{
			char *e = strchr(line, ']');
			if(e)
			{
				*e = '\0';
				snprintf(section, sizeof(section), "%s", line + 1);
			}
			continue;
		}
		char *eq = strchr(line, '=');
		if(!eq)
			continue;
		*eq = '\0';
		const char *key = line, *val = eq + 1;
		if(!strcmp(section, "registered_hosts"))
		{
			int idx = 0;
			char name[64];
			if(sscanf(key, "%d\\%63s", &idx, name) != 2 || idx < 1 || idx > MAX_HOSTS)
				continue;
			HostEntry *h = &c->hosts[idx - 1];
			if(idx > c->host_count)
				c->host_count = idx;
			uint8_t buf[256];
			if(!strcmp(name, "rp_regist_key"))
				h->regist_key_len = qt_value(val, h->regist_key, sizeof(h->regist_key));
			else if(!strcmp(name, "rp_key"))
				h->rp_key_len = qt_value(val, h->rp_key, sizeof(h->rp_key));
			else if(!strcmp(name, "server_mac"))
				h->mac_len = qt_value(val, h->mac, sizeof(h->mac));
			else if(!strcmp(name, "server_nickname"))
			{
				size_t n = qt_value(val, buf, sizeof(buf) - 1);
				buf[n] = '\0';
				memcpy(h->nickname, buf, n < sizeof(h->nickname) - 1 ? n : sizeof(h->nickname) - 1);
			}
			else if(!strcmp(name, "rp_key_type"))
				h->rp_key_type = (unsigned)atol(val);
			else if(!strcmp(name, "target"))
			{
				h->target = atoi(val);
				h->have_target = true;
			}
		}
		else if(!strcmp(section, "settings") && !strcmp(key, "bitrate_local_ps5"))
			c->bitrate_local_ps5 = atol(val);
	}
	free(line);
	fclose(f);
	return true;
}


/* ------------------------------------------------------------------ moonglide's own conf (write side) */

static bool host_usable(const HostEntry *h)
{
	return h->regist_key_len == 16 && h->rp_key_len == 16;
}

static void mac_hex(const uint8_t *mac, char out[13])
{
	for(int i = 0; i < 6; i++)
		snprintf(out + 2 * i, 3, "%02x", mac[i]);
}

/* lowercase hex only (drops ':' '-' etc.), for comparing the discovery host-id with a server_mac */
static void norm_hostid(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	for(; in && *in && o + 1 < cap; in++)
		if((*in >= '0' && *in <= '9') || (*in >= 'a' && *in <= 'f'))
			out[o++] = *in;
		else if(*in >= 'A' && *in <= 'F')
			out[o++] = (char)(*in | 0x20);
	out[o] = '\0';
}


static void put_bytearray(FILE *f, const uint8_t *b, size_t n)
{
	fputs("@ByteArray(", f);
	for(size_t i = 0; i < n; i++)
		fprintf(f, "\\x%02x", b[i]);
	fputc(')', f);
}

/* Rewrite moonglide's conf (QSettings layout, same keys the parser above reads). Atomic, mode 0600. */
static bool conf_write(const Conf *c, const char *path, char *err, size_t errcap)
{
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", path);
	char *sl = strrchr(dir, '/');
	if(sl)
	{
		*sl = '\0';
		for(char *q = dir + 1; *q; q++)
			if(*q == '/')
			{
				*q = '\0';
				mkdir(dir, 0700);
				*q = '/';
			}
		if(mkdir(dir, 0700) != 0 && errno != EEXIST)
		{
			snprintf(err, errcap, "cannot create %s: %s", dir, strerror(errno));
			return false;
		}
	}
	char tmp[540];
	snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if(fd < 0)
	{
		snprintf(err, errcap, "cannot write %s: %s", tmp, strerror(errno));
		return false;
	}
	FILE *f = fdopen(fd, "w");
	int n = 0;
	for(int i = 0; i < c->host_count; i++)
		if(host_usable(&c->hosts[i]))
			n++;
	fprintf(f, "[registered_hosts]\n");
	int k = 0;
	for(int i = 0; i < c->host_count; i++)
	{
		const HostEntry *h = &c->hosts[i];
		if(!host_usable(h))
			continue;
		k++;
		char nick[64];
		snprintf(nick, sizeof(nick), "%s", h->nickname);
		for(char *q = nick; *q; q++)
			if((unsigned char)*q < 0x20 || *q == '"' || *q == '\\' || *q == '=')
				*q = '_';
		fprintf(f, "%d\\target=%d\n", k, h->target);
		fprintf(f, "%d\\server_nickname=%s\n", k, nick);
		fprintf(f, "%d\\server_mac=", k);
		put_bytearray(f, h->mac, h->mac_len == 6 ? 6 : 0);
		fprintf(f, "\n%d\\rp_regist_key=", k);
		put_bytearray(f, h->regist_key, 16);
		fprintf(f, "\n%d\\rp_key_type=%u\n", k, h->rp_key_type);
		fprintf(f, "%d\\rp_key=", k);
		put_bytearray(f, h->rp_key, 16);
		fputc('\n', f);
	}
	fprintf(f, "size=%d\n", n);
	if(c->bitrate_local_ps5 > 0)
		fprintf(f, "\n[settings]\nbitrate_local_ps5=%ld\n", c->bitrate_local_ps5);
	bool ok = fflush(f) == 0 && fsync(fd) == 0;
	ok = fclose(f) == 0 && ok;
	if(!ok || rename(tmp, path) != 0)
	{
		snprintf(err, errcap, "cannot write %s: %s", path, strerror(errno));
		unlink(tmp);
		return false;
	}
	return true;
}

/* ------------------------------------------------------------------ shared memory writer */

static uint8_t *g_shm;
static RpTapHeader *g_hdr;

#define ST(field, v) __atomic_store_n(&g_hdr->field, (v), __ATOMIC_RELEASE)
#define LD(field) __atomic_load_n(&g_hdr->field, __ATOMIC_ACQUIRE)

static void shm_wake(void)
{
	__atomic_fetch_add(&g_hdr->futex, 1, __ATOMIC_RELEASE);
	syscall(SYS_futex, &g_hdr->futex, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

static void shm_status(const char *fmt, ...)
{
	char buf[sizeof(g_hdr->status)];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	memcpy(g_hdr->status, buf, sizeof(buf));
}

static void shm_set_state(uint32_t state, const char *fmt, ...)
{
	char buf[sizeof(g_hdr->status)];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	memcpy(g_hdr->status, buf, sizeof(buf));
	ST(state, state);
	shm_wake();
}

static bool shm_open_map(const char *name)
{
	char path[256];
	snprintf(path, sizeof(path), "%s%s", name[0] == '/' ? "" : "/", name);
	int fd = shm_open(path, O_CREAT | O_RDWR, 0600);
	if(fd < 0)
	{
		TAP_ERR("shm_open %s: %s", path, strerror(errno));
		return false;
	}
	if(ftruncate(fd, RP_TAP_SIZE) < 0)
	{
		TAP_ERR("ftruncate: %s", strerror(errno));
		close(fd);
		return false;
	}
	g_shm = mmap(NULL, RP_TAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if(g_shm == MAP_FAILED)
	{
		TAP_ERR("mmap: %s", strerror(errno));
		g_shm = NULL;
		return false;
	}
	g_hdr = (RpTapHeader *)g_shm;
	return true;
}

static void shm_init_idle(unsigned bitrate_kbps)
{
	ST(magic, 0u);
	g_hdr->version = RP_TAP_VERSION;
	g_hdr->slot_count = RP_TAP_SLOT_COUNT;
	g_hdr->slot_size = RP_TAP_SLOT_SIZE;
	g_hdr->qpc_freq = 1000000000ull;
	g_hdr->features |= RP_TAP_FEATURE_AUDIO_DELAY;
	g_hdr->writer_pid = (uint32_t)getpid();
	g_hdr->bitrate_kbps = bitrate_kbps;
	g_hdr->audio_out_ms = 0;
	g_hdr->audio_held_ms = 0;
	ST(codec_header_size, 0u);
	shm_set_state(RP_TAP_IDLE, "idle");
}

/* ------------------------------------------------------------------ session globals */

typedef struct
{
	ChiakiSession session;
	bool session_inited;
	ChiakiOpusDecoder opus;
	bool opus_inited;

	// stream state (video callback thread)
	bool stream_on;
	uint64_t seq;
	uint32_t codec;          // RpTapCodec
	uint32_t cfg_width, cfg_height, cfg_fps;
	uint32_t lost_total;
	uint32_t bitrate_kbps;

	// session lifecycle
	atomic_bool connected;
	atomic_bool quit;
	char quit_reason[96];
	int quit_code;           // ChiakiQuitReason, or -1 (not a chiaki quit), -2 login PIN
	atomic_uint rumble;      // bit 31 set: pending; left | right << 8
} G;

static G g;

/* ------------------------------------------------------------------ video */

static inline bool is_vcl(uint32_t codec, uint8_t nal0)
{
	if(codec == RP_TAP_H264)
		return (nal0 & 0x1f) >= 1 && (nal0 & 0x1f) <= 5;
	return ((nal0 >> 1) & 0x3f) < 32;
}

static inline bool is_param_set(uint32_t codec, uint8_t nal0)
{
	if(codec == RP_TAP_H264)
	{
		uint8_t t = nal0 & 0x1f;
		return t == 7 || t == 8;
	}
	uint8_t t = (nal0 >> 1) & 0x3f;
	return t >= 32 && t <= 34;
}

/* Collect parameter-set NAL units (start codes included) from the packet's leading NAL units,
 * stopping at the first coded slice. Returns the number of bytes written to out. */
static size_t extract_codec_header(uint32_t codec, const uint8_t *buf, size_t size, uint8_t *out, size_t cap)
{
	size_t scan = size < 8192 ? size : 8192;
	size_t o = 0;
	size_t i = 0;
	// find start codes
	size_t starts[16];
	int ns = 0;
	for(; i + 3 < scan + 1 && i + 3 <= size && ns < 16; i++)
	{
		if(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
		{
			size_t s = (i > 0 && buf[i - 1] == 0) ? i - 1 : i;
			starts[ns++] = s;
			i += 2;
		}
	}
	for(int k = 0; k < ns; k++)
	{
		size_t s = starts[k];
		size_t nal_pos = s + ((buf[s] == 0 && buf[s + 1] == 0 && buf[s + 2] == 0) ? 4 : 3);
		if(nal_pos >= size)
			break;
		uint8_t n0 = buf[nal_pos];
		if(is_vcl(codec, n0))
			break;
		size_t e = (k + 1 < ns) ? starts[k + 1] : size;
		if(is_param_set(codec, n0))
		{
			if(o + (e - s) > cap)
				break;
			memcpy(out + o, buf + s, e - s);
			o += e - s;
		}
	}
	return o;
}

static void stream_start(void)
{
	ChiakiConnectVideoProfile *vp = &g.session.connect_info.video_profile;
	uint32_t w = vp->width, h = vp->height;
	ChiakiVideoReceiver *vr = g.session.stream_connection.video_receiver;
	if(vr && vr->profile_cur >= 0 && vr->profiles[vr->profile_cur].width)
	{
		w = vr->profiles[vr->profile_cur].width;
		h = vr->profiles[vr->profile_cur].height;
	}
	ST(magic, 0u);
	g_hdr->version = RP_TAP_VERSION;
	g_hdr->slot_count = RP_TAP_SLOT_COUNT;
	g_hdr->slot_size = RP_TAP_SLOT_SIZE;
	g_hdr->codec = g.codec;
	g_hdr->width = w;
	g_hdr->height = h;
	g_hdr->max_fps = vp->max_fps;
	g_hdr->qpc_freq = 1000000000ull;
	g_hdr->frames_lost_total = 0;
	g_hdr->bitrate_kbps = g.bitrate_kbps;
	g_hdr->writer_pid = (uint32_t)getpid();
	ST(codec_header_size, 0u);
	for(uint32_t i = 0; i < RP_TAP_SLOT_COUNT; i++)
		((RpTapSlot *)(g_shm + RP_TAP_HEADER_SIZE + (size_t)i * RP_TAP_SLOT_SIZE))->seq = ~0ull;
	g.seq = 0;
	g.lost_total = 0;
	ST(write_seq, 0ull);
	__atomic_fetch_add(&g_hdr->session, 1, __ATOMIC_RELEASE);
	g.stream_on = true;
}

static void stream_publish(void)
{
	// called after the codec header (if any) is in place: magic last, then STREAMING
	ST(magic, RP_TAP_MAGIC);
	shm_set_state(RP_TAP_STREAMING, "streaming %ux%u %s", g_hdr->width, g_hdr->height, g.codec == RP_TAP_H264 ? "h264" : "h265");
}

static bool video_cb(uint8_t *buf, size_t size, int32_t frames_lost, bool recovered, void *user)
{
	(void)user;
	uint64_t arrival = now_ns();
	bool first = !g.stream_on;
	if(first)
	{
		ChiakiCodec c = g.session.connect_info.video_profile.codec;
		g.codec = (c == CHIAKI_CODEC_H264) ? RP_TAP_H264 : (c == CHIAKI_CODEC_H265_HDR ? RP_TAP_H265_HDR : RP_TAP_H265);
		stream_start();
	}

	// codec header (re-sent on a profile switch)
	{
		uint8_t hdr[RP_TAP_CODEC_HEADER_MAX];
		uint32_t base = g.codec == RP_TAP_H264 ? RP_TAP_H264 : RP_TAP_H265;
		size_t hn = extract_codec_header(base, buf, size, hdr, sizeof(hdr));
		if(hn > 0 && (hn != LD(codec_header_size) || memcmp(hdr, g_hdr->codec_header, hn) != 0))
		{
			ST(codec_header_size, 0u);
			memcpy(g_hdr->codec_header, hdr, hn);
			ST(codec_header_session, LD(session));
			ST(codec_header_size, (uint32_t)hn);
		}
	}

	if(first)
		stream_publish();

	if(frames_lost > 0)
	{
		g.lost_total += (uint32_t)frames_lost;
		ST(frames_lost_total, g.lost_total);
	}

	uint64_t n = g.seq;
	RpTapSlot *slot = (RpTapSlot *)(g_shm + RP_TAP_HEADER_SIZE + (size_t)(n % RP_TAP_SLOT_COUNT) * RP_TAP_SLOT_SIZE);
	__atomic_store_n(&slot->seq, ~0ull, __ATOMIC_RELEASE);
	size_t cap = RP_TAP_SLOT_SIZE - RP_TAP_SLOT_HEADER;
	uint32_t flags = 0;
	size_t cp = size;
	if(cp > cap)
	{
		cp = cap;
		flags |= RP_TAP_SLOT_TRUNCATED;
	}
	slot->arrival_ns = arrival;
	slot->size = (uint32_t)cp;
	slot->frames_lost = frames_lost;
	slot->recovered = recovered ? 1u : 0u;
	slot->flags = flags;
	memcpy((uint8_t *)slot + RP_TAP_SLOT_HEADER, buf, cp);
	__atomic_store_n(&slot->seq, n, __ATOMIC_RELEASE);
	g.seq = n + 1;
	__atomic_store_n(&g_hdr->write_seq, n + 1, __ATOMIC_RELEASE);
	shm_wake();
	return true;
}

/* ------------------------------------------------------------------ audio */

#define AUDIO_JITTER_MS 12u   /* minimum sound held back, against network jitter */
#define AUDIO_CHUNK_MS 5
#define AUDIO_FIFO_MAX_MS 1500

typedef struct
{
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int16_t *fifo;           // interleaved s16, capacity in frames: cap
	size_t cap;              // frames
	size_t head, count;      // frames
	uint32_t channels, rate;
	uint32_t fmt_gen;        // bumped when a (new) format arrives
	bool enabled;
	bool quit;
	pthread_t th;
	bool th_started;
} Audio;

static Audio A = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void audio_settings_cb(uint32_t channels, uint32_t rate, void *user)
{
	(void)user;
	pthread_mutex_lock(&A.mu);
	free(A.fifo);
	A.channels = channels;
	A.rate = rate;
	A.cap = (size_t)rate * AUDIO_FIFO_MAX_MS / 1000;
	A.fifo = calloc(A.cap * channels, sizeof(int16_t));
	A.head = A.count = 0;
	A.fmt_gen++;
	pthread_cond_broadcast(&A.cv);
	pthread_mutex_unlock(&A.mu);
	TAP_INFO("audio format: %u ch, %u Hz", channels, rate);
}

static void audio_frame_cb(int16_t *buf, size_t frames, void *user)
{
	(void)user;
	pthread_mutex_lock(&A.mu);
	if(A.fifo && A.channels)
	{
		if(frames > A.cap)
		{
			buf += (frames - A.cap) * A.channels;
			frames = A.cap;
		}
		if(A.count + frames > A.cap)    // overflow: drop the oldest
		{
			size_t drop = A.count + frames - A.cap;
			A.head = (A.head + drop) % A.cap;
			A.count -= drop;
		}
		size_t tail = (A.head + A.count) % A.cap;
		size_t first = A.cap - tail < frames ? A.cap - tail : frames;
		memcpy(A.fifo + tail * A.channels, buf, first * A.channels * sizeof(int16_t));
		if(first < frames)
			memcpy(A.fifo, buf + first * A.channels, (frames - first) * A.channels * sizeof(int16_t));
		A.count += frames;
	}
	pthread_mutex_unlock(&A.mu);
}

static void *audio_thread(void *arg)
{
	(void)arg;
	pa_simple *pa = NULL;
	uint32_t my_gen = 0;
	uint32_t channels = 0, rate = 0;
	int16_t *chunk = NULL;
	int16_t *silence = NULL;
	size_t chunk_frames = 0;
	bool playing = false;
	int lat_tick = 0;
	uint32_t dev_lat_ms = 0;

	for(;;)
	{
		pthread_mutex_lock(&A.mu);
		while(!A.quit && A.fmt_gen == my_gen)
		{
			if(my_gen != 0)
				break;
			pthread_cond_wait(&A.cv, &A.mu);
		}
		if(A.quit)
		{
			pthread_mutex_unlock(&A.mu);
			break;
		}
		if(A.fmt_gen != my_gen)
		{
			my_gen = A.fmt_gen;
			channels = A.channels;
			rate = A.rate;
			pthread_mutex_unlock(&A.mu);
			if(pa)
			{
				pa_simple_free(pa);
				pa = NULL;
			}
			free(chunk);
			free(silence);
			chunk_frames = (size_t)rate * AUDIO_CHUNK_MS / 1000;
			chunk = calloc(chunk_frames * channels, sizeof(int16_t));
			silence = calloc(chunk_frames * channels, sizeof(int16_t));
			playing = false;
			if(A.enabled)
			{
				pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = rate, .channels = (uint8_t)channels };
				pa_buffer_attr ba;
				ba.maxlength = (uint32_t)-1;
				ba.tlength = (uint32_t)((uint64_t)rate * channels * 2 * 30 / 1000);
				ba.prebuf = (uint32_t)-1;
				ba.minreq = (uint32_t)-1;
				ba.fragsize = (uint32_t)-1;
				int perr = 0;
				pa = pa_simple_new(NULL, "moonglide-chiaki-tap", PA_STREAM_PLAYBACK, NULL, "Remote Play", &ss, NULL, &ba, &perr);
				if(!pa)
					TAP_WARN("PulseAudio unavailable (%s); audio disabled", pa_strerror(perr));
			}
			continue;
		}
		pthread_mutex_unlock(&A.mu);
		if(!pa)
		{
			// nothing to play to: discard so the FIFO does not hold stale sound
			pthread_mutex_lock(&A.mu);
			A.head = A.count = 0;
			pthread_mutex_unlock(&A.mu);
			sleep_ms(20);
			continue;
		}

		if(lat_tick++ % 20 == 0)
		{
			int perr;
			pa_usec_t lat = pa_simple_get_latency(pa, &perr);
			if(lat != (pa_usec_t)-1)
			{
				dev_lat_ms = (uint32_t)(lat / 1000);
				__atomic_store_n(&g_hdr->audio_out_ms, dev_lat_ms, __ATOMIC_RELAXED);
			}
		}

		// wanted hold
		uint32_t delay = __atomic_load_n(&g_hdr->audio_delay_ms, __ATOMIC_ACQUIRE);
		uint64_t hb = __atomic_load_n(&g_hdr->reader_heartbeat, __ATOMIC_ACQUIRE);
		uint64_t t = now_ns();
		uint32_t wanted = 0;
		if(hb != 0 && t >= hb && t - hb <= 2000000000ull)
		{
			int w = (int)(delay > 500 ? 500 : delay) - (int)dev_lat_ms;
			wanted = w < 0 ? 0 : (w > 500 ? 500 : (uint32_t)w);
		}
		// Follow the picture's delay slowly and only past a dead band: every change of the hold
		// means cutting or padding the sound (a click), and moonglide's delay jitters by a few ms.
		// A floor keeps a little in hand for the network's jitter, so the FIFO never runs dry.
		static double wanted_s = -1;
		if(wanted_s < 0 || !playing)
			wanted_s = wanted;
		else if(fabs((double)wanted - wanted_s) > 6.0)
			wanted_s += ((double)wanted - wanted_s) * 0.05;
		uint32_t hold = (uint32_t)(wanted_s + 0.5);
		if(hold < AUDIO_JITTER_MS)
			hold = AUDIO_JITTER_MS;
		size_t T = (size_t)rate * hold / 1000;
		size_t c = chunk_frames;

		bool do_play = false;
		pthread_mutex_lock(&A.mu);
		if(A.fmt_gen != my_gen)
		{
			pthread_mutex_unlock(&A.mu);
			continue;
		}
		if(!playing)
		{
			if(A.count >= T + c)
				playing = true;
		}
		else if(A.count < c)
			playing = false;
		if(playing && A.count + 4 * c < T / 2)   // far too little held (a long network gap): pad with silence
			playing = true, do_play = false;
		else if(playing)
		{
			if(A.count > T + 8 * c)               // hold shrank a lot: drop the oldest, 0.5 ms a step
			{
				size_t drop = A.count - (T + 4 * c);
				size_t maxd = rate / 2000 ? rate / 2000 : 1;
				if(drop > maxd)
					drop = maxd;
				A.head = (A.head + drop) % A.cap;
				A.count -= drop;
			}
			do_play = A.count >= c;
			if(do_play)
			{
				size_t first = A.cap - A.head < c ? A.cap - A.head : c;
				memcpy(chunk, A.fifo + A.head * channels, first * channels * sizeof(int16_t));
				if(first < c)
					memcpy(chunk + first * channels, A.fifo, (c - first) * channels * sizeof(int16_t));
				A.head = (A.head + c) % A.cap;
				A.count -= c;
			}
		}
		size_t held = A.count;
		pthread_mutex_unlock(&A.mu);
		__atomic_store_n(&g_hdr->audio_held_ms, (uint32_t)(held * 1000 / rate), __ATOMIC_RELAXED);

		int perr = 0;
		if(pa_simple_write(pa, do_play ? chunk : silence, chunk_frames * channels * sizeof(int16_t), &perr) < 0)
		{
			TAP_WARN("PulseAudio write failed (%s); audio disabled", pa_strerror(perr));
			pa_simple_free(pa);
			pa = NULL;
		}
	}
	if(pa)
		pa_simple_free(pa);
	free(chunk);
	free(silence);
	return NULL;
}

/* ------------------------------------------------------------------ controllers */

#define MAX_PADS 8

typedef struct
{
	SDL_GameController *gc;
	SDL_JoystickID id;
	ChiakiControllerState st;
	bool ds;                       // DualSense / DualSense Edge (SDL_CONTROLLER_TYPE_PS5)
	bool has_led;
	uint16_t fw;                   // firmware version as SDL reports it (0 = unknown, common over Bluetooth)
	uint64_t apply_at_ns;          // lightbar / player LED / mic LED are re-applied 1 s after the pad appears
	ChiakiOrientationTracker ot;
	ChiakiAccelNewZero accel_zero, real_accel;
	uint32_t last_motion_us;
	int8_t touch[2];               // SDL finger (touchpad 0) -> chiaki touch id, -1 = none
} Pad;

static Pad g_pads[MAX_PADS];
static void pad_setup_extras(Pad *p);
static bool any_dualsense_pad(void);
static void haptics_close(void);

static void pad_add(int device_index)
{
	if(!SDL_IsGameController(device_index))
		return;
	SDL_GameController *gc = SDL_GameControllerOpen(device_index);
	if(!gc)
		return;
	SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
	for(int i = 0; i < MAX_PADS; i++)
		if(g_pads[i].gc && g_pads[i].id == id)
		{
			SDL_GameControllerClose(gc);
			return;
		}
	for(int i = 0; i < MAX_PADS; i++)
	{
		if(!g_pads[i].gc)
		{
			g_pads[i].gc = gc;
			g_pads[i].id = id;
			chiaki_controller_state_set_idle(&g_pads[i].st);
			pad_setup_extras(&g_pads[i]);
			TAP_INFO("controller added: %s%s", SDL_GameControllerName(gc), g_pads[i].ds ? " (DualSense)" : "");
			return;
		}
	}
	SDL_GameControllerClose(gc);
}

static void pad_remove(SDL_JoystickID id)
{
	for(int i = 0; i < MAX_PADS; i++)
		if(g_pads[i].gc && g_pads[i].id == id)
		{
			TAP_INFO("controller removed: %s", SDL_GameControllerName(g_pads[i].gc));
			SDL_GameControllerClose(g_pads[i].gc);
			g_pads[i].gc = NULL;
			chiaki_controller_state_set_idle(&g_pads[i].st);
			if(!any_dualsense_pad())
				haptics_close();
		}
}

static Pad *pad_find(SDL_JoystickID id)
{
	for(int i = 0; i < MAX_PADS; i++)
		if(g_pads[i].gc && g_pads[i].id == id)
			return &g_pads[i];
	return NULL;
}

// Mapping as chiaki-ng gui/src/controllermanager.cpp Controller::HandleButtonEvent/HandleAxisEvent
static bool pad_button(Pad *p, const SDL_ControllerButtonEvent *e)
{
	uint32_t b;
	switch(e->button)
	{
		case SDL_CONTROLLER_BUTTON_A: b = CHIAKI_CONTROLLER_BUTTON_CROSS; break;
		case SDL_CONTROLLER_BUTTON_B: b = CHIAKI_CONTROLLER_BUTTON_MOON; break;
		case SDL_CONTROLLER_BUTTON_X: b = CHIAKI_CONTROLLER_BUTTON_BOX; break;
		case SDL_CONTROLLER_BUTTON_Y: b = CHIAKI_CONTROLLER_BUTTON_PYRAMID; break;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: b = CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT; break;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: b = CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT; break;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: b = CHIAKI_CONTROLLER_BUTTON_DPAD_UP; break;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: b = CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN; break;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: b = CHIAKI_CONTROLLER_BUTTON_L1; break;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: b = CHIAKI_CONTROLLER_BUTTON_R1; break;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK: b = CHIAKI_CONTROLLER_BUTTON_L3; break;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK: b = CHIAKI_CONTROLLER_BUTTON_R3; break;
		case SDL_CONTROLLER_BUTTON_START: b = CHIAKI_CONTROLLER_BUTTON_OPTIONS; break;
		case SDL_CONTROLLER_BUTTON_BACK: b = CHIAKI_CONTROLLER_BUTTON_SHARE; break;
		case SDL_CONTROLLER_BUTTON_GUIDE: b = CHIAKI_CONTROLLER_BUTTON_PS; break;
#if SDL_VERSION_ATLEAST(2, 0, 14)
		case SDL_CONTROLLER_BUTTON_TOUCHPAD: b = CHIAKI_CONTROLLER_BUTTON_TOUCHPAD; break;
#endif
		default: return false;
	}
	if(e->type == SDL_CONTROLLERBUTTONDOWN)
		p->st.buttons |= b;
	else
		p->st.buttons &= ~b;
	return true;
}

static bool pad_axis(Pad *p, const SDL_ControllerAxisEvent *e)
{
	switch(e->axis)
	{
		case SDL_CONTROLLER_AXIS_TRIGGERLEFT: p->st.l2_state = (uint8_t)(e->value >> 7); break;
		case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: p->st.r2_state = (uint8_t)(e->value >> 7); break;
		case SDL_CONTROLLER_AXIS_LEFTX: p->st.left_x = e->value; break;
		case SDL_CONTROLLER_AXIS_LEFTY: p->st.left_y = e->value; break;
		case SDL_CONTROLLER_AXIS_RIGHTX: p->st.right_x = e->value; break;
		case SDL_CONTROLLER_AXIS_RIGHTY: p->st.right_y = e->value; break;
		default: return false;
	}
	return true;
}


/* Physical pads merged as the console gets them: buttons OR, triggers max, sticks largest magnitude
 * (chiaki_controller_state_or). */
static void controller_merged(ChiakiControllerState *out)
{
	chiaki_controller_state_set_idle(out);
	for(int i = 0; i < MAX_PADS; i++)
		if(g_pads[i].gc)
		{
			ChiakiControllerState tmp;
			chiaki_controller_state_or(&tmp, out, &g_pads[i].st);
			*out = tmp;
		}
}

/* Camera-stick onset for the app's input-lag measurement (RpTapStick in rp_tap.h): the right stick
 * of the state sent to the console goes from neutral (|v| < 0.1 for >= 300 ms) to deflected
 * (|v| > 0.5, reached within 250 ms of leaving neutral). Stamped when the state is sent. */
static void stick_watch(const ChiakiControllerState *st, uint64_t sent_ns)
{
	static uint64_t neutral_since, left_at, dz_at;
	static bool armed;
	if(!g_shm)
		return;
	const float fx = st->right_x / 32768.0f, fy = st->right_y / 32768.0f;
	const float mag2 = fx * fx + fy * fy;
	if(mag2 < 0.01f)
	{
		if(!neutral_since)
			neutral_since = sent_ns;
		armed = false;
		return;
	}
	if(neutral_since)
	{
		armed = sent_ns - neutral_since >= 300000000ull;
		left_at = sent_ns;
		dz_at = 0;
		neutral_since = 0;
	}
	/* The onset is when the stick passed a quarter of its travel, about where games' dead zones
	 * end: earlier the game does not react yet, later would hide part of the lag. */
	if(!dz_at && mag2 > 0.0625f)
		dz_at = sent_ns;
	if(armed && sent_ns - left_at > 250000000ull)
		armed = false;
	if(armed && mag2 > 0.25f)
	{
		armed = false;
		volatile RpTapStick *k = (volatile RpTapStick *)(g_shm + RP_TAP_STICK_OFFSET);
		k->x = st->right_x;
		k->y = st->right_y;
		k->onset_ns = dz_at ? dz_at : sent_ns;
		__atomic_fetch_add(&k->seq, 1u, __ATOMIC_RELEASE);
	}
}

/* ------------------------------------------------------------------ DualSense features
 * Ported from chiaki-ng (gui/src/controllermanager.cpp, streamsession.cpp): output report for
 * rumble / adaptive triggers / mic LED through SDL_GameControllerSendEffect, motion (gyro, accel,
 * orientation tracker), touchpad, lightbar + player index, haptics.
 * Haptics: a USB DualSense exposes a 4-channel audio device (the haptics are channels 3/4) which is
 * fed through SDL audio. Over Bluetooth there is no such device (neither for chiaki-ng), so the
 * haptic stream is turned into rumble ("haptics to rumble"). All SDL calls happen on the pump
 * thread; the session threads only set state in `fx`. */

typedef struct __attribute__((packed))
{
	uint8_t ucEnableBits1, ucEnableBits2, ucRumbleRight, ucRumbleLeft;
	uint8_t ucHeadphoneVolume, ucSpeakerVolume, ucMicrophoneVolume, ucAudioEnableBits;
	uint8_t ucMicLightMode, ucAudioMuteBits;
	uint8_t rgucRightTriggerEffect[11];
	uint8_t rgucLeftTriggerEffect[11];
	uint8_t rgucUnknown1[6];
	uint8_t ucEnableBits3;
	uint8_t rgucUnknown2[2];
	uint8_t ucLedAnim, ucLedBrightness, ucPadLights, ucLedRed, ucLedGreen, ucLedBlue;
} DS5Effects;
_Static_assert(sizeof(DS5Effects) == 47, "DS5 effects report layout");

#define PS_TOUCHPAD_MAXX 1920
#define PS_TOUCHPAD_MAXY 1079
#define HAPTIC_RUMBLE_MIN_STRENGTH 100
#define HAPTIC_RUMBLE_PACKETS 3          // 3 x 10 ms haptic frames are averaged into one rumble value

static struct
{
	pthread_mutex_t mu;
	bool trig_pending, led_pending, pidx_pending, motion_reset;
	uint8_t tl_type, tr_type, tl[10], tr[10];
	uint8_t led[3], pidx;
	bool led_known, pidx_known;
	atomic_int rumble_int;        // -1 = console turned rumble off, else the PS5 intensity nibble
	atomic_int trigger_int;       // -1 = triggers off
	atomic_uint ds_intensity;     // trigger | rumble nibbles, goes into every effects report
	atomic_uint rumble_mult;      // percent, for non-DualSense pads
	atomic_bool ds_session;       // this session was started with enable_dualsense
	atomic_uint hap_sum, hap_cnt; // haptics-to-rumble accumulator
} fx = { .mu = PTHREAD_MUTEX_INITIALIZER };

static SDL_AudioDeviceID g_hap_dev;        // set/cleared by the pump thread only
static atomic_uint g_hap_dev_a;            // mirror for the haptics callback thread
static SDL_AudioCVT g_hap_cvt;
static uint8_t *g_hap_buf;
static bool g_hap_cvt_ok;
static bool g_hap_audio_init;
static uint64_t g_hap_next_try_ns;
static unsigned g_hap_tries;
static uint64_t g_hap_last_ns;
static bool g_hap_rumble_on;

static void fx_reset(bool ds)
{
	pthread_mutex_lock(&fx.mu);
	fx.trig_pending = fx.led_pending = fx.pidx_pending = fx.motion_reset = false;
	fx.led_known = fx.pidx_known = false;
	pthread_mutex_unlock(&fx.mu);
	atomic_store(&fx.rumble_int, 0);
	atomic_store(&fx.trigger_int, 0);
	atomic_store(&fx.ds_intensity, 0);
	atomic_store(&fx.rumble_mult, 100);
	atomic_store(&fx.hap_sum, 0);
	atomic_store(&fx.hap_cnt, 0);
	atomic_store(&fx.ds_session, ds);
	g_hap_tries = 0;
	g_hap_next_try_ns = 0;
}

static bool any_dualsense_pad(void)
{
	for(int i = 0; i < MAX_PADS; i++)
		if(g_pads[i].gc && g_pads[i].ds)
			return true;
	return false;
}

/* True when a DualSense is attached right now (checked straight against SDL, no event needed). */
static bool dualsense_attached(void)
{
	for(int i = 0; i < SDL_NumJoysticks(); i++)
		if(SDL_IsGameController(i) && SDL_GameControllerTypeForIndex(i) == SDL_CONTROLLER_TYPE_PS5)
			return true;
	return false;
}

static void pad_send_effects(Pad *p, DS5Effects *e)
{
	SDL_GameControllerSendEffect(p->gc, e, sizeof(*e));
}

static void pad_ds_rumble(Pad *p, uint8_t left, uint8_t right)
{
	DS5Effects e;
	memset(&e, 0, sizeof(e));
	// Same rule as SDL's DualSense driver: firmware 0 (often all that is read over Bluetooth) and
	// newer firmware take the "current rumble" bits
	if(p->fw != 0 && p->fw < 0x0224)
	{
		e.ucEnableBits1 |= 0x01;
		e.ucRumbleLeft = left >> 1;
		e.ucRumbleRight = right >> 1;
	}
	else
	{
		e.ucEnableBits3 |= 0x04;
		e.ucRumbleLeft = left;
		e.ucRumbleRight = right;
	}
	e.rgucUnknown1[4] = (uint8_t)atomic_load(&fx.ds_intensity);
	e.ucEnableBits2 |= 0x40;
	e.ucEnableBits1 |= 0x02;
	pad_send_effects(p, &e);
}

static void pad_ds_triggers(Pad *p, uint8_t tl_type, const uint8_t *tl, uint8_t tr_type, const uint8_t *tr)
{
	DS5Effects e;
	memset(&e, 0, sizeof(e));
	e.rgucUnknown1[4] = (uint8_t)atomic_load(&fx.ds_intensity);
	e.ucEnableBits2 |= 0x40;
	e.ucEnableBits1 |= 0x04 | 0x08;
	e.rgucLeftTriggerEffect[0] = tl_type;
	memcpy(e.rgucLeftTriggerEffect + 1, tl, 10);
	e.rgucRightTriggerEffect[0] = tr_type;
	memcpy(e.rgucRightTriggerEffect + 1, tr, 10);
	pad_send_effects(p, &e);
}

static void pad_ds_mic(Pad *p, bool muted)
{
	DS5Effects e;
	memset(&e, 0, sizeof(e));
	e.ucEnableBits2 |= 0x01 | 0x02;
	e.ucMicLightMode = muted ? 0x01 : 0x00;
	e.ucAudioMuteBits = muted ? 0x08 : 0x00;
	pad_send_effects(p, &e);
}

/* console rumble (0..255 per motor) */
static void pad_rumble(Pad *p, uint8_t left, uint8_t right)
{
	if(p->ds)
		pad_ds_rumble(p, left, right);
	else
	{
		unsigned m = atomic_load(&fx.rumble_mult);
		left = (uint8_t)(left * m / 100);
		right = (uint8_t)(right * m / 100);
		SDL_GameControllerRumble(p->gc, (uint16_t)(left << 8), (uint16_t)(right << 8), 500);
	}
}

static void pad_setup_extras(Pad *p)
{
	p->ds = SDL_GameControllerGetType(p->gc) == SDL_CONTROLLER_TYPE_PS5;
	p->has_led = SDL_GameControllerHasLED(p->gc);
	p->fw = SDL_GameControllerGetFirmwareVersion(p->gc);
	p->apply_at_ns = now_ns() + 1000000000ull;
	p->touch[0] = p->touch[1] = -1;
	chiaki_orientation_tracker_init(&p->ot);
	chiaki_accel_new_zero_set_inactive(&p->accel_zero, false);
	chiaki_accel_new_zero_set_inactive(&p->real_accel, true);
	p->last_motion_us = 0;
	if(SDL_GameControllerHasSensor(p->gc, SDL_SENSOR_ACCEL))
		SDL_GameControllerSetSensorEnabled(p->gc, SDL_SENSOR_ACCEL, SDL_TRUE);
	if(SDL_GameControllerHasSensor(p->gc, SDL_SENSOR_GYRO))
		SDL_GameControllerSetSensorEnabled(p->gc, SDL_SENSOR_GYRO, SDL_TRUE);
}

static void pad_motion_reset(Pad *p)
{
	chiaki_accel_new_zero_set_active(&p->accel_zero, p->real_accel.accel_x, p->real_accel.accel_y, p->real_accel.accel_z, false);
	chiaki_orientation_tracker_init(&p->ot);
	chiaki_orientation_tracker_update(&p->ot, p->st.gyro_x, p->st.gyro_y, p->st.gyro_z,
		p->real_accel.accel_x, p->real_accel.accel_y, p->real_accel.accel_z, &p->accel_zero, false, p->last_motion_us);
	chiaki_orientation_tracker_apply_to_controller_state(&p->ot, &p->st);
}

static bool pad_sensor(Pad *p, const SDL_ControllerSensorEvent *e)
{
	float ax, ay, az;
	uint32_t ts = (uint32_t)e->timestamp_us;
	switch(e->sensor)
	{
		case SDL_SENSOR_ACCEL:
			ax = e->data[0] / SDL_STANDARD_GRAVITY;
			ay = e->data[1] / SDL_STANDARD_GRAVITY;
			az = e->data[2] / SDL_STANDARD_GRAVITY;
			chiaki_accel_new_zero_set_active(&p->real_accel, ax, ay, az, true);
			chiaki_orientation_tracker_update(&p->ot, p->st.gyro_x, p->st.gyro_y, p->st.gyro_z,
				ax, ay, az, &p->accel_zero, false, ts);
			break;
		case SDL_SENSOR_GYRO:
			chiaki_orientation_tracker_update(&p->ot, e->data[0], e->data[1], e->data[2],
				p->st.accel_x, p->st.accel_y, p->st.accel_z, &p->accel_zero, true, ts);
			break;
		default:
			return false;
	}
	p->last_motion_us = ts;
	chiaki_orientation_tracker_apply_to_controller_state(&p->ot, &p->st);
	return true;
}

static bool pad_touchpad(Pad *p, const SDL_ControllerTouchpadEvent *e)
{
	if(e->touchpad != 0 || e->finger < 0 || e->finger >= 2)
		return false;
	uint16_t x = (uint16_t)(e->x * PS_TOUCHPAD_MAXX), y = (uint16_t)(e->y * PS_TOUCHPAD_MAXY);
	int8_t *id = &p->touch[e->finger];
	switch(e->type)
	{
		case SDL_CONTROLLERTOUCHPADDOWN:
			if(*id >= 0)
				chiaki_controller_state_stop_touch(&p->st, (uint8_t)*id);
			*id = chiaki_controller_state_start_touch(&p->st, x, y);
			return true;
		case SDL_CONTROLLERTOUCHPADMOTION:
			if(*id < 0)
				return false;
			chiaki_controller_state_set_touch_pos(&p->st, (uint8_t)*id, x, y);
			return true;
		case SDL_CONTROLLERTOUCHPADUP:
			if(*id < 0)
				return false;
			chiaki_controller_state_stop_touch(&p->st, (uint8_t)*id);
			*id = -1;
			return true;
		default:
			return false;
	}
}

/* ---- haptics */

static void haptics_close(void)
{
	atomic_store(&g_hap_dev_a, 0);
	if(g_hap_dev)
	{
		SDL_CloseAudioDevice(g_hap_dev);
		g_hap_dev = 0;
	}
}

static bool any_ds_wired_or_unknown(void)
{
	for(int i = 0; i < MAX_PADS; i++)
	{
		if(!g_pads[i].gc || !g_pads[i].ds)
			continue;
		switch(SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(g_pads[i].gc)))
		{
			case SDL_JOYSTICK_POWER_EMPTY:
			case SDL_JOYSTICK_POWER_LOW:
			case SDL_JOYSTICK_POWER_MEDIUM:
			case SDL_JOYSTICK_POWER_FULL:
				break;                 // runs on battery: Bluetooth (or at least no cable proof)
			default:
				return true;
		}
	}
	return false;
}

/* Try to open the DualSense's USB audio device (haptics on channels 3/4). Pump thread only. */
static void haptics_try_open(void)
{
	if(g_hap_dev || !atomic_load(&fx.ds_session) || !any_dualsense_pad())
		return;
	uint64_t t = now_ns();
	if(t < g_hap_next_try_ns || g_hap_tries >= 12)
		return;
	g_hap_next_try_ns = t + 2000000000ull;
	// a pad on battery has no audio device; only keep looking while a cable is possible
	if(!any_ds_wired_or_unknown())
		return;
	g_hap_tries++;
	if(!g_hap_audio_init)
	{
		SDL_SetHint(SDL_HINT_AUDIODRIVER, "pipewire,pulseaudio");   // chiaki-ng: haptics are most reliable on PipeWire
		if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		{
			TAP_WARN("DualSense haptics: SDL audio unavailable (%s)", SDL_GetError());
			g_hap_tries = 99;
			return;
		}
		g_hap_audio_init = true;
	}
	if(!g_hap_cvt_ok)
	{
		if(SDL_BuildAudioCVT(&g_hap_cvt, AUDIO_S16SYS, 4, 3000, AUDIO_S16SYS, 4, 48000) < 0)
		{
			TAP_WARN("DualSense haptics: audio converter failed (%s)", SDL_GetError());
			g_hap_tries = 99;
			return;
		}
		g_hap_cvt.len = 240;
		g_hap_buf = calloc((size_t)g_hap_cvt.len * (size_t)g_hap_cvt.len_mult, 1);
		if(!g_hap_buf)
		{
			g_hap_tries = 99;
			return;
		}
		g_hap_cvt_ok = true;
	}
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 48000;
	want.format = AUDIO_S16SYS;
	want.channels = 4;
	want.samples = 480;
	for(int i = 0; i < SDL_GetNumAudioDevices(0); i++)
	{
		const char *name = SDL_GetAudioDeviceName(i, 0);
		if(!name || !strstr(name, "DualSense"))
			continue;
		g_hap_dev = SDL_OpenAudioDevice(name, 0, &want, &have, 0);
		if(!g_hap_dev)
		{
			TAP_WARN("DualSense haptics: cannot open '%s' (%s)", name, SDL_GetError());
			continue;
		}
		SDL_PauseAudioDevice(g_hap_dev, 0);
		atomic_store(&g_hap_dev_a, (unsigned)g_hap_dev);
		TAP_INFO("DualSense haptics over audio device '%s' (%d ch, %d Hz)", name, have.channels, have.freq);
		// as chiaki-ng: clear the trigger effects once the haptics path is up
		static const uint8_t clear[10];
		for(int k = 0; k < MAX_PADS; k++)
			if(g_pads[k].gc && g_pads[k].ds)
				pad_ds_triggers(&g_pads[k], 0x05, clear, 0x05, clear);
		return;
	}
}

/* Session thread: one 10 ms haptics frame (stereo S16, 3 kHz). */
static void haptics_cb(uint8_t *buf, size_t size, void *user)
{
	(void)user;
	if(size == 0 || size % 4 != 0 || atomic_load(&fx.rumble_int) < 0)
		return;
	unsigned dev = atomic_load(&g_hap_dev_a);
	if(dev)
	{
		if(!g_hap_buf || size * 2 > 240u * (unsigned)g_hap_cvt.len_mult)
			return;
		SDL_AudioCVT cvt = g_hap_cvt;
		cvt.len = (int)size * 2;
		cvt.buf = g_hap_buf;
		// remix stereo to 4 channels: haptics go to channels 3/4
		for(size_t i = 0; i < size; i += 4)
		{
			memset(g_hap_buf + i * 2, 0, 4);
			memcpy(g_hap_buf + i * 2 + 4, buf + i, 4);
		}
		if(SDL_ConvertAudio(&cvt) != 0)
			return;
		if(SDL_GetQueuedAudioSize(dev) > 480u * 4 * 2 * 6)   // keep at most 60 ms queued
			SDL_ClearQueuedAudio(dev);
		SDL_QueueAudio(dev, cvt.buf, (Uint32)cvt.len_cvt);
		return;
	}
	// no audio device (Bluetooth): haptics-to-rumble, as chiaki-ng at "Normal" intensity
	size_t n = size / 4;
	uint32_t suml = 0, sumr = 0;
	for(size_t i = 0; i < n; i++)
	{
		int16_t l, r;
		memcpy(&l, buf + i * 4, 2);
		memcpy(&r, buf + i * 4 + 2, 2);
		suml += (uint32_t)abs((int)l) * 2;
		sumr += (uint32_t)abs((int)r) * 2;
	}
	uint32_t l = suml / (uint32_t)n, r = sumr / (uint32_t)n;
	l = l > HAPTIC_RUMBLE_MIN_STRENGTH ? l : 0;
	r = r > HAPTIC_RUMBLE_MIN_STRENGTH ? r : 0;
	uint32_t m = l > r ? l : r;
	if(m > 0xffffu) m = 0xffffu;
	if(m > 0 && m < (1u << 9)) m = 1u << 9;
	atomic_fetch_add(&fx.hap_sum, m);
	atomic_fetch_add(&fx.hap_cnt, 1);
}

/* every ~30 ms: apply the averaged haptic strength as rumble */
static void haptics_rumble_tick(uint64_t t)
{
	if(!atomic_load(&fx.ds_session) || atomic_load(&g_hap_dev_a))
		return;
	if(t - g_hap_last_ns < 10000000ull * HAPTIC_RUMBLE_PACKETS)
		return;
	g_hap_last_ns = t;
	uint32_t sum = atomic_exchange(&fx.hap_sum, 0);
	atomic_store(&fx.hap_cnt, 0);
	uint32_t strength = sum / HAPTIC_RUMBLE_PACKETS;
	if(strength > 0xffffu) strength = 0xffffu;
	if(strength == 0 && !g_hap_rumble_on)
		return;
	for(int i = 0; i < MAX_PADS; i++)
	{
		if(!g_pads[i].gc)
			continue;
		if(g_pads[i].ds)
			pad_ds_rumble(&g_pads[i], (uint8_t)(strength >> 8), (uint8_t)(strength >> 8));
		else
			SDL_GameControllerRumble(g_pads[i].gc, (uint16_t)strength, (uint16_t)strength, 500);
	}
	g_hap_rumble_on = strength > 0;
}

/* apply what the session threads queued (pump thread) */
static void fx_apply(uint64_t t)
{
	bool trig, led, pidx, mreset;
	uint8_t tlt, trt, tl[10], tr[10], rgb[3], idx;
	pthread_mutex_lock(&fx.mu);
	trig = fx.trig_pending; led = fx.led_pending; pidx = fx.pidx_pending; mreset = fx.motion_reset;
	tlt = fx.tl_type; trt = fx.tr_type;
	memcpy(tl, fx.tl, 10); memcpy(tr, fx.tr, 10); memcpy(rgb, fx.led, 3); idx = fx.pidx;
	fx.trig_pending = fx.led_pending = fx.pidx_pending = fx.motion_reset = false;
	pthread_mutex_unlock(&fx.mu);

	for(int i = 0; i < MAX_PADS; i++)
	{
		Pad *p = &g_pads[i];
		if(!p->gc)
			continue;
		if(trig && p->ds)
			pad_ds_triggers(p, tlt, tl, trt, tr);
		if(led && p->has_led)
			SDL_GameControllerSetLED(p->gc, rgb[0], rgb[1], rgb[2]);
		if(pidx)
			SDL_GameControllerSetPlayerIndex(p->gc, idx);
		if(mreset)
			pad_motion_reset(p);
		if(p->apply_at_ns && t >= p->apply_at_ns)
		{
			// a freshly attached pad: SDL's own init would overwrite these, so re-apply after 1 s
			p->apply_at_ns = 0;
			uint8_t rgb2[3], idx2;
			pthread_mutex_lock(&fx.mu);
			bool lk = fx.led_known, pk = fx.pidx_known;
			memcpy(rgb2, fx.led, 3); idx2 = fx.pidx;
			pthread_mutex_unlock(&fx.mu);
			if(lk && p->has_led)
				SDL_GameControllerSetLED(p->gc, rgb2[0], rgb2[1], rgb2[2]);
			if(pk)
				SDL_GameControllerSetPlayerIndex(p->gc, idx2);
			if(p->ds && atomic_load(&fx.ds_session))
				pad_ds_mic(p, true);   // this client sends no microphone: show the mic as muted
		}
	}
}

/* session ended / connection lost: stop any rumble and trigger resistance */
static void fx_stop_all(void)
{
	static const uint8_t clear[10];
	for(int i = 0; i < MAX_PADS; i++)
	{
		if(!g_pads[i].gc)
			continue;
		if(g_pads[i].ds)
		{
			pad_ds_rumble(&g_pads[i], 0, 0);
			pad_ds_triggers(&g_pads[i], 0x05, clear, 0x05, clear);
		}
		else
			SDL_GameControllerRumble(g_pads[i].gc, 0, 0, 0);
	}
	g_hap_rumble_on = false;
	haptics_close();
}

/* session thread: console -> controller events */
static void fx_event(ChiakiEvent *ev)
{
	switch(ev->type)
	{
		case CHIAKI_EVENT_LED_COLOR:
			pthread_mutex_lock(&fx.mu);
			memcpy(fx.led, ev->led_state, 3);
			fx.led_pending = fx.led_known = true;
			pthread_mutex_unlock(&fx.mu);
			break;
		case CHIAKI_EVENT_PLAYER_INDEX:
			pthread_mutex_lock(&fx.mu);
			fx.pidx = ev->player_index;
			fx.pidx_pending = fx.pidx_known = true;
			pthread_mutex_unlock(&fx.mu);
			break;
		case CHIAKI_EVENT_MOTION_RESET:
			pthread_mutex_lock(&fx.mu);
			fx.motion_reset = true;
			pthread_mutex_unlock(&fx.mu);
			break;
		case CHIAKI_EVENT_TRIGGER_EFFECTS:
			if(atomic_load(&fx.trigger_int) < 0)
				break;
			pthread_mutex_lock(&fx.mu);
			fx.tl_type = ev->trigger_effects.type_left;
			fx.tr_type = ev->trigger_effects.type_right;
			memcpy(fx.tl, ev->trigger_effects.left, 10);
			memcpy(fx.tr, ev->trigger_effects.right, 10);
			fx.trig_pending = true;
			pthread_mutex_unlock(&fx.mu);
			break;
		case CHIAKI_EVENT_HAPTIC_INTENSITY:
		case CHIAKI_EVENT_TRIGGER_INTENSITY:
		{
			int v;
			unsigned mult = 100;
			bool haptic = ev->type == CHIAKI_EVENT_HAPTIC_INTENSITY;
			switch(ev->intensity)
			{
				case Off:    v = -1; mult = 0; break;
				case Weak:   v = haptic ? 0x03 : 0x90; mult = 33; break;
				case Medium: v = haptic ? 0x02 : 0x60; mult = 50; break;
				default:     v = 0x00; break;
			}
			if(haptic)
			{
				atomic_store(&fx.rumble_int, v);
				atomic_store(&fx.rumble_mult, mult);
			}
			else
				atomic_store(&fx.trigger_int, v);
			int t = atomic_load(&fx.trigger_int), r = atomic_load(&fx.rumble_int);
			atomic_store(&fx.ds_intensity, (unsigned)(((t < 0) ? 0xF0 : t) | ((r < 0) ? 0x0F : r)));
			break;
		}
		default:
			break;
	}
}

/* ------------------------------------------------------------------ session events */

static void event_cb(ChiakiEvent *ev, void *user)
{
	(void)user;
	switch(ev->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			atomic_store(&g.connected, true);
			ST(rtt_us, (uint32_t)g.session.rtt_us);
			shm_status("connected, waiting for video");
			TAP_INFO("session connected");
			break;
		case CHIAKI_EVENT_QUIT:
			snprintf(g.quit_reason, sizeof(g.quit_reason), "%s%s%s", chiaki_quit_reason_string(ev->quit.reason),
				ev->quit.reason_str ? ": " : "", ev->quit.reason_str ? ev->quit.reason_str : "");
			g.quit_code = (int)ev->quit.reason;
			atomic_store(&g.quit, true);
			break;
		case CHIAKI_EVENT_RUMBLE:
			if(atomic_load(&fx.rumble_int) >= 0)
				atomic_store(&g.rumble, 0x80000000u | ev->rumble.left | ((unsigned)ev->rumble.right << 8));
			break;
		case CHIAKI_EVENT_LED_COLOR:
		case CHIAKI_EVENT_PLAYER_INDEX:
		case CHIAKI_EVENT_MOTION_RESET:
		case CHIAKI_EVENT_TRIGGER_EFFECTS:
		case CHIAKI_EVENT_HAPTIC_INTENSITY:
		case CHIAKI_EVENT_TRIGGER_INTENSITY:
			fx_event(ev);
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			snprintf(g.quit_reason, sizeof(g.quit_reason), "console asks for a login PIN (not supported)");
			g.quit_code = -2;
			atomic_store(&g.quit, true);
			break;
		default:
			break;
	}
}

/* ------------------------------------------------------------------ discovery */

typedef struct
{
	atomic_int state;        // ChiakiDiscoveryHostState + 1, 0 = no answer
	char name[64], type[32], version[32];
} DiscResult;

static void discovery_cb(ChiakiDiscoveryHost *host, void *user)
{
	DiscResult *r = user;
	snprintf(r->name, sizeof(r->name), "%s", host->host_name ? host->host_name : "");
	snprintf(r->type, sizeof(r->type), "%s", host->host_type ? host->host_type : "");
	snprintf(r->version, sizeof(r->version), "%s", host->system_version ? host->system_version : "");
	atomic_store(&r->state, (int)host->state + 1);
}

/* Passive UDP status query (SRCH). Returns 0 no answer, else ChiakiDiscoveryHostState+1. */
static int discover(const char *host, bool ps5, unsigned timeout_ms, DiscResult *r)
{
	ChiakiDiscovery d;
	if(chiaki_discovery_init(&d, &g_log, AF_INET) != CHIAKI_ERR_SUCCESS)
		return 0;
	ChiakiDiscoveryThread th;
	atomic_store(&r->state, 0);
	if(chiaki_discovery_thread_start(&th, &d, discovery_cb, r) != CHIAKI_ERR_SUCCESS)
	{
		chiaki_discovery_fini(&d);
		return 0;
	}
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(ps5 ? CHIAKI_DISCOVERY_PORT_PS5 : CHIAKI_DISCOVERY_PORT_PS4) };
	if(inet_pton(AF_INET, host, &sa.sin_addr) != 1)
	{
		TAP_ERR("--host must be an IPv4 address for discovery");
		chiaki_discovery_thread_stop(&th);
		chiaki_discovery_fini(&d);
		return 0;
	}
	ChiakiDiscoveryPacket p = { 0 };
	p.cmd = CHIAKI_DISCOVERY_CMD_SRCH;
	p.protocol_version = ps5 ? CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5 : CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4;
	uint64_t t0 = now_ns();
	while(!atomic_load(&r->state) && !g_sig_stop && (now_ns() - t0) / 1000000 < timeout_ms)
	{
		chiaki_discovery_send(&d, &p, (struct sockaddr *)&sa, sizeof(sa));
		for(int i = 0; i < 25 && !atomic_load(&r->state); i++)
			sleep_ms(20);
	}
	chiaki_discovery_thread_stop(&th);
	chiaki_discovery_fini(&d);
	return atomic_load(&r->state);
}

enum { DISC_NO_ANSWER = 0, DISC_UNKNOWN = 1, DISC_READY = 2, DISC_STANDBY = 3 };   // discover()'s results

static const char *disc_state_str(int s)
{
	return s == 0 ? "no answer" : s == 1 ? "unknown" : s == 2 ? "ready" : "standby";
}


/* ------------------------------------------------------------------ pairing / setup modes (JSON lines) */

#define MAX_FOUND 32

typedef struct
{
	char addr[48], name[64], type[16], version[32], host_id[32];
	int state;   // ChiakiDiscoveryHostState + 1
} Found;

typedef struct
{
	pthread_mutex_t mu;
	Found f[MAX_FOUND];
	int n;
} FoundList;

static void found_cb(ChiakiDiscoveryHost *host, void *user)
{
	FoundList *fl = user;
	pthread_mutex_lock(&fl->mu);
	Found *e = NULL;
	for(int i = 0; i < fl->n; i++)
		if(host->host_addr && !strcmp(fl->f[i].addr, host->host_addr))
			e = &fl->f[i];
	if(!e && fl->n < MAX_FOUND)
		e = &fl->f[fl->n++];
	if(e)
	{
		snprintf(e->addr, sizeof(e->addr), "%s", host->host_addr ? host->host_addr : "");
		snprintf(e->name, sizeof(e->name), "%s", host->host_name ? host->host_name : "");
		snprintf(e->type, sizeof(e->type), "%s", host->host_type ? host->host_type : "");
		snprintf(e->version, sizeof(e->version), "%s", host->system_version ? host->system_version : "");
		snprintf(e->host_id, sizeof(e->host_id), "%s", host->host_id ? host->host_id : "");
		e->state = (int)host->state + 1;
	}
	pthread_mutex_unlock(&fl->mu);
}

/* Send SRCH (PS5 and PS4 flavour) to every target, repeatedly, and collect the answers.
 * stop_early: return as soon as the first console answered. Returns the number found. */
static int discover_scan(char **targets, int ntargets, unsigned timeout_ms, bool stop_early, FoundList *fl)
{
	pthread_mutex_init(&fl->mu, NULL);
	fl->n = 0;
	ChiakiDiscovery d;
	if(chiaki_discovery_init(&d, &g_log, AF_INET) != CHIAKI_ERR_SUCCESS)
		return 0;
	ChiakiDiscoveryThread th;
	if(chiaki_discovery_thread_start(&th, &d, found_cb, fl) != CHIAKI_ERR_SUCCESS)
	{
		chiaki_discovery_fini(&d);
		return 0;
	}
	uint64_t t0 = now_ns();
	int n = 0;
	while(!g_sig_stop && (now_ns() - t0) / 1000000 < timeout_ms)
	{
		for(int ps5 = 0; ps5 < 2; ps5++)
		{
			ChiakiDiscoveryPacket p = { 0 };
			p.cmd = CHIAKI_DISCOVERY_CMD_SRCH;
			p.protocol_version = (char *)(ps5 ? CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5 : CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4);
			for(int i = 0; i < ntargets; i++)
			{
				struct sockaddr_in sa = { .sin_family = AF_INET,
					.sin_port = htons(ps5 ? CHIAKI_DISCOVERY_PORT_PS5 : CHIAKI_DISCOVERY_PORT_PS4) };
				if(inet_pton(AF_INET, targets[i], &sa.sin_addr) == 1)
					chiaki_discovery_send(&d, &p, (struct sockaddr *)&sa, sizeof(sa));
			}
		}
		for(int i = 0; i < 20; i++)   // ~400 ms between rounds
		{
			sleep_ms(20);
			pthread_mutex_lock(&fl->mu);
			n = fl->n;
			pthread_mutex_unlock(&fl->mu);
			if(stop_early && n)
				goto done;
		}
	}
done:
	if(stop_early && n)
		sleep_ms(150);   // let a second packet (other flavour) update the entry
	chiaki_discovery_thread_stop(&th);
	chiaki_discovery_fini(&d);
	pthread_mutex_lock(&fl->mu);
	n = fl->n;
	pthread_mutex_unlock(&fl->mu);
	return n;
}

static void json_emit(json_object *o)
{
	puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	fflush(stdout);
	json_object_put(o);
}

static int json_fail(const char *fmt, ...)
{
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	json_object *o = json_object_new_object();
	json_object_object_add(o, "ok", json_object_new_boolean(0));
	json_object_object_add(o, "error", json_object_new_string(buf));
	json_emit(o);
	TAP_ERR("%s", buf);
	return 1;
}

static const char *target_type(int target)
{
	return chiaki_target_is_ps5((ChiakiTarget)target) ? "PS5" : "PS4";
}

static void default_conf_paths(const char *chiaki_override, const char *app_override, char *chiaki_path, size_t cp, char *app_path, size_t vp)
{
	const char *home = getenv("HOME") ? getenv("HOME") : "";
	if(chiaki_override)
		snprintf(chiaki_path, cp, "%s", chiaki_override);
	else
		snprintf(chiaki_path, cp, "%s/.config/Chiaki/Chiaki.conf", home);
	if(app_override)
		snprintf(app_path, vp, "%s", app_override);
	else
		snprintf(app_path, vp, "%s/.config/moonglide/remoteplay.conf", home);
}

static bool conf_has_console(const Conf *c, const char *hostid_norm, const char *name)
{
	for(int i = 0; i < c->host_count; i++)
	{
		const HostEntry *h = &c->hosts[i];
		if(!host_usable(h))
			continue;
		if(h->mac_len == 6 && hostid_norm[0])
		{
			char m[13];
			mac_hex(h->mac, m);
			if(!strcmp(m, hostid_norm))
				return true;
		}
		if((h->mac_len != 6 || !hostid_norm[0]) && name[0] && !strcmp(h->nickname, name))
			return true;
	}
	return false;
}

/* directed broadcast address of every up interface, plus the limited broadcast */
static int broadcast_targets(char **out, int cap)
{
	int n = 0;
	struct ifaddrs *ifa, *p;
	if(getifaddrs(&ifa) == 0)
	{
		for(p = ifa; p && n < cap - 1; p = p->ifa_next)
		{
			if(!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !(p->ifa_flags & IFF_UP) ||
				(p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_BROADCAST) || !p->ifa_broadaddr)
				continue;
			char buf[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_broadaddr)->sin_addr, buf, sizeof(buf));
			bool dup = false;
			for(int i = 0; i < n; i++)
				if(!strcmp(out[i], buf))
					dup = true;
			if(!dup)
				out[n++] = strdup(buf);
		}
		freeifaddrs(ifa);
	}
	out[n++] = strdup("255.255.255.255");
	return n;
}

static int mode_discover_all(unsigned timeout_ms, const char *chiaki_conf, const char *app_conf)
{
	char cp[512], vp[512];
	default_conf_paths(chiaki_conf, app_conf, cp, sizeof(cp), vp, sizeof(vp));
	Conf *cc = calloc(1, sizeof(*cc)), *cv = calloc(1, sizeof(*cv));
	conf_load(cc, cp);
	conf_load(cv, vp);

	char *targets[16];
	int nt = broadcast_targets(targets, 16);
	FoundList *fl = calloc(1, sizeof(*fl));
	int n = discover_scan(targets, nt, timeout_ms, false, fl);
	for(int i = 0; i < n; i++)
	{
		const Found *e = &fl->f[i];
		char hid[32];
		norm_hostid(e->host_id, hid, sizeof(hid));
		json_object *o = json_object_new_object();
		json_object_object_add(o, "host", json_object_new_string(e->addr));
		json_object_object_add(o, "name", json_object_new_string(e->name));
		json_object_object_add(o, "type", json_object_new_string(e->type));
		json_object_object_add(o, "state", json_object_new_string(e->state == DISC_STANDBY ? "standby" : "ready"));
		json_object_object_add(o, "system_version", json_object_new_string(e->version));
		json_object_object_add(o, "host_id", json_object_new_string(hid[0] ? hid : e->host_id));
		json_object_object_add(o, "registered", json_object_new_boolean(conf_has_console(cv, hid, e->name) || conf_has_console(cc, hid, e->name)));
		json_emit(o);
	}
	TAP_INFO("%d console(s) found", n);
	for(int i = 0; i < nt; i++)
		free(targets[i]);
	explicit_bzero(cc, sizeof(*cc));
	explicit_bzero(cv, sizeof(*cv));
	free(cc);
	free(cv);
	free(fl);
	return 0;
}

static int mode_list_registered(const char *chiaki_conf, const char *app_conf)
{
	char cp[512], vp[512];
	default_conf_paths(chiaki_conf, app_conf, cp, sizeof(cp), vp, sizeof(vp));
	for(int pass = 0; pass < 2; pass++)
	{
		Conf *c = calloc(1, sizeof(*c));
		if(conf_load(c, pass == 0 ? vp : cp))
			for(int i = 0; i < c->host_count; i++)
			{
				const HostEntry *h = &c->hosts[i];
				if(!host_usable(h))
					continue;
				char m[13] = "";
				if(h->mac_len == 6)
					mac_hex(h->mac, m);
				json_object *o = json_object_new_object();
				json_object_object_add(o, "name", json_object_new_string(h->nickname));
				json_object_object_add(o, "type", json_object_new_string(target_type(h->target)));
				json_object_object_add(o, "host_id", json_object_new_string(m));
				json_object_object_add(o, "source", json_object_new_string(pass == 0 ? "moonglide" : "chiaki-ng"));
				json_emit(o);
			}
		explicit_bzero(c, sizeof(*c));
		free(c);
	}
	return 0;
}

/* ---- PSN OAuth (same public app credentials and endpoints as chiaki-ng's gui/include/psnaccountid.h) */

#define PSN_CLIENT_ID "ba495a24-818c-472b-b12d-ff231c1b5745"
#define PSN_CLIENT_SECRET "mvaiZkRsAsI1IBkY"
#define PSN_TOKEN_URL "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token"
#define PSN_REDIRECT_PAGE "https://remoteplay.dl.playstation.net/remoteplay/redirect"
#define PSN_SCOPE "psn:clientapp referenceDataService:countryConfig.read pushNotification:webSocket.desktop.connect sessionManager:remotePlaySession.system.update"

static int mode_psn_login_url(void)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "url", json_object_new_string(
		"https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?service_entity=urn:service-entity:psn&response_type=code"
		"&client_id=" PSN_CLIENT_ID "&redirect_uri=" PSN_REDIRECT_PAGE
		"&scope=psn:clientapp%20referenceDataService:countryConfig.read%20pushNotification:webSocket.desktop.connect%20sessionManager:remotePlaySession.system.update"
		"&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal&"));
	json_emit(o);
	return 0;
}

typedef struct { char *buf; size_t len; } Slurp;

static size_t slurp_cb(char *p, size_t sz, size_t nm, void *u)
{
	Slurp *s = u;
	size_t n = sz * nm;
	if(s->len + n > (1u << 20))
		return 0;
	char *nb = realloc(s->buf, s->len + n + 1);
	if(!nb)
		return 0;
	s->buf = nb;
	memcpy(s->buf + s->len, p, n);
	s->len += n;
	s->buf[s->len] = '\0';
	return n;
}

static void curl_common(CURL *c, Slurp *s)
{
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, slurp_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
	curl_easy_setopt(c, CURLOPT_USERPWD, PSN_CLIENT_ID ":" PSN_CLIENT_SECRET);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
	if(access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0)
		curl_easy_setopt(c, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
	else
		curl_easy_setopt(c, CURLOPT_CAPATH, "/etc/ssl/certs");
}

static char *url_param(const char *url, const char *key)
{
	const char *q = strchr(url, '?');
	size_t kl = strlen(key);
	while(q)
	{
		q++;
		if(!strncmp(q, key, kl) && q[kl] == '=')
		{
			const char *v = q + kl + 1;
			size_t n = strcspn(v, "&#");
			char *out = malloc(n + 1);
			size_t o = 0;
			for(size_t i = 0; i < n; i++)
			{
				if(v[i] == '%' && i + 2 < n && isxdigit((unsigned char)v[i + 1]) && isxdigit((unsigned char)v[i + 2]))
				{
					char h[3] = { v[i + 1], v[i + 2], 0 };
					out[o++] = (char)strtol(h, NULL, 16);
					i += 2;
				}
				else
					out[o++] = v[i] == '+' ? ' ' : v[i];
			}
			out[o] = '\0';
			return out;
		}
		q = strpbrk(q, "&#");
		if(q && *q == '#')
			break;
	}
	return NULL;
}

static int mode_psn_account_id(const char *redirect_url)
{
	if(strncmp(redirect_url, PSN_REDIRECT_PAGE, strlen(PSN_REDIRECT_PAGE)) != 0)
		return json_fail("that is not the redirect URL (it must start with %s)", PSN_REDIRECT_PAGE);
	char *code = url_param(redirect_url, "code");
	if(!code || !*code)
	{
		free(code);
		return json_fail("the redirect URL has no code parameter; sign in again and copy the full address");
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);
	int rc = 1;
	CURL *c = curl_easy_init();
	Slurp s = { 0 };
	char *body = NULL;
	json_object *j = NULL, *j2 = NULL;
	char *tok = NULL;
	if(!c)
	{
		rc = json_fail("curl init failed");
		goto out;
	}
	// body is built like chiaki-ng does (scope with raw spaces, which Sony accepts)
	if(asprintf(&body, "grant_type=authorization_code&code=%s&scope=" PSN_SCOPE "&redirect_uri=" PSN_REDIRECT_PAGE "&", code) < 0)
	{
		body = NULL;
		rc = json_fail("out of memory");
		goto out;
	}
	curl_common(c, &s);
	curl_easy_setopt(c, CURLOPT_URL, PSN_TOKEN_URL);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
	struct curl_slist *hdr = curl_slist_append(NULL, "Content-Type: application/x-www-form-urlencoded");
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
	CURLcode cr = curl_easy_perform(c);
	long http = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
	curl_slist_free_all(hdr);
	if(cr != CURLE_OK)
	{
		rc = json_fail("cannot reach the PlayStation Network: %s", curl_easy_strerror(cr));
		goto out;
	}
	j = s.buf ? json_tokener_parse(s.buf) : NULL;
	json_object *at = NULL;
	if(http != 200 || !j || !json_object_object_get_ex(j, "access_token", &at) || !json_object_is_type(at, json_type_string))
	{
		rc = json_fail("PSN refused the code (HTTP %ld): it is single use and expires in a few minutes, sign in again", http);
		goto out;
	}
	tok = strdup(json_object_get_string(at));
	free(s.buf);
	s = (Slurp){ 0 };
	json_object_put(j);
	j = NULL;

	char *url2 = NULL;
	if(asprintf(&url2, PSN_TOKEN_URL "/%s", tok) < 0)
	{
		rc = json_fail("out of memory");
		goto out;
	}
	curl_easy_reset(c);
	curl_common(c, &s);
	curl_easy_setopt(c, CURLOPT_URL, url2);
	cr = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
	explicit_bzero(url2, strlen(url2));
	free(url2);
	if(cr != CURLE_OK)
	{
		rc = json_fail("cannot reach the PlayStation Network: %s", curl_easy_strerror(cr));
		goto out;
	}
	j2 = s.buf ? json_tokener_parse(s.buf) : NULL;
	json_object *uid = NULL, *oid = NULL;
	if(http != 200 || !j2 || !json_object_object_get_ex(j2, "user_id", &uid))
	{
		rc = json_fail("PSN returned no account id (HTTP %ld)", http);
		goto out;
	}
	{
		long long id = strtoll(json_object_get_string(uid), NULL, 10);
		if(id <= 0)
		{
			rc = json_fail("PSN returned an invalid account id");
			goto out;
		}
		uint8_t le[8];
		for(int i = 0; i < 8; i++, id >>= 8)
			le[i] = (uint8_t)(id & 0xff);
		char b64[16];
		EVP_EncodeBlock((unsigned char *)b64, le, 8);
		json_object *o = json_object_new_object();
		json_object_object_add(o, "ok", json_object_new_boolean(1));
		json_object_object_add(o, "account_id", json_object_new_string(b64));
		const char *online = "";
		if(json_object_object_get_ex(j2, "online_id", &oid) && json_object_is_type(oid, json_type_string))
			online = json_object_get_string(oid);
		json_object_object_add(o, "online_id", json_object_new_string(online));
		json_emit(o);
		rc = 0;
	}
out:
	if(tok)
	{
		explicit_bzero(tok, strlen(tok));
		free(tok);
	}
	if(s.buf)
	{
		explicit_bzero(s.buf, s.len);
		free(s.buf);
	}
	if(body)
	{
		explicit_bzero(body, strlen(body));
		free(body);
	}
	explicit_bzero(code, strlen(code));
	free(code);
	if(j)
		json_object_put(j);
	if(j2)
		json_object_put(j2);
	if(c)
		curl_easy_cleanup(c);
	curl_global_cleanup();
	return rc;
}

/* ---- registration */

typedef struct
{
	atomic_int done;       // 0 running, 1 success, 2 failed, 3 canceled
	ChiakiRegisteredHost host;
} RegistWait;

static void regist_cb(ChiakiRegistEvent *ev, void *user)
{
	RegistWait *w = user;
	if(ev->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && ev->registered_host)
	{
		w->host = *ev->registered_host;
		atomic_store(&w->done, 1);
	}
	else
		atomic_store(&w->done, ev->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED ? 3 : 2);
}

static int mode_register(const char *host, const char *account_b64, const char *pin_s, bool ps4, const char *app_conf)
{
	if(!host || !account_b64 || !pin_s)
		return json_fail("--register needs --host, --psn-account-id and --pin");
	char pin_buf[32];
	if(!strcmp(pin_s, "-"))
	{
		if(!fgets(pin_buf, sizeof(pin_buf), stdin))
			return json_fail("no PIN on stdin");
		pin_buf[strcspn(pin_buf, "\r\n")] = '\0';
		char *b = pin_buf;
		while(*b == ' ' || *b == '\t')
			b++;
		size_t l = strlen(b);
		while(l && (b[l - 1] == ' ' || b[l - 1] == '\t'))
			b[--l] = '\0';
		memmove(pin_buf, b, l + 1);
		pin_s = pin_buf;
	}
	if(strlen(pin_s) != 8 || strspn(pin_s, "0123456789") != 8)
		return json_fail("the PIN must be 8 digits");
	struct in_addr ia;
	if(inet_pton(AF_INET, host, &ia) != 1)
		return json_fail("--host must be an IPv4 address");
	uint8_t acc[16];
	int accn = EVP_DecodeBlock(acc, (const unsigned char *)account_b64, (int)strlen(account_b64));
	if(strlen(account_b64) != 12 || accn < 8 || account_b64[11] != '=')
		return json_fail("invalid account id (expected the base64 string from --psn-account-id)");

	// the console must be on its "Link Device" screen, but it still answers discovery
	{
		FoundList *fl = calloc(1, sizeof(*fl));
		char *t[1] = { (char *)host };
		int n = discover_scan(t, 1, 2500, true, fl);
		if(n == 0)
			TAP_WARN("no discovery answer from %s; trying to register anyway", host);
		else if(!strcmp(fl->f[0].type, "PS5") == ps4 && fl->f[0].type[0])
		{
			int rc = json_fail("the console at %s is a %s, but %s was requested", host, fl->f[0].type, ps4 ? "--ps4" : "a PS5");
			free(fl);
			return rc;
		}
		free(fl);
	}

	ChiakiRegistInfo info = { 0 };
	info.target = ps4 ? CHIAKI_TARGET_PS4_10 : CHIAKI_TARGET_PS5_1;
	info.host = host;
	info.broadcast = false;
	info.pin = (uint32_t)strtoul(pin_s, NULL, 10);
	info.console_pin = 0;
	info.holepunch_info = NULL;
	memcpy(info.psn_account_id, acc, CHIAKI_PSN_ACCOUNT_ID_SIZE);

	RegistWait *w = calloc(1, sizeof(*w));
	ChiakiRegist regist;
	atomic_store(&g_regist_http_code, 0);
	if(chiaki_regist_start(&regist, &g_log, &info, regist_cb, w) != CHIAKI_ERR_SUCCESS)
	{
		explicit_bzero(&info, sizeof(info));
		free(w);
		return json_fail("cannot start the registration");
	}
	uint64_t t0 = now_ns();
	while(!atomic_load(&w->done) && !g_sig_stop && (now_ns() - t0) / 1000000000ull < 45)
		sleep_ms(100);
	bool finished = atomic_load(&w->done) != 0;
	if(!finished)
		chiaki_regist_stop(&regist);
	chiaki_regist_fini(&regist);
	explicit_bzero(&info, sizeof(info));
	int done = atomic_load(&w->done);

	if(done != 1)
	{
		int http = atomic_load(&g_regist_http_code);
		int rc;
		if(g_sig_stop)
			rc = json_fail("registration interrupted");
		else if(http == 403 || http == 401)
			rc = json_fail("wrong PIN or the PIN expired (generate a new one on the console: Settings > System > Remote Play > Link Device)");
		else if(http)
			rc = json_fail("the console rejected the registration (HTTP %d); check the PIN and the account id", http);
		else if(atomic_load(&g_regist_search_refused))
			rc = json_fail("the console refused the registration: keep its Link Device screen (with the PIN) open while registering");
		else if(!finished || atomic_load(&g_regist_search_timeout))
			rc = json_fail("timeout: the console did not answer; is it on, in the same network, and showing the Link Device screen?");
		else
			rc = json_fail("registration failed (see the log on stderr)");
		explicit_bzero(w, sizeof(*w));
		free(w);
		return rc;
	}

	// store it in moonglide's own conf
	char cp[512], vp[512];
	default_conf_paths(NULL, app_conf, cp, sizeof(cp), vp, sizeof(vp));
	Conf *c = calloc(1, sizeof(*c));
	conf_load(c, vp);
	HostEntry ne = { 0 };
	snprintf(ne.nickname, sizeof(ne.nickname), "%.*s", (int)sizeof(w->host.server_nickname), w->host.server_nickname);
	ne.target = (int)w->host.target;
	ne.have_target = true;
	memcpy(ne.regist_key, w->host.rp_regist_key, 16);
	ne.regist_key_len = 16;
	memcpy(ne.rp_key, w->host.rp_key, 16);
	ne.rp_key_len = 16;
	memcpy(ne.mac, w->host.server_mac, 6);
	ne.mac_len = 6;
	ne.rp_key_type = w->host.rp_key_type;
	int slot = -1;
	for(int i = 0; i < c->host_count; i++)
		if(host_usable(&c->hosts[i]) && c->hosts[i].mac_len == 6 && !memcmp(c->hosts[i].mac, ne.mac, 6))
			slot = i;
	if(slot < 0)
	{
		for(int i = 0; i < c->host_count && slot < 0; i++)
			if(!host_usable(&c->hosts[i]))
				slot = i;
		if(slot < 0 && c->host_count < MAX_HOSTS)
			slot = c->host_count++;
	}
	int rc;
	char err[700] = "";
	if(slot < 0)
		rc = json_fail("moonglide's registration list is full (%d)", MAX_HOSTS);
	else
	{
		c->hosts[slot] = ne;
		if(!conf_write(c, vp, err, sizeof(err)))
			rc = json_fail("%s", err);
		else
		{
			char m[13];
			mac_hex(ne.mac, m);
			json_object *o = json_object_new_object();
			json_object_object_add(o, "ok", json_object_new_boolean(1));
			json_object_object_add(o, "name", json_object_new_string(ne.nickname));
			json_object_object_add(o, "type", json_object_new_string(target_type(ne.target)));
			json_object_object_add(o, "host_id", json_object_new_string(m));
			json_emit(o);
			rc = 0;
		}
	}
	explicit_bzero(&ne, sizeof(ne));
	explicit_bzero(c, sizeof(*c));
	explicit_bzero(w, sizeof(*w));
	free(c);
	free(w);
	return rc;
}

/* ------------------------------------------------------------------ main */

typedef struct
{
	const char *host, *conf, *app_conf, *nickname, *shm;
	const char *psn_account_id, *pin;   // --psn-account-id: redirect URL (alone) or base64 id (with --register)
	unsigned timeout_ms;
	bool discover_all, psn_login_url, register_mode, ps4, list_registered;
	long bitrate;            // kbps, 0 = from conf / default
	int fps, res;
	bool h265;
	bool wake, audio, controller, reconnect, discover_only, check_conf;
} Opts;

static void usage(void)
{
	fputs("usage: moonglide-chiaki-tap --host IP [options]            (session)\n"
		"       moonglide-chiaki-tap MODE                          (setup, JSON lines on stdout, exit 0/1)\n"
		"  --conf PATH       Chiaki.conf (default ~/.config/Chiaki/Chiaki.conf)\n"
		"  --app-conf PATH moonglide's own registrations (default ~/.config/moonglide/remoteplay.conf; searched first)\n"
		"  --nickname NAME   registered host by server_nickname (default: the one matching the console at --host,\n"
		"                    else the first)\n"
		"  --shm NAME        shared memory name (default moonglide-remoteplay)\n"
		"  --bitrate KBPS    (default: settings/bitrate_local_ps5, else 30000)\n"
		"  --fps 30|60       (60)\n  --res 720|1080|1440|2160 (1080; 1440/2160 are custom sizes, the console may downgrade)\n  --codec h264|h265 (h265)\n"
		"  --wake            wake the console from rest mode at start (later: only on the reader's wake_request)\n"
		"  --no-audio  --no-controller\n"
		"  --reconnect       retry forever after the session ends (backoff 1,2,4,8,10 s; 4 s while the PS5 is busy; 30 s on auth errors)\n"
		"  --discover-only   print the console's state and exit\n"
		"  --check-conf      parse the conf, print only field lengths, exit\n"
		"setup modes:\n"
		"  --discover-all [--timeout-ms N]   broadcast discovery (2000 ms); one JSON line per console:\n"
		"                    {host,name,type,state,system_version,host_id,registered}\n"
		"  --psn-login-url                   {\"url\":...} the PSN sign-in page to open in a browser\n"
		"  --psn-account-id REDIRECT_URL     exchange the redirect URL's code: {ok,account_id,online_id}\n"
		"  --register --host IP --psn-account-id B64 --pin NNNNNNNN|- [--ps4]\n"
		"                    register (PS5 by default) and store it in --app-conf: {ok,name,type,host_id}\n"
		"  --list-registered                 {name,type,host_id,source:moonglide|chiaki-ng} per registration\n"
		"  -v                log everything (hexdumps stay suppressed)\n", stderr);
}

static bool parse_args(int argc, char **argv, Opts *o)
{
	*o = (Opts){ .shm = RP_TAP_DEFAULT_NAME, .fps = 60, .res = 1080, .h265 = true, .audio = true, .controller = true };
	for(int i = 1; i < argc; i++)
	{
		const char *a = argv[i];
		#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), exit(2), (char *)0))
		if(!strcmp(a, "--host")) o->host = NEXT();
		else if(!strcmp(a, "--conf")) o->conf = NEXT();
		else if(!strcmp(a, "--app-conf")) o->app_conf = NEXT();
		else if(!strcmp(a, "--nickname")) o->nickname = NEXT();
		else if(!strcmp(a, "--discover-all")) o->discover_all = true;
		else if(!strcmp(a, "--timeout-ms")) o->timeout_ms = (unsigned)atoi(NEXT());
		else if(!strcmp(a, "--psn-login-url")) o->psn_login_url = true;
		else if(!strcmp(a, "--psn-account-id")) o->psn_account_id = NEXT();
		else if(!strcmp(a, "--register")) o->register_mode = true;
		else if(!strcmp(a, "--pin")) o->pin = NEXT();
		else if(!strcmp(a, "--ps4")) o->ps4 = true;
		else if(!strcmp(a, "--list-registered")) o->list_registered = true;
		else if(!strcmp(a, "--shm")) o->shm = NEXT();
		else if(!strcmp(a, "--bitrate")) o->bitrate = atol(NEXT());
		else if(!strcmp(a, "--fps")) o->fps = atoi(NEXT());
		else if(!strcmp(a, "--res")) o->res = atoi(NEXT());
		else if(!strcmp(a, "--codec"))
		{
			const char *c = NEXT();
			if(!strcmp(c, "h264")) o->h265 = false;
			else if(!strcmp(c, "h265")) o->h265 = true;
			else return false;
		}
		else if(!strcmp(a, "--wake")) o->wake = true;
		else if(!strcmp(a, "--no-audio")) o->audio = false;
		else if(!strcmp(a, "--no-controller")) o->controller = false;
		else if(!strcmp(a, "--reconnect")) o->reconnect = true;
		else if(!strcmp(a, "--discover-only")) o->discover_only = true;
		else if(!strcmp(a, "--check-conf")) o->check_conf = true;
		else if(!strcmp(a, "-v")) g_verbose = true;
		else if(!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); exit(0); }
		else return false;
		#undef NEXT
	}
	if(o->fps != 30 && o->fps != 60) return false;
	if(o->res != 720 && o->res != 1080 && o->res != 1440 && o->res != 2160) return false;
	if(!o->timeout_ms) o->timeout_ms = 2000;
	return o->host || o->check_conf || o->discover_all || o->psn_login_url || o->psn_account_id || o->register_mode || o->list_registered;
}

/* SDL events -> the pads' states; waits at most ~4 ms. */
static void pads_poll(bool *changed_out, bool *press_out)
{
	SDL_Event e;
	bool changed = false, press = false;
	if(SDL_WaitEventTimeout(&e, 4))
	{
		do
		{
			switch(e.type)
			{
				case SDL_CONTROLLERDEVICEADDED: pad_add(e.cdevice.which); changed = true; break;
				case SDL_CONTROLLERDEVICEREMOVED: pad_remove(e.cdevice.which); changed = true; break;
				case SDL_CONTROLLERBUTTONDOWN:
				case SDL_CONTROLLERBUTTONUP:
				{
					Pad *p = pad_find(e.cbutton.which);
					if(p && pad_button(p, &e.cbutton))
					{
						changed = true;
						if(e.type == SDL_CONTROLLERBUTTONDOWN)
							press = true;
					}
					break;
				}
				case SDL_CONTROLLERAXISMOTION:
				{
					Pad *p = pad_find(e.caxis.which);
					if(p && pad_axis(p, &e.caxis)) changed = true;
					break;
				}
				case SDL_CONTROLLERSENSORUPDATE:
				{
					Pad *p = pad_find(e.csensor.which);
					if(p && pad_sensor(p, &e.csensor)) changed = true;
					break;
				}
				case SDL_CONTROLLERTOUCHPADDOWN:
				case SDL_CONTROLLERTOUCHPADMOTION:
				case SDL_CONTROLLERTOUCHPADUP:
				{
					Pad *p = pad_find(e.ctouchpad.which);
					if(p && pad_touchpad(p, &e.ctouchpad)) changed = true;
					break;
				}
				default: break;
			}
		} while(SDL_PollEvent(&e));
	}
	*changed_out = changed;
	*press_out = press;
}

static void controller_pump(bool send)
{
	// returns after at most ~4 ms
	bool changed, press;
	pads_poll(&changed, &press);

	static uint64_t last_send;
	static bool have_last;
	uint64_t t = now_ns();
	if(send && g.session_inited && atomic_load(&g.connected))
	{
		if(changed || !have_last || t - last_send >= 50000000ull)
		{
			ChiakiControllerState out;
			controller_merged(&out);
			chiaki_session_set_controller_state(&g.session, &out);
			stick_watch(&out, now_ns());
			if(press && g_hdr)
			{
				__atomic_store_n(&g_hdr->input_ns, now_ns(), __ATOMIC_RELEASE);
				__atomic_fetch_add(&g_hdr->input_seq, 1u, __ATOMIC_RELEASE);
			}
			have_last = true;
			last_send = t;
		}
	}
	else
		have_last = false;

	unsigned r = atomic_exchange(&g.rumble, 0);
	if(r & 0x80000000u)
		for(int i = 0; i < MAX_PADS; i++)
			if(g_pads[i].gc)
				pad_rumble(&g_pads[i], (uint8_t)(r & 0xff), (uint8_t)((r >> 8) & 0xff));
	fx_apply(t);
	haptics_try_open();
	haptics_rumble_tick(t);
}

int main(int argc, char **argv)
{
	Opts o;
	chiaki_log_init(&g_log, CHIAKI_LOG_ALL & ~(g_verbose ? 0 : (CHIAKI_LOG_VERBOSE | CHIAKI_LOG_DEBUG)), log_cb, NULL);
	if(!parse_args(argc, argv, &o))
	{
		usage();
		return 2;
	}
	// -v is parsed after log init; reset the level now
	chiaki_log_set_level(&g_log, g_verbose ? CHIAKI_LOG_ALL : (CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO | CHIAKI_LOG_VERBOSE));

	struct sigaction sa0 = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa0, NULL);
	sigaction(SIGTERM, &sa0, NULL);
	signal(SIGPIPE, SIG_IGN);


	// setup modes: JSON lines on stdout
	if(o.psn_login_url)
		return mode_psn_login_url();
	if(o.register_mode)
		return mode_register(o.host, o.psn_account_id, o.pin, o.ps4, o.app_conf);
	if(o.psn_account_id)
		return mode_psn_account_id(o.psn_account_id);
	if(o.discover_all)
		return mode_discover_all(o.timeout_ms, o.conf, o.app_conf);
	if(o.list_registered)
		return mode_list_registered(o.conf, o.app_conf);

	char confpath[512], apppath[512];
	default_conf_paths(o.conf, o.app_conf, confpath, sizeof(confpath), apppath, sizeof(apppath));

	Conf *confs[2] = { calloc(1, sizeof(Conf)), calloc(1, sizeof(Conf)) };   // [0] moonglide, [1] Chiaki.conf
	bool loaded[2] = { conf_load(confs[0], apppath), conf_load(confs[1], confpath) };
	if(!loaded[0] && !loaded[1])
	{
		TAP_ERR("cannot read %s or %s", apppath, confpath);
		return 1;
	}

	// which console is at --host? (its host-id / name pick the registration)
	Found probe = { 0 };
	if(o.host && !o.check_conf && !o.discover_only)
	{
		FoundList *fl = calloc(1, sizeof(*fl));
		char *t[1] = { (char *)o.host };
		if(discover_scan(t, 1, 2000, true, fl) > 0)
			probe = fl->f[0];
		free(fl);
	}
	char probe_id[32];
	norm_hostid(probe.host_id, probe_id, sizeof(probe_id));

	Conf *conf = NULL;
	HostEntry *he = NULL;
	const char *src = NULL;
	for(int pass = 0; pass < 4 && !he; pass++)
	{
		// passes: 0/1 = by nickname or by the probed console (moonglide, Chiaki.conf), 2/3 = first usable
		int ci = pass & 1;
		if(!loaded[ci] || (pass >= 2 && o.nickname))
			continue;
		for(int i = 0; i < confs[ci]->host_count; i++)
		{
			HostEntry *h = &confs[ci]->hosts[i];
			if(!host_usable(h) && !(h->regist_key_len || h->rp_key_len))
				continue;
			bool match;
			if(pass >= 2)
				match = true;
			else if(o.nickname)
				match = !strcmp(h->nickname, o.nickname);
			else if(probe.addr[0])
			{
				char m[13] = "";
				if(h->mac_len == 6)
					mac_hex(h->mac, m);
				match = (probe_id[0] && h->mac_len == 6 && !strcmp(m, probe_id)) || (!probe_id[0] && !strcmp(h->nickname, probe.name));
			}
			else
				match = false;
			if(match)
			{
				he = h;
				conf = confs[ci];
				src = ci == 0 ? "moonglide" : "chiaki-ng";
				if(pass >= 2 && probe.addr[0])
					TAP_WARN("no registration matches the console at %s (%s); using the first one", o.host, probe.name);
				break;
			}
		}
	}
	if(!he)
	{
		TAP_ERR("no registered host%s%s in the conf", o.nickname ? " named " : "", o.nickname ? o.nickname : "");
		return 1;
	}
	{
		Conf *other = conf == confs[0] ? confs[1] : confs[0];
		if(conf->bitrate_local_ps5 <= 0)
			conf->bitrate_local_ps5 = other->bitrate_local_ps5;
		explicit_bzero(other, sizeof(*other));
		free(other);
	}
	TAP_INFO("registration '%s' from the %s conf", he->nickname, src);
	if(o.check_conf)
	{
		printf("hosts=%d nickname=%s target=%d rp_regist_key=%zu rp_key=%zu server_mac=%zu bitrate_local_ps5=%ld\n",
			conf->host_count, he->nickname, he->target, he->regist_key_len, he->rp_key_len, he->mac_len, conf->bitrate_local_ps5);
		int ok = (he->regist_key_len == 16 && he->rp_key_len == 16 && he->mac_len == 6) ? 0 : 1;
		explicit_bzero(conf, sizeof(*conf));
		return ok;
	}
	if(he->regist_key_len != 16 || he->rp_key_len != 16)
	{
		TAP_ERR("registered host '%s' has an invalid key (lengths %zu/%zu)", he->nickname, he->regist_key_len, he->rp_key_len);
		explicit_bzero(conf, sizeof(*conf));
		return 1;
	}
	bool ps5 = chiaki_target_is_ps5((ChiakiTarget)he->target);
	long bitrate = o.bitrate ? o.bitrate : (conf->bitrate_local_ps5 > 0 ? conf->bitrate_local_ps5 : 30000);
	TAP_INFO("host '%s' target %d (%s), %ux%u@%d %s, %ld kbps", he->nickname, he->target, ps5 ? "PS5" : "PS4",
		o.res == 2160 ? 3840 : o.res == 1440 ? 2560 : o.res == 1080 ? 1920 : 1280, o.res, o.fps, o.h265 ? "h265" : "h264", bitrate);

	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if(o.discover_only)
	{
		DiscResult r = { 0 };
		int s = discover(o.host, ps5, 4000, &r);
		printf("state=%s name=%s type=%s system_version=%s\n", disc_state_str(s), s ? r.name : "-", s ? r.type : "-", s ? r.version : "-");
		explicit_bzero(conf, sizeof(*conf));
		return s ? 0 : 1;
	}

	if(!shm_open_map(o.shm))
		return 1;
	shm_init_idle((unsigned)bitrate);

	// audio
	A.enabled = o.audio;
	if(o.audio)
	{
		if(pthread_create(&A.th, NULL, audio_thread, NULL) == 0)
			A.th_started = true;
	}

	// controllers
	bool sdl_ok = false;
	if(o.controller)
	{
		SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
		// like chiaki-ng: the HIDAPI drivers with rumble (needed over Bluetooth), plus effects/sensors/touchpad
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "0");
		if(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0)
			sdl_ok = true;
		else
			TAP_WARN("SDL init failed (%s); controllers disabled", SDL_GetError());
	}

	int rc = 0;
	unsigned backoff_s = 0;
	uint64_t busy_t0 = 0;
	bool first_attempt = true;
	for(;;)
	{
		atomic_store(&g.connected, false);
		atomic_store(&g.quit, false);
		g.quit_reason[0] = '\0';
		g.quit_code = -1;
		g.stream_on = false;
		uint64_t sess_t0 = now_ns();

		// rest mode: wake the console only when asked (--wake: the first attempt only; afterwards
		// the reader's wake_request). Putting the PS5 to sleep must not wake it straight back up.
		{
			DiscResult r = { 0 };
			int s = discover(o.host, ps5, 3000, &r);
			const bool wake_at_start = o.wake && first_attempt;
			first_attempt = false;
			if(s == DISC_STANDBY && !wake_at_start && !LD(wake_request))
			{
				shm_set_state(RP_TAP_STANDBY, "console in rest mode");
				TAP_INFO("console in rest mode; waiting for a wake request");
				uint64_t tp = now_ns();
				while(!g_sig_stop && !LD(wake_request) && s == DISC_STANDBY)
				{
					if(sdl_ok)
						controller_pump(false);
					else
						sleep_ms(4);
					if((now_ns() - tp) / 1000000ull >= 3000)
					{
						s = discover(o.host, ps5, 1500, &r);
						tp = now_ns();
					}
				}
				if(g_sig_stop)
					goto session_done;
			}
			if(s == DISC_STANDBY)   // and waking was asked for, and waking was asked for
			{
				ST(wake_request, 0u);
				char *end = NULL;
				char keystr[17];
				memcpy(keystr, he->regist_key, 16);
				keystr[16] = '\0';
				uint64_t cred = strtoull(keystr, &end, 16);
				bool ok = end && *end == '\0' && strlen(keystr) <= 8;
				explicit_bzero(keystr, sizeof(keystr));
				if(!ok)
				{
					snprintf(g.quit_reason, sizeof(g.quit_reason), "invalid regist key for wakeup");
					goto session_done;
				}
				shm_set_state(RP_TAP_WAKING, "waking the console");
				TAP_INFO("console in standby, sending wakeup");
				chiaki_discovery_wakeup(&g_log, NULL, o.host, cred, ps5);
				cred = 0;
				uint64_t t0 = now_ns();
				while(!g_sig_stop && (now_ns() - t0) / 1000000000ull < 40)
				{
					sleep_ms(1000);
					s = discover(o.host, ps5, 1500, &r);
					if(s == DISC_READY)
						break;
				}
				if(s != DISC_READY || g_sig_stop)
				{
					if(!g_sig_stop)
						snprintf(g.quit_reason, sizeof(g.quit_reason), "console did not wake within 40 s");
					goto session_done;
				}
				TAP_INFO("console is ready");
			}
			else if(s == DISC_NO_ANSWER)
				TAP_WARN("console did not answer discovery; trying to connect anyway");
			ST(wake_request, 0u);
		}

		// session
		ChiakiConnectInfo ci = { 0 };
		ci.ps5 = ps5;
		ci.host = o.host;
		memcpy(ci.regist_key, he->regist_key, sizeof(ci.regist_key));
		memcpy(ci.morning, he->rp_key, sizeof(ci.morning));
		chiaki_connect_video_profile_preset(&ci.video_profile, o.res >= 1080 ? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p : CHIAKI_VIDEO_RESOLUTION_PRESET_720p,
			o.fps == 60 ? CHIAKI_VIDEO_FPS_PRESET_60 : CHIAKI_VIDEO_FPS_PRESET_30);
		if(o.res == 1440 || o.res == 2160)   // no libchiaki preset: custom size (the console downgrades what it cannot do)
		{
			ci.video_profile.width = o.res == 2160 ? 3840 : 2560;
			ci.video_profile.height = (unsigned)o.res;
		}
		ci.video_profile.bitrate = (unsigned)bitrate;
		ci.video_profile.codec = o.h265 ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
		ci.video_profile_auto_downgrade = true;
		ci.enable_keyboard = false;
		bool ds_session = sdl_ok && dualsense_attached();
		ci.enable_dualsense = ds_session;
		fx_reset(ds_session);
		ci.packet_loss_max = 0.05;
		ci.enable_idr_on_fec_failure = true;
		ci.holepunch_session = NULL;
		if(!o.audio)
			ci.audio_video_disabled = CHIAKI_AUDIO_DISABLED;
		g.bitrate_kbps = (unsigned)bitrate;

		shm_set_state(RP_TAP_CONNECTING, "connecting to %s", o.host);
		g_hdr->bitrate_kbps = (uint32_t)bitrate;
		ChiakiErrorCode err = chiaki_session_init(&g.session, &ci, &g_log);
		explicit_bzero(ci.regist_key, sizeof(ci.regist_key));
		explicit_bzero(ci.morning, sizeof(ci.morning));
		if(err != CHIAKI_ERR_SUCCESS)
		{
			snprintf(g.quit_reason, sizeof(g.quit_reason), "session init failed: %s", chiaki_error_string(err));
			goto session_done;
		}
		g.session_inited = true;
		chiaki_opus_decoder_init(&g.opus, &g_log);
		g.opus_inited = true;
		chiaki_opus_decoder_set_cb(&g.opus, audio_settings_cb, audio_frame_cb, NULL);
		ChiakiAudioSink sink;
		chiaki_opus_decoder_get_sink(&g.opus, &sink);
		chiaki_session_set_audio_sink(&g.session, &sink);
		chiaki_session_set_video_sample_cb(&g.session, video_cb, NULL);
		chiaki_session_set_event_cb(&g.session, event_cb, NULL);
		if(ds_session)
		{
			ChiakiAudioSink hsink = { 0 };
			hsink.frame_cb = haptics_cb;
			chiaki_session_set_haptics_sink(&g.session, &hsink);
			TAP_INFO("DualSense attached: DualSense feedback enabled (rumble, adaptive triggers, haptics, lightbar, motion, touchpad)");
		}

		atomic_store(&g_login_refused, false);
		err = chiaki_session_start(&g.session);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			snprintf(g.quit_reason, sizeof(g.quit_reason), "session start failed: %s", chiaki_error_string(err));
			goto session_end;
		}

		while(!atomic_load(&g.quit) && !g_sig_stop)
		{
			if(atomic_load(&g_login_refused))
			{
				snprintf(g.quit_reason, sizeof(g.quit_reason), "login refused");
				break;
			}
			if(sdl_ok)
				controller_pump(true);
			else
				sleep_ms(4);
			if(g.stream_on && LD(idr_request))
			{
				ST(idr_request, 0u);
				chiaki_session_request_idr(&g.session);
			}
			if(g.stream_on)
				ST(frames_lost_total, g.lost_total);
		}
		chiaki_session_stop(&g.session);
		chiaki_session_join(&g.session);

session_end:
		atomic_store(&g.connected, false);
		if(sdl_ok)
			fx_stop_all();
		if(g.session_inited)
		{
			chiaki_session_fini(&g.session);
			explicit_bzero(&g.session, sizeof(g.session));
			g.session_inited = false;
		}
		if(g.opus_inited)
		{
			chiaki_opus_decoder_fini(&g.opus);
			g.opus_inited = false;
		}
session_done:
		ST(magic, 0u);
		if(g_sig_stop)
			snprintf(g.quit_reason, sizeof(g.quit_reason), "stopped by signal");
		else if(!g.quit_reason[0])
			snprintf(g.quit_reason, sizeof(g.quit_reason), "session ended");
		TAP_INFO("session over: %s", g.quit_reason);
		shm_set_state(RP_TAP_QUIT, "%s", g.quit_reason);
		if(!g_sig_stop && strstr(g.quit_reason, "Stopped") == NULL)
			rc = 1;

		if(!o.reconnect || g_sig_stop)
			break;

		// retry policy
		{
			bool streamed = g.stream_on && (now_ns() - sess_t0) / 1000000000ull > 30;
			unsigned delay_s;
			const char *why = NULL;
			bool busy = false;
			if(streamed)
				backoff_s = 0;
			if(g.quit_code == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE)
			{
				if(!busy_t0)
					busy_t0 = now_ns();
				if((now_ns() - busy_t0) / 1000000000ull < 90)
				{
					busy = true;
					delay_s = 4;
					why = "PS5 busy (stale session), retrying";
				}
				else
				{
					busy_t0 = 0;   // never released; fall back to generic backoff
					why = "PS5 still busy";
					backoff_s = backoff_s ? (backoff_s * 2 > 10 ? 10 : backoff_s * 2) : 1;
					delay_s = backoff_s;
				}
			}
			else if(atomic_load(&g_login_refused))
			{
				busy_t0 = 0;
				backoff_s = 0;
				delay_s = 5;
				why = "PS5: no user signed in (pick a user on the console)";
			}
			else if(g.quit_code == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH ||
				g.quit_code == CHIAKI_QUIT_REASON_PSN_REGIST_FAILED || g.quit_code == -2)
			{
				busy_t0 = 0;
				delay_s = 30;
			}
			else
			{
				busy_t0 = 0;
				backoff_s = backoff_s ? (backoff_s * 2 > 10 ? 10 : backoff_s * 2) : 1;
				delay_s = backoff_s;
			}
			char st[sizeof(g_hdr->status)];
			if(busy)
				snprintf(st, sizeof(st), "%s", why);
			else
				snprintf(st, sizeof(st), "%s; retry in %us", why ? why : g.quit_reason, delay_s);
			shm_set_state(RP_TAP_QUIT, "%s", st);
			TAP_INFO("retry in %u s (%s)", delay_s, st);
			uint64_t w0 = now_ns();
			while(!g_sig_stop && (now_ns() - w0) / 1000000 < (uint64_t)delay_s * 1000)
			{
				if(sdl_ok)
					controller_pump(false);
				else
					sleep_ms(4);
			}
		}
		if(g_sig_stop)
			break;
		TAP_INFO("reconnecting");
	}

	if(A.th_started)
	{
		pthread_mutex_lock(&A.mu);
		A.quit = true;
		pthread_cond_broadcast(&A.cv);
		pthread_mutex_unlock(&A.mu);
		pthread_join(A.th, NULL);
	}
	if(sdl_ok)
	{
		for(int i = 0; i < MAX_PADS; i++)
			if(g_pads[i].gc)
				SDL_GameControllerClose(g_pads[i].gc);
		SDL_Quit();
	}
	if(g_sig_stop)
		rc = 0;
	ST(magic, 0u);
	munmap(g_shm, RP_TAP_SIZE);   // the shm file stays in place
	explicit_bzero(conf, sizeof(*conf));
	free(conf);
	return rc;
}
