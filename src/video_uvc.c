/*
 * rkkvm-video : USB-UVC (libusb/libusb-uvc) MJPEG streamer + web UI for
 * RK3506 IP-KVM.  The kernel has no uvcvideo driver, so the capture stick
 * is driven from userspace via libusb.
 *
 * HTTP endpoints (default port 8080):
 *   /            web UI (served from /userdata/rkkvm.html)
 *   /stream      multipart/x-mixed-replace MJPEG stream
 *   /snapshot    single JPEG
 *   /status      text status
 *   /setres?w=&h= restart stream at a new resolution
 *   /api/hid     POST -> forwarded to the rkkvm-hid daemon on 127.0.0.1:5001
 *   /ws          WebSocket: low-latency HID command channel
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <libuvc/libuvc.h>
#include <turbojpeg.h>

#define LISTEN_PORT 8080
#define HID_HOST "127.0.0.1"
#define HID_PORT 5001
#define HTML_FILE "/userdata/rkkvm.html"

static uvc_context_t *ctx;
static uvc_device_handle_t *devh;
static uvc_stream_handle_t *strmh;
static pthread_mutex_t strm_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static uint8_t *g_jpeg = NULL;
static size_t g_jpeg_len = 0;
static unsigned long g_seq = 0;
static int g_width = 0, g_height = 0, g_fps = 0;
static volatile sig_atomic_t g_stop = 0;

static pthread_mutex_t hid_lock = PTHREAD_MUTEX_INITIALIZER;
static int hid_fd = -1;

/* enumerated MJPEG modes */
struct vmode { int w, h; int fps[8]; int nfps; };
static struct vmode g_modes[32];
static int g_nmodes = 0;
static unsigned long long g_tx_bytes = 0;

/* KVM jumper GPIO config (persisted in /userdata/kvm.conf) */
static int cfg_power = 22, cfg_reset = 23, cfg_status = -1;
static int gpio_read(int n);
static int gpio_set(int n, const char *val, const char *dir);
static void load_cfg(void)
{
	FILE *f = fopen("/userdata/kvm.conf", "r");
	if (!f)
		return;
	char line[160];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = line;
		while (*k == ' ' || *k == '\t')
			k++;
		char *v = eq + 1;
		while (*v == ' ' || *v == '\t')
			v++;
		char *e = v + strlen(v);
		while (e > v && (e[-1] == '\n' || e[-1] == '\r' ||
				 e[-1] == ' ' || e[-1] == '\t'))
			*--e = 0;
		if (!strcmp(k, "POWER_GPIO"))
			cfg_power = atoi(v);
		else if (!strcmp(k, "RESET_GPIO"))
			cfg_reset = atoi(v);
		else if (!strcmp(k, "STATUS_GPIO"))
			cfg_status = atoi(v);
	}
	fclose(f);
}

static void on_term(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* software JPEG re-encoding (compression mode) */
static tjhandle g_tjdec = NULL, g_tjenc = NULL;
static unsigned char *g_rgb = NULL;
static size_t g_rgb_cap = 0;
static int g_enc_mode = 0;	/* 0 = passthrough, 1 = recompress */
static int g_quality = 75;

static int recompress(const uint8_t *in, size_t inlen, uint8_t **out,
		      size_t *outlen)
{
	int w, h, sub, cs;
	if (tjDecompressHeader3(g_tjdec, in, inlen, &w, &h, &sub, &cs) < 0)
		return -1;
	size_t need = (size_t)w * h * 3;
	if (need > g_rgb_cap) {
		unsigned char *nb = realloc(g_rgb, need);
		if (!nb)
			return -1;
		g_rgb = nb;
		g_rgb_cap = need;
	}
	if (tjDecompress2(g_tjdec, in, inlen, g_rgb, w, 0, h, TJPF_BGR,
			  TJFLAG_FASTDCT) < 0)
		return -1;
	unsigned char *ob = NULL;
	unsigned long os = 0;
	int q = __atomic_load_n(&g_quality, __ATOMIC_RELAXED);
	if (tjCompress2(g_tjenc, g_rgb, w, 0, h, TJPF_BGR, &ob, &os,
			sub, q, TJFLAG_FASTDCT) < 0)
		return -1;
	*out = ob;
	*outlen = os;
	return 0;
}

/* ============================ auth / sessions ============================ */

static ssize_t send_all(int fd, const void *buf, size_t len);
static void serve_file(int c, const char *path, const char *ctype);
static int gpio_set(int n, const char *val, const char *dir);

static void sha256(const uint8_t *d, size_t n, uint8_t out[32])
{
	static const uint32_t K[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
	uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
		       0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
	uint64_t ml=(uint64_t)n*8;
	size_t total=n+1; while(total%64!=56) total++;
	uint8_t *m=calloc(total+8,1);
	memcpy(m,d,n); m[n]=0x80;
	for(int i=0;i<8;i++) m[total+i]=(ml>>(56-8*i))&0xff;
#define ROR(x,c) (((x)>>(c))|((x)<<(32-(c))))
	for(size_t off=0;off<total+8;off+=64){
		uint32_t w[64];
		for(int i=0;i<16;i++)
			w[i]=(m[off+4*i]<<24)|(m[off+4*i+1]<<16)|(m[off+4*i+2]<<8)|m[off+4*i+3];
		for(int i=16;i<64;i++){
			uint32_t s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3);
			uint32_t s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);
			w[i]=w[i-16]+s0+w[i-7]+s1;
		}
		uint32_t a=h[0],b=h[1],c=h[2],d2=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
		for(int i=0;i<64;i++){
			uint32_t S1=ROR(e,6)^ROR(e,11)^ROR(e,25);
			uint32_t ch=(e&f)^((~e)&g);
			uint32_t t1=hh+S1+ch+K[i]+w[i];
			uint32_t S0=ROR(a,2)^ROR(a,13)^ROR(a,22);
			uint32_t maj=(a&b)^(a&c)^(b&c);
			uint32_t t2=S0+maj;
			hh=g;g=f;f=e;e=d2+t1;d2=c;c=b;b=a;a=t1+t2;
		}
		h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d2;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
	}
#undef ROR
	free(m);
	for(int i=0;i<8;i++){
		out[4*i]=h[i]>>24; out[4*i+1]=h[i]>>16;
		out[4*i+2]=h[i]>>8; out[4*i+3]=h[i];
	}
}

static void hexs(const uint8_t *in, size_t n, char *out)
{
	static const char hx[]="0123456789abcdef";
	for(size_t i=0;i<n;i++){ out[2*i]=hx[in[i]>>4]; out[2*i+1]=hx[in[i]&15]; }
	out[2*n]=0;
}

static char g_user[32]="admin", g_salt[32]="", g_hash[65]="";

static void passwd_hash(const char *salt, const char *pass, char *out)
{
	char buf[160];
	snprintf(buf, sizeof buf, "%s%s", salt, pass);
	uint8_t d[32];
	sha256((uint8_t *)buf, strlen(buf), d);
	hexs(d, 32, out);
}

static void load_creds(void)
{
	FILE *f = fopen("/userdata/passwd", "r");
	if (f) {
		char line[160];
		if (fgets(line, sizeof line, f)) {
			char *c1 = strchr(line, ':');
			if (c1) {
				*c1 = 0;
				char *c2 = strchr(c1 + 1, ':');
				if (c2) {
					*c2 = 0;
					snprintf(g_user, sizeof g_user, "%s", line);
					snprintf(g_salt, sizeof g_salt, "%s", c1 + 1);
					char *e = c2 + 1;
					while (*e && (*e=='\n'||*e=='\r')) *e-- = 0;
					snprintf(g_hash, sizeof g_hash, "%s", e);
				}
			}
		}
		fclose(f);
		return;
	}
	/* create default admin/admin */
	uint8_t r[8];
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) { if (read(fd, r, 8) < 0) {} close(fd); }
	hexs(r, 8, g_salt);
	passwd_hash(g_salt, "admin", g_hash);
	f = fopen("/userdata/passwd", "w");
	if (f) { fprintf(f, "%s:%s:%s\n", g_user, g_salt, g_hash); fclose(f); }
}

static int check_login(const char *user, const char *pass)
{
	if (strcmp(user, g_user) != 0)
		return 0;
	char h[65];
	passwd_hash(g_salt, pass, h);
	/* constant-time-ish compare */
	int diff = 0;
	for (int i = 0; i < 64; i++)
		diff |= h[i] ^ g_hash[i];
	return diff == 0;
}

#define MAXSESS 64
struct sess { char tok[33]; char user[32]; time_t exp; };
static struct sess g_sess[MAXSESS];
static pthread_mutex_t sess_lock = PTHREAD_MUTEX_INITIALIZER;

static void rand_token(char *out)
{
	uint8_t r[16];
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) { if (read(fd, r, 16) < 0) {} close(fd); }
	hexs(r, 16, out);
}

static void sess_add(const char *tok, const char *user)
{
	pthread_mutex_lock(&sess_lock);
	time_t now = time(NULL);
	int slot = -1;
	for (int i = 0; i < MAXSESS; i++) {
		if (g_sess[i].exp && g_sess[i].exp < now)
			g_sess[i].exp = 0;
		if (slot < 0 && g_sess[i].exp == 0)
			slot = i;
	}
	if (slot < 0) {
		/* overwrite oldest */
		slot = 0;
		for (int i = 1; i < MAXSESS; i++)
			if (g_sess[i].exp < g_sess[slot].exp)
				slot = i;
	}
	snprintf(g_sess[slot].tok, sizeof g_sess[slot].tok, "%s", tok);
	snprintf(g_sess[slot].user, sizeof g_sess[slot].user, "%s", user);
	g_sess[slot].exp = now + 8 * 3600;
	pthread_mutex_unlock(&sess_lock);
}

static int sess_valid(const char *tok)
{
	if (!tok || !tok[0])
		return 0;
	time_t now = time(NULL);
	int ok = 0;
	pthread_mutex_lock(&sess_lock);
	for (int i = 0; i < MAXSESS; i++)
		if (g_sess[i].exp > now && !strcmp(g_sess[i].tok, tok))
			ok = 1;
	pthread_mutex_unlock(&sess_lock);
	return ok;
}

static void sess_del(const char *tok)
{
	if (!tok)
		return;
	pthread_mutex_lock(&sess_lock);
	for (int i = 0; i < MAXSESS; i++)
		if (!strcmp(g_sess[i].tok, tok))
			g_sess[i].exp = 0;
	pthread_mutex_unlock(&sess_lock);
}

static int get_cookie(const char *req, const char *name, char *out, size_t n)
{
	const char *p = req;
	size_t nl = strlen(name);
	while ((p = strstr(p, name))) {
		/* must be preceded by start/; /space */
		if (p != req && p[-1] != ';' && p[-1] != ' ' &&
		    p[-1] != '\r' && p[-1] != '\n') {
			p++;
			continue;
		}
		if (p[nl] == '=') {
			p += nl + 1;
			size_t i = 0;
			while (*p && *p != ';' && *p != '\r' && *p != '\n' && i < n - 1)
				out[i++] = *p++;
			out[i] = 0;
			return 1;
		}
		p++;
	}
	return 0;
}

static void url_decode(char *s)
{
	char *o = s;
	while (*s) {
		if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
			char h[3] = { s[1], s[2], 0 };
			*o++ = (char)strtol(h, NULL, 16);
			s += 3;
		} else if (*s == '+') {
			*o++ = ' '; s++;
		} else {
			*o++ = *s++;
		}
	}
	*o = 0;
}

static int form_value(const char *body, const char *key, char *out, size_t n)
{
	char pat[64];
	snprintf(pat, sizeof pat, "%s=", key);
	const char *p = body;
	while ((p = strstr(p, pat))) {
		if (p == body || p[-1] == '&') {
			p += strlen(pat);
			size_t i = 0;
			while (*p && *p != '&' && i < n - 1)
				out[i++] = *p++;
			out[i] = 0;
			url_decode(out);
			return 1;
		}
		p++;
	}
	return 0;
}

static void send_redirect(int c, const char *loc)
{
	char hdr[256];
	int hl = snprintf(hdr, sizeof hdr,
		"HTTP/1.1 302 Found\r\nLocation: %s\r\n"
		"Content-Length: 0\r\nConnection: close\r\n\r\n", loc);
	send_all(c, hdr, hl);
}

static void send_401(int c)
{
	const char *s = "HTTP/1.1 401 Unauthorized\r\n"
		"Content-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\n"
		"Content-Length: 13\r\nConnection: close\r\n\r\nlogin required";
	send_all(c, s, strlen(s));
}

static void frame_cb(uvc_frame_t *frame, void *ptr)
{
	(void)ptr;
	if (frame->frame_format != UVC_FRAME_FORMAT_MJPEG)
		return;
	const uint8_t *data = frame->data;
	size_t len = frame->data_bytes;
	uint8_t *tmp = NULL;
	size_t tlen = 0;
	if (__atomic_load_n(&g_enc_mode, __ATOMIC_RELAXED) == 1 &&
	    g_tjdec && g_tjenc) {
		if (recompress(data, len, &tmp, &tlen) == 0) {
			data = tmp;
			len = tlen;
		}
	}
	pthread_mutex_lock(&lock);
	if (len > g_jpeg_len) {
		uint8_t *nb = realloc(g_jpeg, len);
		if (!nb) {
			pthread_mutex_unlock(&lock);
			if (tmp)
				tjFree(tmp);
			return;
		}
		g_jpeg = nb;
	}
	memcpy(g_jpeg, data, len);
	g_jpeg_len = len;
	g_seq++;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);
	if (tmp)
		tjFree(tmp);
}

static int start_stream(int w, int h, int want_fps)
{
	const int fps_try[] = { 30, 25, 20, 15, 10, 5, 0 };
	uvc_stream_ctrl_t ctrl;
	uvc_error_t r;
	int fps = 0;

	pthread_mutex_lock(&strm_lock);
	if (want_fps > 0 &&
	    uvc_get_stream_ctrl_format_size(devh, &ctrl,
			UVC_FRAME_FORMAT_MJPEG, w, h, want_fps) == UVC_SUCCESS)
		fps = want_fps;
	for (int j = 0; !fps && fps_try[j]; j++) {
		r = uvc_get_stream_ctrl_format_size(devh, &ctrl,
			UVC_FRAME_FORMAT_MJPEG, w, h, fps_try[j]);
		if (r == UVC_SUCCESS)
			fps = fps_try[j];
	}
	if (!fps) {
		pthread_mutex_unlock(&strm_lock);
		return -1;
	}
	if (strmh) {
		uvc_stream_stop(strmh);
		uvc_stream_close(strmh);
		strmh = NULL;
	}
	r = uvc_stream_open_ctrl(devh, &strmh, &ctrl);
	if (r == UVC_SUCCESS)
		r = uvc_stream_start(strmh, frame_cb, NULL, 0);
	if (r < 0) {
		fprintf(stderr, "start_stream: %s\n", uvc_strerror(r));
		strmh = NULL;
		pthread_mutex_unlock(&strm_lock);
		return -1;
	}
	g_width = w;
	g_height = h;
	g_fps = fps;
	fprintf(stderr, "video: MJPEG %dx%d @%dfps\n", w, h, fps);
	pthread_mutex_unlock(&strm_lock);
	return 0;
}

static int setup_uvc(void)
{
	uvc_error_t r = uvc_init(&ctx, NULL);
	if (r < 0) {
		fprintf(stderr, "uvc_init: %s\n", uvc_strerror(r));
		return -1;
	}
	uvc_device_t *dev;
	r = uvc_find_device(ctx, &dev, 0, 0, NULL);
	if (r < 0) {
		fprintf(stderr, "no UVC device: %s\n", uvc_strerror(r));
		return -1;
	}
	for (int i = 0; i < 15; i++) {
		r = uvc_open(dev, &devh);
		if (r == UVC_SUCCESS)
			break;
		fprintf(stderr, "uvc_open: %s (retry %d)\n", uvc_strerror(r), i);
		sleep(1);
	}
	if (r < 0) {
		fprintf(stderr, "uvc_open failed\n");
		return -1;
	}
	uvc_print_diag(devh, stderr);

	g_tjdec = tjInitDecompress();
	g_tjenc = tjInitCompress();
	if (!g_tjdec || !g_tjenc)
		fprintf(stderr, "turbojpeg init failed\n");

	/* enumerate all MJPEG modes and frame rates */
	g_nmodes = 0;
	for (const uvc_format_desc_t *fd = uvc_get_format_descs(devh);
	     fd && g_nmodes < 32; fd = fd->next) {
		if (memcmp(fd->fourccFormat, "MJPG", 4) != 0)
			continue;
		for (uvc_frame_desc_t *fr = fd->frame_descs;
		     fr && g_nmodes < 32; fr = fr->next) {
			struct vmode *m = &g_modes[g_nmodes++];
			m->w = fr->wWidth;
			m->h = fr->wHeight;
			m->nfps = 0;
			for (int i = 0; fr->intervals && fr->intervals[i] &&
					m->nfps < 8; i++) {
				int f = (int)(10000000u / fr->intervals[i]);
				int dup = 0;
				for (int k = 0; k < m->nfps; k++)
					if (m->fps[k] == f)
						dup = 1;
				if (!dup)
					m->fps[m->nfps++] = f;
			}
		}
	}

	/* prefer 720p for low latency; fall back to others */
	const int sizes[][2] = {
		{1280, 720}, {1920, 1080}, {1024, 768}, {640, 480}
	};
	for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
		if (start_stream(sizes[i][0], sizes[i][1], 30) == 0)
			return 0;
	for (int i = 0; i < g_nmodes; i++)
		if (start_stream(g_modes[i].w, g_modes[i].h,
				 g_modes[i].nfps ? g_modes[i].fps[0] : 0) == 0)
			return 0;
	fprintf(stderr, "no usable MJPEG mode found\n");
	return -1;
}

/* ---------------------------- HID proxy ---------------------------------- */

static int hid_connect(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(HID_PORT);
	inet_pton(AF_INET, HID_HOST, &sa.sin_addr);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(fd);
		return -1;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

/* forward one command line, fill reply. returns 0 on ok */
static int hid_cmd(const char *cmd, size_t clen, char *reply, size_t rl)
{
	pthread_mutex_lock(&hid_lock);
	if (hid_fd < 0)
		hid_fd = hid_connect();
	if (hid_fd < 0) {
		pthread_mutex_unlock(&hid_lock);
		return -1;
	}
	char line[512];
	size_t n = clen < sizeof(line) - 2 ? clen : sizeof(line) - 2;
	memcpy(line, cmd, n);
	line[n++] = '\n';
	if (write(hid_fd, line, n) < 0) {
		close(hid_fd);
		hid_fd = hid_connect();
		if (hid_fd < 0) {
			pthread_mutex_unlock(&hid_lock);
			return -1;
		}
		write(hid_fd, line, n);
	}
	ssize_t got = read(hid_fd, reply, rl - 1);
	if (got < 0) {
		close(hid_fd);
		hid_fd = -1;
		pthread_mutex_unlock(&hid_lock);
		return -1;
	}
	reply[got] = 0;
	pthread_mutex_unlock(&hid_lock);
	return 0;
}

/* ------------------------------- HTTP ------------------------------------ */

static ssize_t send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	size_t left = len;
	while (left > 0) {
		ssize_t n = write(fd, p, left);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		left -= n;
	}
	return (ssize_t)len;
}

static int read_all(int fd, void *buf, size_t len)
{
	char *p = buf;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static void serve_status(int c)
{
	char body[256], hdr[288];
	int bm = __atomic_load_n(&g_enc_mode, __ATOMIC_RELAXED);
	int bq = __atomic_load_n(&g_quality, __ATOMIC_RELAXED);
	int pwr = cfg_status >= 0 ? gpio_read(cfg_status) : -1;
	int caps = -1, num = -1;
	char rep[64] = {0};
	if (hid_cmd("led", 3, rep, sizeof rep) == 0)
		sscanf(rep, "led %d %d", &caps, &num);
	int bl = snprintf(body, sizeof body,
		"MJPEG %dx%d @%d fps, frame %lu, tx %llu, hid=%s, enc=%s%d, sz=%zu, pwr=%d, pgpio=%d, rgpio=%d, sgpio=%d, caps=%d, num=%d\n",
		g_width, g_height, g_fps, g_seq,
		__atomic_load_n(&g_tx_bytes, __ATOMIC_RELAXED),
		hid_fd >= 0 ? "up" : "down",
		bm ? "q" : "pass", bm ? bq : 0,
		__atomic_load_n(&g_jpeg_len, __ATOMIC_RELAXED),
		pwr, cfg_power, cfg_reset, cfg_status, caps, num);
	int hl = snprintf(hdr, sizeof hdr,
		"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Content-Length: %d\r\nConnection: close\r\n\r\n", bl);
	send_all(c, hdr, hl);
	send_all(c, body, bl);
}

static void serve_file(int c, const char *path, const char *ctype)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
				 "Connection: close\r\n\r\n";
		send_all(c, nf, strlen(nf));
		return;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char hdr[192];
	int hl = snprintf(hdr, sizeof hdr,
		"HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
		"Cache-Control: no-cache\r\nContent-Length: %ld\r\n"
		"Connection: close\r\n\r\n", ctype, sz);
	send_all(c, hdr, hl);
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		if (send_all(c, buf, n) < 0)
			break;
	fclose(f);
}

static void serve_snapshot(int c)
{
	pthread_mutex_lock(&lock);
	unsigned long seq = g_seq;
	while (g_seq == seq && !g_stop)
		pthread_cond_wait(&cond, &lock);
	uint8_t *buf = malloc(g_jpeg_len);
	size_t len = g_jpeg_len;
	if (buf)
		memcpy(buf, g_jpeg, len);
	pthread_mutex_unlock(&lock);
	if (buf) {
		char hdr[192];
		int hl = snprintf(hdr, sizeof hdr,
			"HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
			"Access-Control-Allow-Origin: *\r\n"
			"Content-Length: %zu\r\nConnection: close\r\n\r\n", len);
		send_all(c, hdr, hl);
		send_all(c, buf, len);
		free(buf);
	}
}

static void serve_stream(int c)
{
	static const char shdr[] =
		"HTTP/1.1 200 OK\r\n"
		"Connection: close\r\n"
		"Cache-Control: no-cache\r\nPragma: no-cache\r\n"
		"Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
	if (send_all(c, shdr, sizeof shdr - 1) < 0)
		return;
	unsigned long last = 0;
	while (!g_stop) {
		pthread_mutex_lock(&lock);
		while (g_seq == last && !g_stop)
			pthread_cond_wait(&cond, &lock);
		last = g_seq;
		uint8_t *buf = malloc(g_jpeg_len);
		size_t len = g_jpeg_len;
		if (buf)
			memcpy(buf, g_jpeg, len);
		pthread_mutex_unlock(&lock);
		if (!buf || len == 0) {
			free(buf);
			continue;
		}
		char fh[96];
		int fl = snprintf(fh, sizeof fh,
			"--frame\r\nContent-Type: image/jpeg\r\n"
			"Content-Length: %zu\r\n\r\n", len);
		if (send_all(c, fh, fl) < 0 || send_all(c, buf, len) < 0 ||
		    send_all(c, "\r\n", 2) < 0) {
			free(buf);
			break;
		}
		__atomic_add_fetch(&g_tx_bytes, len, __ATOMIC_RELAXED);
		free(buf);
	}
}

/* ----------------------------- WebSocket --------------------------------- */

static void sha1(const uint8_t *d, size_t n, uint8_t out[20])
{
	uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
		 h3 = 0x10325476, h4 = 0xC3D2E1F0;
	uint64_t ml = (uint64_t)n * 8;
	size_t total = n + 1;
	while (total % 64 != 56)
		total++;
	uint8_t *msg = calloc(total + 8, 1);
	memcpy(msg, d, n);
	msg[n] = 0x80;
	for (int i = 0; i < 8; i++)
		msg[total + i] = (uint8_t)(ml >> (56 - 8 * i));
#define ROL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))
	for (size_t off = 0; off < total + 8; off += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (msg[off + 4*i] << 24) | (msg[off + 4*i+1] << 16) |
			       (msg[off + 4*i+2] << 8) | msg[off + 4*i+3];
		for (int i = 16; i < 80; i++)
			w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
		uint32_t a=h0,b=h1,c=h2,d2=h3,e=h4;
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) { f = (b & c) | ((~b) & d2); k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d2; k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b&c)|(b&d2)|(c&d2); k = 0x8F1BBCDC; }
			else { f = b ^ c ^ d2; k = 0xCA62C1D6; }
			uint32_t t = ROL(a,5) + f + e + k + w[i];
			e = d2; d2 = c; c = ROL(b,30); b = a; a = t;
		}
		h0+=a; h1+=b; h2+=c; h3+=d2; h4+=e;
	}
	free(msg);
	uint32_t hh[5] = { h0, h1, h2, h3, h4 };
	for (int i = 0; i < 5; i++) {
		out[4*i]   = hh[i] >> 24;
		out[4*i+1] = hh[i] >> 16;
		out[4*i+2] = hh[i] >> 8;
		out[4*i+3] = hh[i];
	}
#undef ROL
}

static void b64enc(const uint8_t *in, size_t n, char *out)
{
	static const char t[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, o = 0;
	for (i = 0; i + 2 < n; i += 3) {
		out[o++] = t[in[i] >> 2];
		out[o++] = t[((in[i] & 3) << 4) | (in[i+1] >> 4)];
		out[o++] = t[((in[i+1] & 15) << 2) | (in[i+2] >> 6)];
		out[o++] = t[in[i+2] & 63];
	}
	if (i < n) {
		out[o++] = t[in[i] >> 2];
		if (i + 1 < n) {
			out[o++] = t[((in[i] & 3) << 4) | (in[i+1] >> 4)];
			out[o++] = t[(in[i+1] & 15) << 2];
		} else {
			out[o++] = t[(in[i] & 3) << 4];
			out[o++] = '=';
		}
		out[o++] = '=';
	}
	out[o] = 0;
}

static void ws_pong(int c)
{
	/* send an unmasked pong with empty payload */
	uint8_t f[2] = { 0x8A, 0x00 };
	send_all(c, f, 2);
}

static int ws_accept(int c, char *req)
{
	char *k = strcasestr(req, "Sec-WebSocket-Key:");
	if (!k) {
		const char *bad = "HTTP/1.1 400 Bad Request\r\nContent-Length:0\r\n\r\n";
		send_all(c, bad, strlen(bad));
		return -1;
	}
	k += strlen("Sec-WebSocket-Key:");
	while (*k == ' ')
		k++;
	char key[128];
	int kl = 0;
	while (*k && *k != '\r' && *k != '\n' && kl < (int)sizeof key - 1)
		key[kl++] = *k++;
	key[kl] = 0;

	const char *GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	char tmp[256];
	snprintf(tmp, sizeof tmp, "%s%s", key, GUID);
	uint8_t digest[20];
	sha1((uint8_t *)tmp, strlen(tmp), digest);
	char accept[64];
	b64enc(digest, 20, accept);

	char resp[256];
	int rl = snprintf(resp, sizeof resp,
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	if (send_all(c, resp, rl) < 0)
		return -1;
	return 0;
}

static void ws_serve(int c, char *req)
{
	if (ws_accept(c, req) != 0)
		return;

	/* frame loop */
	for (;;) {
		uint8_t h[2];
		if (read_all(c, h, 2) < 0)
			break;
		int opcode = h[0] & 0x0F;
		int masked = h[1] & 0x80;
		uint64_t len = h[1] & 0x7F;
		if (len == 126) {
			uint8_t e[2];
			if (read_all(c, e, 2) < 0)
				break;
			len = (e[0] << 8) | e[1];
		} else if (len == 127) {
			uint8_t e[8];
			if (read_all(c, e, 8) < 0)
				break;
			len = 0;
			for (int i = 0; i < 8; i++)
				len = (len << 8) | e[i];
		}
		uint8_t mask[4] = {0};
		if (masked && read_all(c, mask, 4) < 0)
			break;
		if (len > 4096)
			break;
		char payload[4097];
		if (len && read_all(c, payload, len) < 0)
			break;
		for (uint64_t i = 0; i < len; i++)
			payload[i] ^= mask[i & 3];
		payload[len] = 0;

		if (opcode == 0x8)
			break;
		else if (opcode == 0x9)
			ws_pong(c);
		else if (opcode == 0x1) {
			char reply[64] = {0};
			hid_cmd(payload, len, reply, sizeof reply);
		}
	}
}

/* WebSocket video: push each new JPEG frame as a binary message */
static void ws_video_serve(int c, char *req)
{
	if (ws_accept(c, req) != 0)
		return;
	unsigned long last = 0;
	uint8_t *buf = NULL;
	size_t cap = 0;
	while (!g_stop) {
		pthread_mutex_lock(&lock);
		while (g_seq == last && !g_stop)
			pthread_cond_wait(&cond, &lock);
		last = g_seq;
		size_t len = g_jpeg_len;
		if (len > cap) {
			uint8_t *nb = realloc(buf, len);
			if (!nb) { pthread_mutex_unlock(&lock); break; }
			buf = nb; cap = len;
		}
		if (len)
			memcpy(buf, g_jpeg, len);
		pthread_mutex_unlock(&lock);
		if (!len)
			continue;
		uint8_t hdr[10];
		int hl;
		hdr[0] = 0x82;			/* FIN + binary */
		if (len < 126) {
			hdr[1] = (uint8_t)len; hl = 2;
		} else if (len < 65536) {
			hdr[1] = 126;
			hdr[2] = (len >> 8) & 0xff; hdr[3] = len & 0xff; hl = 4;
		} else {
			hdr[1] = 127;
			for (int i = 0; i < 8; i++)
				hdr[2 + i] = (uint8_t)(len >> (56 - 8 * i));
			hl = 10;
		}
		if (send_all(c, hdr, hl) < 0 || send_all(c, buf, len) < 0)
			break;
	}
	free(buf);
}

/* -------------------- audio (ALSA capture over WebSocket) ---------------- */
static pthread_mutex_t a_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t a_cond = PTHREAD_COND_INITIALIZER;
static uint8_t a_buf[32768];
static int a_len = 0;
static volatile unsigned long a_seq = 0;
static char g_audio_dev[64] = "hw:0,0";
static int g_audio_rate = 48000, g_audio_ch = 2;
static int g_audio_on = 1;

static void load_audio_cfg(void)
{
	FILE *f = fopen("/userdata/audio.conf", "r");
	if (!f)
		return;
	char line[160];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = line;
		while (*k == ' ' || *k == '\t') k++;
		char *v = eq + 1;
		while (*v == ' ' || *v == '\t') v++;
		char *e = v + strlen(v);
		while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
			*--e = 0;
		if (!strcmp(k, "AUDIO_DEV"))
			snprintf(g_audio_dev, sizeof g_audio_dev, "%s", v);
		else if (!strcmp(k, "AUDIO_RATE"))
			g_audio_rate = atoi(v);
		else if (!strcmp(k, "AUDIO_CH"))
			g_audio_ch = atoi(v);
		else if (!strcmp(k, "AUDIO_ENABLE"))
			g_audio_on = atoi(v);
	}
	fclose(f);
}

/* Audio: stream raw PCM from a FIFO written by an external 'arecord'
 * (started by the init script).  This runs inside the /aws client thread
 * (same model as /stream) so we never fork() nor spawn a startup thread. */
static void ws_audio_serve(int c, char *req)
{
	if (ws_accept(c, req) != 0)
		return;
	int fd = open("/run/rk.pcm", O_RDONLY);	/* blocks until arecord writes */
	if (fd < 0)
		return;
	uint8_t buf[4096];
	while (!g_stop) {
		ssize_t r = read(fd, buf, sizeof buf);
		if (r <= 0)
			break;
		uint8_t hdr[10];
		int hl;
		size_t L = r;
		hdr[0] = 0x82;
		if (L < 126) { hdr[1] = L; hl = 2; }
		else if (L < 65536) { hdr[1] = 126; hdr[2] = (L >> 8) & 0xff; hdr[3] = L & 0xff; hl = 4; }
		else { hdr[1] = 127; for (int i = 0; i < 8; i++) hdr[2 + i] = (L >> (56 - 8 * i)) & 0xff; hl = 10; }
		if (send_all(c, hdr, hl) < 0 || send_all(c, buf, L) < 0)
			break;
	}
	close(fd);
}

/* ------------------------------ dispatch --------------------------------- */

static int gpio_set(int n, const char *val, const char *dir)
{
	char p[128], s[16];
	snprintf(p, sizeof p, "/sys/class/gpio/gpio%d", n);
	if (access(p, F_OK) != 0) {
		snprintf(s, sizeof s, "%d", n);
		int fd = open("/sys/class/gpio/export", O_WRONLY);
		if (fd >= 0) { write(fd, s, strlen(s)); close(fd); }
	}
	if (access(p, F_OK) != 0)
		return -1;
	if (dir) {
		snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/direction", n);
		int fd = open(p, O_WRONLY);
		if (fd >= 0) { write(fd, dir, strlen(dir)); close(fd); }
	}
	if (val) {
		snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", n);
		int fd = open(p, O_WRONLY);
		if (fd < 0)
			return -1;
		write(fd, val, strlen(val));
		close(fd);
	}
	return 0;
}

static int gpio_read(int n)
{
	char p[128], b[8];
	snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", n);
	int fd = open(p, O_RDONLY);
	if (fd < 0)
		return -1;
	int r = pread(fd, b, sizeof b - 1, 0);
	close(fd);
	if (r <= 0)
		return -1;
	b[r] = 0;
	return atoi(b);
}

struct client { int fd; };

static void *client_thread(void *arg)
{
	int c = ((struct client *)arg)->fd;
	free(arg);

	char req[8192];
	int n = read(c, req, sizeof req - 1);
	if (n <= 0) {
		close(c);
		return NULL;
	}
	req[n] = 0;

	/* ensure the full request body is read (headers/body may arrive in
	 * separate segments, e.g. through a reverse proxy) */
	{
		char *he = strstr(req, "\r\n\r\n");
		if (he) {
			char *cl = strcasestr(req, "Content-Length:");
			if (cl) {
				size_t want = (size_t)strtoul(cl + 15, NULL, 10);
				size_t have = (size_t)(n - ((he + 4) - req));
				while (have < want && n < (int)sizeof req - 1) {
					int m = read(c, req + n, sizeof req - 1 - n);
					if (m <= 0)
						break;
					n += m;
					have += m;
					req[n] = 0;
				}
			}
		}
	}

	/* -------------------- auth gate -------------------- */
	const char *path = req;
	if (!strncmp(req, "GET ", 4)) path = req + 4;
	else if (!strncmp(req, "POST ", 5)) path = req + 5;

	int public_ep = !strncmp(path, "/login", 6) || !strncmp(path, "/logout", 7) ||
			!strncmp(path, "/favicon", 8);
	char atok[33] = {0};
	int authed = get_cookie(req, "sid", atok, sizeof atok) && sess_valid(atok);

	if (!public_ep && !authed) {
		int is_data = !strncmp(path, "/api", 4) || !strncmp(path, "/stream", 7) ||
			!strncmp(path, "/snapshot", 9) || !strncmp(path, "/status", 7) ||
			!strncmp(path, "/setres", 7) || !strncmp(path, "/setenc", 7) ||
			!strncmp(path, "/gpio", 5) || !strncmp(path, "/gpiocfg", 8) ||
			!strncmp(path, "/ws", 3) || !strncmp(path, "/vws", 4) ||
			!strncmp(path, "/aws", 4) || !strncmp(path, "/admin/exec", 11);
		if (is_data)
			send_401(c);
		else
			send_redirect(c, "/login");
		close(c);
		return NULL;
	}

	char *body = strstr(req, "\r\n\r\n");
	if (body)
		body += 4;

	/* -------------------- auth routes -------------------- */
	if (!strncmp(req, "GET /login", 10)) {
		serve_file(c, "/userdata/login.html", "text/html; charset=utf-8");
	} else if (!strncmp(req, "POST /login", 11)) {
		char user[64] = {0}, pass[128] = {0};
		if (body) {
			form_value(body, "user", user, sizeof user);
			form_value(body, "pass", pass, sizeof pass);
		}
		if (check_login(user, pass)) {
			char t[33];
			rand_token(t);
			sess_add(t, user);
			char hdr[256];
			int hl = snprintf(hdr, sizeof hdr,
				"HTTP/1.1 302 Found\r\nLocation: /\r\n"
				"Set-Cookie: sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800\r\n"
				"Content-Length: 0\r\nConnection: close\r\n\r\n", t);
			send_all(c, hdr, hl);
		} else {
			send_redirect(c, "/login?e=1");
		}
	} else if (!strncmp(req, "GET /logout", 11)) {
		sess_del(atok);
		const char *h = "HTTP/1.1 302 Found\r\nLocation: /login\r\n"
			"Set-Cookie: sid=; Path=/; Max-Age=0\r\n"
			"Content-Length: 0\r\nConnection: close\r\n\r\n";
		send_all(c, h, strlen(h));
	} else if (!strncmp(req, "POST /admin/exec", 16) ||
		   !strncmp(req, "GET /admin/exec", 15)) {
		char cmd[1024] = {0};
		if (body)
			form_value(body, "cmd", cmd, sizeof cmd);
		if (!cmd[0])
			strcpy(cmd, "uptime; free; df -h; ps");
		FILE *pp = popen(cmd, "r");
		char out[65536];
		size_t o = 0;
		if (pp) {
			char buf[4096];
			size_t r;
			while ((r = fread(buf, 1, sizeof buf, pp)) > 0 && o < sizeof out - 1) {
				size_t k = r;
				if (o + k > sizeof out - 1)
					k = sizeof out - 1 - o;
				memcpy(out + o, buf, k);
				o += k;
			}
			pclose(pp);
		}
		out[o] = 0;
		char hdr[192];
		int hl = snprintf(hdr, sizeof hdr,
			"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
			"Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n"
			"Connection: close\r\n\r\n", o);
		send_all(c, hdr, hl);
		send_all(c, out, o);
	} else if (!strncmp(req, "POST /passwd", 12)) {
		char oldp[128] = {0}, nwp[128] = {0};
		if (body) {
			form_value(body, "old", oldp, sizeof oldp);
			form_value(body, "new", nwp, sizeof nwp);
		}
		int ok = strlen(nwp) >= 4 && check_login(g_user, oldp);
		if (ok) {
			passwd_hash(g_salt, nwp, g_hash);
			FILE *f = fopen("/userdata/passwd", "w");
			if (f) { fprintf(f, "%s:%s:%s\n", g_user, g_salt, g_hash); fclose(f); }
		}
		const char *o = ok ? "ok" : "err";
		char hdr[160];
		int hl = snprintf(hdr, sizeof hdr,
			"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
			"Content-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(o), o);
		send_all(c, hdr, hl);
	} else if (!strncmp(req, "GET /admin", 10)) {
		serve_file(c, "/userdata/admin.html", "text/html; charset=utf-8");
	} else if (!strncmp(req, "GET /aws", 8)) {
		ws_audio_serve(c, req);
	} else if (!strncmp(req, "GET /vws", 8)) {
		ws_video_serve(c, req);
	} else if (!strncmp(req, "GET /ws", 7)) {
		ws_serve(c, req);
	} else if (!strncmp(req, "POST /api/hid", 13)) {
		char *body = strstr(req, "\r\n\r\n");
		char reply[64] = {0};
		int rc = -1;
		if (body) {
			body += 4;
			char *cl = strcasestr(req, "Content-Length:");
			size_t want = cl ? (size_t)strtoul(cl + 15, NULL, 10) : 0;
			size_t have = (size_t)(n - (body - req));
			while (have < want) {
				int m = read(c, req + n, sizeof req - 1 - n);
				if (m <= 0)
					break;
				n += m; have += m; req[n] = 0;
			}
			rc = hid_cmd(body, have, reply, sizeof reply);
		}
		const char *out = rc == 0 ? reply : "err";
		char hdr[160];
		int hl = snprintf(hdr, sizeof hdr,
			"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
			"Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n"
			"Connection: close\r\n\r\n%s", strlen(out), out);
		send_all(c, hdr, hl);
	} else if (!strncmp(req, "GET /formats", 12)) {
		char body[2048];
		int o = 0;
		o += snprintf(body + o, sizeof body - o, "[");
		for (int i = 0; i < g_nmodes; i++) {
			o += snprintf(body + o, sizeof body - o,
				"%s{\"w\":%d,\"h\":%d,\"fps\":[",
				i ? "," : "", g_modes[i].w, g_modes[i].h);
			for (int k = 0; k < g_modes[i].nfps; k++)
				o += snprintf(body + o, sizeof body - o,
					"%s%d", k ? "," : "", g_modes[i].fps[k]);
			o += snprintf(body + o, sizeof body - o, "]}");
		}
		o += snprintf(body + o, sizeof body - o, "]");
		char hdr[192];
		int hl = snprintf(hdr, sizeof hdr,
			"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
			"Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n"
			"Connection: close\r\n\r\n", o);
		send_all(c, hdr, hl);
		send_all(c, body, o);
	} else if (!strncmp(req, "GET /gpio?", 10)) {
		int n = -1, ms = 500;
		char act[16] = "pulse";
		char *q = strchr(req, '?');
		if (q)
			sscanf(q, "?n=%d&act=%15[a-z]&ms=%d", &n, act, &ms);
		char resp[16] = "err";
		if (n >= 0) {
			if (!strcmp(act, "read")) {
				int v = gpio_read(n);
				if (v >= 0)
					snprintf(resp, sizeof resp, "%d", v);
			} else if (!strcmp(act, "high")) {
				if (gpio_set(n, "1", "out") == 0)
					strcpy(resp, "ok");
			} else if (!strcmp(act, "low")) {
				if (gpio_set(n, "0", "out") == 0)
					strcpy(resp, "ok");
			} else {
				gpio_set(n, "0", "out");
				gpio_set(n, "1", NULL);
				usleep((ms > 0 && ms < 10000 ? ms : 500) * 1000);
				if (gpio_set(n, "0", NULL) == 0)
					strcpy(resp, "ok");
			}
		}
		char hdr[160];
		int hl = snprintf(hdr, sizeof hdr,
			"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
			"Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n"
			"Connection: close\r\n\r\n%s", strlen(resp), resp);
		send_all(c, hdr, hl);
	} else if (!strncmp(req, "GET /gpiocfg", 12)) {
		int p = cfg_power, r = cfg_reset, s = cfg_status;
		char *q = strchr(req, '?');
		if (q)
			sscanf(q, "?power=%d&reset=%d&status=%d", &p, &r, &s);
		cfg_power = p;
		cfg_reset = r;
		cfg_status = s;
		FILE *f = fopen("/userdata/kvm.conf", "w");
		if (f) {
			fprintf(f, "POWER_GPIO=%d\nRESET_GPIO=%d\nSTATUS_GPIO=%d\n",
				p, r, s);
			fclose(f);
		}
		if (cfg_power >= 0)
			gpio_set(cfg_power, "0", "out");
		if (cfg_reset >= 0)
			gpio_set(cfg_reset, "0", "out");
		if (cfg_status >= 0)
			gpio_set(cfg_status, NULL, "in");
		char hdr[160];
		const char *body = "{\"power\":%d,\"reset\":%d,\"status\":%d}";
		char bb[128];
		int bl = snprintf(bb, sizeof bb, body, p, r, s);
		int hl = snprintf(hdr, sizeof hdr,
			"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
			"Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n"
			"Connection: close\r\n\r\n", bl);
		send_all(c, hdr, hl);
		send_all(c, bb, bl);
	} else if (!strncmp(req, "GET /setenc", 11)) {
		int mode = 0, q = 0;
		char *qs = strchr(req, '?');
		if (qs) sscanf(qs, "?mode=%d&quality=%d", &mode, &q);
		if (q > 0) {
			if (q < 20) q = 20;
			if (q > 95) q = 95;
			__atomic_store_n(&g_quality, q, __ATOMIC_RELAXED);
		}
		__atomic_store_n(&g_enc_mode, mode ? 1 : 0, __ATOMIC_RELAXED);
		const char *ok = "HTTP/1.1 200 OK\r\nContent-Length:2\r\n"
				 "Connection: close\r\n\r\nok";
		send_all(c, ok, strlen(ok));
	} else if (!strncmp(req, "GET /setres", 11)) {
		int w = 0, h = 0, fps = 0;
		char *q = strchr(req, '?');
		if (q) sscanf(q, "?w=%d&h=%d&fps=%d", &w, &h, &fps);
		if (w > 0 && h > 0 && start_stream(w, h, fps) == 0) {
			const char *ok = "HTTP/1.1 200 OK\r\nContent-Length:2\r\n"
					 "Connection: close\r\n\r\nok";
			send_all(c, ok, strlen(ok));
		} else {
			const char *e = "HTTP/1.1 500 Error\r\nContent-Length:3\r\n"
					"Connection: close\r\n\r\nerr";
			send_all(c, e, strlen(e));
		}
	} else if (!strncmp(req, "OPTIONS ", 8)) {
		const char *o = "HTTP/1.1 204 No Content\r\n"
			"Access-Control-Allow-Origin: *\r\n"
			"Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
			"Access-Control-Allow-Headers: *\r\nContent-Length: 0\r\n"
			"Connection: close\r\n\r\n";
		send_all(c, o, strlen(o));
	} else if (!strncmp(req, "GET /status", 11)) {
		serve_status(c);
	} else if (!strncmp(req, "GET /snapshot", 13)) {
		serve_snapshot(c);
	} else if (!strncmp(req, "GET /stream", 11)) {
		serve_stream(c);
	} else if (!strncmp(req, "GET / ", 6) ||
		   !strncmp(req, "GET /index", 10)) {
		serve_file(c, HTML_FILE, "text/html; charset=utf-8");
	} else {
		const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
				 "Connection: close\r\n\r\n";
		send_all(c, nf, strlen(nf));
	}
	close(c);
	return NULL;
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	load_cfg();
	load_creds();
	load_audio_cfg();

	if (setup_uvc() < 0)
		return 1;

	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) {
		perror("socket");
		return 1;
	}
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(LISTEN_PORT);
	if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("bind");
		return 1;
	}
	listen(srv, 16);
	fprintf(stderr, "video: http://0.0.0.0:%d/  (MJPEG %dx%d @%d)\n",
		LISTEN_PORT, g_width, g_height, g_fps);

	while (!g_stop) {
		int c = accept(srv, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		struct client *cl = malloc(sizeof *cl);
		cl->fd = c;
		pthread_t th;
		if (pthread_create(&th, NULL, client_thread, cl) != 0) {
			close(c);
			free(cl);
		} else {
			pthread_detach(th);
		}
	}

	if (strmh) {
		uvc_stream_stop(strmh);
		uvc_stream_close(strmh);
	}
	if (devh)
		uvc_close(devh);
	if (ctx)
		uvc_exit(ctx);
	return 0;
}
