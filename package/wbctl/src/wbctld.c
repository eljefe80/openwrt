/*
 * wbctld - WattBox WB-300VB-IP-5 control daemon for OpenWrt.
 *
 * Talks to the PIC MCU on /dev/ttyS1 @ B2400 8N1 (protocol reverse-engineered;
 * summarized below and in README.md). Publishes state + Home Assistant MQTT
 * discovery and accepts interactive commands over MQTT.
 *
 * PIC wire protocol: TX "$<cmd>\r"  ->  RX "(<cmd><data>\r".
 *   outlet on/off : $T0nY / $T0nN   (n = 1..N)   [confirmed live]
 *   voltage       : $V -> (Vdddd    (x0.1 V)     [confirmed live]
 *   current       : $I -> (Iddd     (x0.1 A)     [confirmed live]
 *   model         : $M -> (M<str>                [confirmed live]
 *   outlet status : $Q -> (Q<d..>  (1 digit/outlet, outlet 1 leftmost) [confirmed live]
 *   LEDs          : $L + 6 digits                [from RE; mapping cfg-driven]
 * Outlet state is set optimistically on command for instant feedback and then
 * reconciled from the real '$Q' readback each poll (so external changes --
 * schedule, power button, reboot -- are reflected). '$S' is only a set/echo
 * buffer, not the status.
 *
 * Config: plain key=value file (rendered from UCI by the init script).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <time.h>
#include <sys/select.h>
#include <signal.h>
#include <pthread.h>
#include <mosquitto.h>

#define MAXOUT 16
#define NLED 3

struct cfg {
	char host[128];
	int port;
	char user[64];
	char pass[64];
	char base[64]; /* base MQTT topic, e.g. "wattbox" */
	char disco[64]; /* HA discovery prefix, e.g. "homeassistant" */
	char serial[64]; /* /dev/ttyS1 */
	char devname[64]; /* device name for HA */
	char uid[64]; /* unique id prefix, e.g. wattbox_<serial> */
	int poll; /* seconds */
	int nout; /* number of outlets */
	double vscale, iscale; /* raw -> real (default 0.1) */
	char oname[MAXOUT][64]; /* outlet names */
	char led_name[NLED][32]; /* HA display names for the 3 bi-color LEDs */
	char pon[MAXOUT][8]; /* per-outlet power-on state: "last"|"on"|"off" */
	char statefile[96]; /* persisted last-known outlet state (overlay) */
	int pondelay; /* ms stagger between restore commands (inrush) */
	int reset_delay; /* seconds off during an outlet power-cycle */
} C;

static const char *LED_KEY[NLED] = { "internet", "system", "auto_reboot" };
static const char *LED_NAMED[NLED] = { "Internet LED", "System LED",
				       "Auto-Reboot LED" };
/* $L 6-digit pattern: 3 bi-color LEDs, each a red digit + a green digit
 * (confirmed live 2026-08-23): internet red=0 grn=1, system red=2 grn=3,
 * auto_reboot red=4 grn=5. There is NO Power LED on the $L bus -- it is a
 * hardwired power indicator. */
static const int LED_RED[NLED] = { 0, 2, 4 };
static const int LED_GRN[NLED] = { 1, 3, 5 };
static const char *LCOLOR[4] = { "off", "green", "red",
				 "amber" }; /* led_color value */
static int outlet_state[MAXOUT]; /* 0/1; set on cmd, reconciled from $Q */
static time_t
	reset_at[MAXOUT]; /* !=0: time() to switch a resetting outlet back on */
static int led_color[NLED]; /* manual colour: 0=off 1=green 2=red 3=amber */
static int led_mode[NLED]; /* 0=auto (status-driven), 1=manual (HA-set) */
static char led_last[7] = ""; /* last $L pattern actually sent (change-cache) */
/* health signals that drive the auto LED behaviour */
static volatile int mqtt_up = 0; /* MQTT/controller link currently connected */
static volatile int ever_up = 0; /* connected at least once since boot */
static volatile int pic_ok = 1; /* last PIC status poll succeeded */
static int blink_on = 0; /* blink phase for flashing auto states */
static int serfd = -1;
static pthread_mutex_t serlock =
	PTHREAD_MUTEX_INITIALIZER; /* serialises /dev/ttyS1 */
static struct mosquitto *mq;
static volatile int running = 1;

static void on_sig(int s)
{
	(void)s;
	running = 0;
}

/* ---- serial ---- */
static int ser_open(const char *dev)
{
	int fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return -1;
	}
	struct termios t;
	memset(&t, 0, sizeof t);
	tcgetattr(fd, &t);
	cfmakeraw(&t);
	cfsetispeed(&t, B2400);
	cfsetospeed(&t, B2400);
	t.c_cflag |= (CLOCAL | CREAD);
	t.c_cflag &= ~PARENB;
	t.c_cflag &= ~CSTOPB;
	t.c_cflag &= ~CSIZE;
	t.c_cflag |= CS8;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(fd, TCSANOW, &t);
	tcflush(fd, TCIOFLUSH);
	return fd;
}
/* send "$<cmd>\r", collect reply until '\r' or timeout; strip leading "(<letter>" */
static int pic_cmd(const char *cmd, char *out, int outsz)
{
	if (serfd < 0)
		return -1;
	/* Serialise ALL PIC access: poll_outlets/poll_meter (main thread),
	 * set_outlet/set_led (mqtt callback thread) and the LED blink tick share
	 * one 2400-baud port; concurrent transactions garble each other. */
	pthread_mutex_lock(&serlock);
	char tx[64];
	snprintf(tx, sizeof tx, "$%s\r", cmd);
	tcflush(serfd, TCIOFLUSH);
	if (write(serfd, tx, strlen(tx)) < 0) {
		pthread_mutex_unlock(&serlock);
		return -1;
	}
	char buf[128];
	int n = 0;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long ms = (now.tv_sec - t0.tv_sec) * 1000 +
			  (now.tv_nsec - t0.tv_nsec) / 1000000;
		if (ms > 800)
			break;
		struct timeval tv = { 0, 60000 };
		fd_set r;
		FD_ZERO(&r);
		FD_SET(serfd, &r);
		if (select(serfd + 1, &r, 0, 0, &tv) > 0) {
			char c;
			int k = read(serfd, &c, 1);
			if (k == 1) {
				if (c == '\r') {
					break;
				}
				if (n < (int)sizeof buf - 1)
					buf[n++] = c;
			}
		}
	}
	buf[n] = 0;
	/* reply "(<letter><data>" -> data starts at index 2 */
	if (out) {
		const char *d = (n >= 2 && buf[0] == '(') ? buf + 2 : buf;
		snprintf(out, outsz, "%s", d);
	}
	pthread_mutex_unlock(&serlock);
	return n;
}

/* ---- config ---- */
static void trim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
		*--e = 0;
}
static void load_cfg(const char *path)
{
	/* defaults */
	strcpy(C.host, "127.0.0.1");
	C.port = 1883;
	C.base[0] = 0;
	strcpy(C.disco, "homeassistant");
	strcpy(C.serial, "/dev/ttyS1");
	strcpy(C.devname, "WattBox");
	strcpy(C.uid, "wattbox");
	C.poll = 10;
	C.nout = 5;
	C.vscale = 0.1;
	C.iscale = 0.1;
	strcpy(C.base, "wattbox");
	for (int i = 0; i < MAXOUT; i++)
		snprintf(C.oname[i], 64, "Outlet %d", i + 1);
	for (int i = 0; i < NLED; i++)
		snprintf(C.led_name[i], 32, "%s", LED_NAMED[i]);
	for (int i = 0; i < MAXOUT; i++)
		strcpy(C.pon[i], "last");
	strcpy(C.statefile, "/etc/wbctl.state");
	C.pondelay = 0;
	C.reset_delay = 5;
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "no config %s, using defaults\n", path);
		return;
	}
	char line[256];
	while (fgets(line, sizeof line, f)) {
		trim(line);
		if (line[0] == '#' || !line[0])
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		if (!strcmp(k, "mqtt_host"))
			snprintf(C.host, 128, "%s", v);
		else if (!strcmp(k, "mqtt_port"))
			C.port = atoi(v);
		else if (!strcmp(k, "mqtt_user"))
			snprintf(C.user, 64, "%s", v);
		else if (!strcmp(k, "mqtt_pass"))
			snprintf(C.pass, 64, "%s", v);
		else if (!strcmp(k, "base_topic"))
			snprintf(C.base, 64, "%s", v);
		else if (!strcmp(k, "disco_prefix"))
			snprintf(C.disco, 64, "%s", v);
		else if (!strcmp(k, "serial"))
			snprintf(C.serial, 64, "%s", v);
		else if (!strcmp(k, "device_name"))
			snprintf(C.devname, 64, "%s", v);
		else if (!strcmp(k, "uid"))
			snprintf(C.uid, 64, "%s", v);
		else if (!strcmp(k, "poll"))
			C.poll = atoi(v);
		else if (!strcmp(k, "num_outlets"))
			C.nout = atoi(v);
		else if (!strcmp(k, "volt_scale"))
			C.vscale = atof(v);
		else if (!strcmp(k, "curr_scale"))
			C.iscale = atof(v);
		else if (!strcmp(k, "state_file"))
			snprintf(C.statefile, 96, "%s", v);
		else if (!strcmp(k, "power_on_delay"))
			C.pondelay = atoi(v);
		else if (!strcmp(k, "reset_delay"))
			C.reset_delay = atoi(v);
		else if (!strncmp(k, "outlet", 6) && strstr(k, "_power_on")) {
			int i = atoi(k + 6) - 1;
			if (i >= 0 && i < MAXOUT) {
				if (!strcmp(v, "on") || !strcmp(v, "off") ||
				    !strcmp(v, "last"))
					snprintf(C.pon[i], 8, "%s", v);
			}
		} else if (!strncmp(k, "outlet", 6) && strstr(k, "_name")) {
			int i = atoi(k + 6) - 1;
			if (i >= 0 && i < MAXOUT)
				snprintf(C.oname[i], 64, "%s", v);
		} else if (!strncmp(k, "led_", 4) && strstr(k, "_name")) {
			for (int i = 0; i < NLED; i++) {
				char kk[32];
				snprintf(kk, 32, "led_%s_name", LED_KEY[i]);
				if (!strcmp(k, kk))
					snprintf(C.led_name[i], 32, "%s", v);
			}
		}
	}
	fclose(f);
	if (C.nout < 1)
		C.nout = 1;
	if (C.nout > MAXOUT)
		C.nout = MAXOUT;
}

/* ---- persistent outlet state (mirrors vendor NVRAM outlet%d_status) ---- */
/* Saved as one digit per outlet (outlet 1 leftmost), e.g. "10010\n". Lives in
 * the jffs2 overlay so it survives warm/cold boot and a kernel+squashfs reflash
 * (a firstboot/factory-reset intentionally clears it). */
static void save_state(void)
{
	if (!C.statefile[0])
		return;
	char tmp[112];
	snprintf(tmp, sizeof tmp, "%s.tmp", C.statefile);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "save_state: open %s: %s\n", tmp,
			strerror(errno));
		return;
	}
	for (int i = 0; i < C.nout; i++)
		fputc(outlet_state[i] ? '1' : '0', f);
	fputc('\n', f);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	if (rename(tmp, C.statefile) < 0)
		fprintf(stderr, "save_state: rename: %s\n", strerror(errno));
}
/* Fill saved[0..nout-1] from the state file. Returns digits read, or -1 if none. */
static int load_saved(int *saved)
{
	FILE *f = fopen(C.statefile, "r");
	if (!f)
		return -1;
	int n = 0, c;
	while (n < C.nout && (c = fgetc(f)) != EOF) {
		if (c == '0' || c == '1')
			saved[n++] = (c == '1');
	}
	fclose(f);
	return n > 0 ? n : -1;
}

/* ---- mqtt publish helpers ---- */
static void pub(const char *topic, const char *payload, int retain)
{
	mosquitto_publish(mq, NULL, topic, payload ? (int)strlen(payload) : 0,
			  payload, 0, retain);
}
static void dev_json(char *b, int sz)
{
	snprintf(
		b, sz,
		"{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"SnapAV\",\"mdl\":\"WB-300VB-IP-5\"}",
		C.uid, C.devname);
}
static void publish_discovery(void)
{
	char t[256], p[768], dev[256];
	dev_json(dev, sizeof dev);
	for (int i = 0; i < C.nout; i++) {
		snprintf(t, sizeof t, "%s/switch/%s_outlet%d/config", C.disco,
			 C.uid, i + 1);
		snprintf(
			p, sizeof p,
			"{\"name\":\"%s\",\"uniq_id\":\"%s_outlet%d\",\"cmd_t\":\"%s/outlet/%d/set\","
			"\"stat_t\":\"%s/outlet/%d/state\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
			"\"avty_t\":\"%s/status\",\"dev\":%s}",
			C.oname[i], C.uid, i + 1, C.base, i + 1, C.base, i + 1,
			C.base, dev);
		pub(t, p, 1);
		/* per-outlet Reset button (power-cycle: off -> delay -> on) */
		snprintf(t, sizeof t, "%s/button/%s_outlet%d_reset/config",
			 C.disco, C.uid, i + 1);
		snprintf(
			p, sizeof p,
			"{\"name\":\"%s Reset\",\"uniq_id\":\"%s_outlet%d_reset\",\"cmd_t\":\"%s/outlet/%d/reset\","
			"\"avty_t\":\"%s/status\",\"dev\":%s}",
			C.oname[i], C.uid, i + 1, C.base, i + 1, C.base, dev);
		pub(t, p, 1);
	}
	snprintf(t, sizeof t, "%s/sensor/%s_voltage/config", C.disco, C.uid);
	snprintf(
		p, sizeof p,
		"{\"name\":\"Voltage\",\"uniq_id\":\"%s_voltage\",\"stat_t\":\"%s/voltage\","
		"\"unit_of_meas\":\"V\",\"dev_cla\":\"voltage\",\"stat_cla\":\"measurement\",\"avty_t\":\"%s/status\",\"dev\":%s}",
		C.uid, C.base, C.base, dev);
	pub(t, p, 1);
	snprintf(t, sizeof t, "%s/sensor/%s_current/config", C.disco, C.uid);
	snprintf(
		p, sizeof p,
		"{\"name\":\"Current\",\"uniq_id\":\"%s_current\",\"stat_t\":\"%s/current\","
		"\"unit_of_meas\":\"A\",\"dev_cla\":\"current\",\"stat_cla\":\"measurement\",\"avty_t\":\"%s/status\",\"dev\":%s}",
		C.uid, C.base, C.base, dev);
	pub(t, p, 1);
	snprintf(t, sizeof t, "%s/sensor/%s_power/config", C.disco, C.uid);
	snprintf(
		p, sizeof p,
		"{\"name\":\"Power\",\"uniq_id\":\"%s_power\",\"stat_t\":\"%s/power\","
		"\"unit_of_meas\":\"W\",\"dev_cla\":\"power\",\"stat_cla\":\"measurement\",\"avty_t\":\"%s/status\",\"dev\":%s}",
		C.uid, C.base, C.base, dev);
	pub(t, p, 1);
	for (int i = 0; i < NLED; i++) {
		snprintf(t, sizeof t, "%s/select/%s_led_%s/config", C.disco,
			 C.uid, LED_KEY[i]);
		snprintf(
			p, sizeof p,
			"{\"name\":\"%s\",\"uniq_id\":\"%s_led_%s\",\"cmd_t\":\"%s/led/%s/set\","
			"\"stat_t\":\"%s/led/%s/state\",\"options\":[\"auto\",\"off\",\"green\",\"red\",\"amber\"],"
			"\"avty_t\":\"%s/status\",\"dev\":%s}",
			C.led_name[i], C.uid, LED_KEY[i], C.base, LED_KEY[i],
			C.base, LED_KEY[i], C.base, dev);
		pub(t, p, 1);
	}
}
static void publish_outlet(int i)
{
	char t[96];
	snprintf(t, sizeof t, "%s/outlet/%d/state", C.base, i + 1);
	pub(t, outlet_state[i] ? "ON" : "OFF", 1);
}
static void publish_led(int i)
{
	char t[96];
	snprintf(t, sizeof t, "%s/led/%s/state", C.base, LED_KEY[i]);
	pub(t, led_mode[i] ? LCOLOR[led_color[i] & 3] : "auto", 1);
}

/* ---- actions ---- */
static void set_outlet(int n, int on)
{ /* n = 1..nout */
	if (n < 1 || n > C.nout)
		return;
	char cmd[16];
	snprintf(cmd, sizeof cmd, "T%02d%c", n, on ? 'Y' : 'N');
	pic_cmd(cmd, NULL, 0);
	outlet_state[n - 1] = on ? 1 : 0;
	publish_outlet(n - 1);
	save_state();
}
/* Power-cycle an outlet: switch off now, schedule on after reset_delay (done
 * in the main loop so we never block the mqtt callback thread). */
static void reset_outlet(int n)
{
	if (n < 1 || n > C.nout)
		return;
	set_outlet(n, 0);
	reset_at[n - 1] = time(NULL) + (C.reset_delay > 0 ? C.reset_delay : 1);
}

/* Apply each outlet's power-on state at startup by driving the PIC (the PIC
 * itself does not persist relay state across a reset -- stock firmware restores
 * host-side too). mode "last" = the saved state; "on"/"off" = forced. Runs
 * before MQTT so it does not block on the broker; MQTT state is published by
 * on_connect from outlet_state[] afterwards. */
static void restore_outlets(void)
{
	int saved[MAXOUT];
	int have = load_saved(saved);
	int applied = 0;
	for (int i = 0; i < C.nout; i++) {
		int desired;
		if (!strcmp(C.pon[i], "on"))
			desired = 1;
		else if (!strcmp(C.pon[i], "off"))
			desired = 0;
		else { /* last */
			if (have > i)
				desired = saved[i];
			else
				desired = 0;
		}
		char cmd[16];
		snprintf(cmd, sizeof cmd, "T%02d%c", i + 1,
			 desired ? 'Y' : 'N');
		pic_cmd(cmd, NULL, 0);
		outlet_state[i] = desired;
		fprintf(stderr, "restore outlet %d -> %s (mode=%s)\n", i + 1,
			desired ? "ON" : "OFF", C.pon[i]);
		if (desired)
			applied++;
		if (C.pondelay > 0 && i < C.nout - 1)
			usleep((useconds_t)C.pondelay * 1000);
	}
	save_state();
	fprintf(stderr, "restore_outlets: %d/%d on (saved=%s)\n", applied,
		C.nout, have > 0 ? "yes" : "none");
}
/* Status-driven colour for LED i when it is in auto mode. Makes the panel
 * meaningful with no HA attached. */
static int auto_color(int i)
{
	if (i == 0) { /* Internet = controller/MQTT link */
		return mqtt_up ? 1 : 2; /* green when up, red when down */
	}
	if (i == 1) { /* System = wbctld/PIC health */
		if (!pic_ok)
			return 2; /* solid red: PIC not responding */
		if (!ever_up)
			return blink_on ? 1 :
					  0; /* flashing green: starting up */
		if (!mqtt_up)
			return blink_on ? 3 : 0; /* flashing amber: link lost */
		return 1; /* solid green: healthy */
	}
	return 0; /* Auto-Reboot: off in auto */
}
/* Build the 6-digit $L pattern from each LED's effective colour, and send it
 * only when it changed (avoids constant 2400-baud chatter for steady states). */
static void apply_leds(void)
{
	char p[7] = "000000";
	for (int i = 0; i < NLED; i++) {
		int c = led_mode[i] ? led_color[i] : auto_color(i);
		if (c == 2 || c == 3)
			p[LED_RED[i]] = '1'; /* red or amber */
		if (c == 1 || c == 3)
			p[LED_GRN[i]] = '1'; /* green or amber */
	}
	if (strcmp(p, led_last)) {
		char cmd[16];
		snprintf(cmd, sizeof cmd, "L%s", p);
		pic_cmd(cmd, NULL, 0);
		strcpy(led_last, p);
	}
}
/* color 0-3 pins a manual colour; color <0 returns the LED to auto mode. */
static void set_led(int i, int color)
{
	if (i < 0 || i >= NLED)
		return;
	if (color < 0)
		led_mode[i] = 0;
	else {
		led_mode[i] = 1;
		led_color[i] = color & 3;
	}
	apply_leds();
	publish_led(i);
}

/* ---- mqtt callbacks ---- */
static void on_connect(struct mosquitto *m, void *u, int rc)
{
	(void)u;
	(void)rc;
	char t[96];
	mqtt_up = 1;
	ever_up =
		1; /* flags only; main loop drives the serial $L (never from this thread) */
	snprintf(t, sizeof t, "%s/status", C.base);
	pub(t, "online", 1);
	snprintf(t, sizeof t, "%s/outlet/+/set", C.base);
	mosquitto_subscribe(m, NULL, t, 0);
	snprintf(t, sizeof t, "%s/outlet/+/reset", C.base);
	mosquitto_subscribe(m, NULL, t, 0);
	snprintf(t, sizeof t, "%s/led/+/set", C.base);
	mosquitto_subscribe(m, NULL, t, 0);
	publish_discovery();
	for (int i = 0; i < C.nout; i++)
		publish_outlet(i);
	for (int i = 0; i < NLED; i++)
		publish_led(i);
}
static void on_disconnect(struct mosquitto *m, void *u, int rc)
{
	(void)m;
	(void)u;
	(void)rc;
	mqtt_up = 0; /* flag only; main loop applies the LED change */
}
static void on_message(struct mosquitto *m, void *u,
		       const struct mosquitto_message *msg)
{
	(void)m;
	(void)u;
	char pl[16] = { 0 };
	int n = msg->payloadlen < 15 ? msg->payloadlen : 15;
	memcpy(pl, msg->payload, n);
	int on = (!strcasecmp(pl, "ON") || !strcmp(pl, "1") ||
		  !strcasecmp(pl, "true"));
	/* topic: base/outlet/N/set  or  base/led/<key>/set */
	char base_outlet[96], base_led[96];
	snprintf(base_outlet, sizeof base_outlet, "%s/outlet/", C.base);
	snprintf(base_led, sizeof base_led, "%s/led/", C.base);
	if (!strncmp(msg->topic, base_outlet, strlen(base_outlet))) {
		int idx = atoi(msg->topic + strlen(base_outlet));
		if (strstr(msg->topic, "/reset"))
			reset_outlet(idx); /* button press */
		else
			set_outlet(idx, on);
	} else if (!strncmp(msg->topic, base_led, strlen(base_led))) {
		const char *k = msg->topic + strlen(base_led);
		int color = -1; /* default/unknown -> auto */
		for (int c = 0; c < 4; c++)
			if (!strcasecmp(pl, LCOLOR[c]))
				color = c;
		if (!strcasecmp(pl, "on"))
			color = 1; /* ON -> green convenience */
		if (!strcasecmp(pl, "auto"))
			color = -1; /* back to status-driven */
		for (int i = 0; i < NLED; i++) {
			if (!strncmp(k, LED_KEY[i], strlen(LED_KEY[i]))) {
				set_led(i, color);
				break;
			}
		}
	}
}

/* ---- metering poll ---- */
static void poll_meter(void)
{
	char r[64], t[96], b[32];
	double v = -1, a = -1;
	if (pic_cmd("V", r, sizeof r) > 0) {
		v = atoi(r) * C.vscale;
		snprintf(b, 32, "%.1f", v);
		snprintf(t, sizeof t, "%s/voltage", C.base);
		pub(t, b, 0);
	}
	if (pic_cmd("I", r, sizeof r) > 0) {
		a = atoi(r) * C.iscale;
		snprintf(b, 32, "%.1f", a);
		snprintf(t, sizeof t, "%s/current", C.base);
		pub(t, b, 0);
	}
	/* Apparent power V*I -- the PIC has no power register ($P is unknown), and
	 * there is no power-factor sense, so this matches what stock fw reported. */
	if (v >= 0 && a >= 0) {
		snprintf(b, 32, "%.1f", v * a);
		snprintf(t, sizeof t, "%s/power", C.base);
		pub(t, b, 0);
	}
}

/* ---- outlet status readback ----
 * '$Q' returns the real per-outlet state: reply data is one digit per outlet,
 * outlet 1 leftmost ('1'=on, '0'=off). Confirmed live (toggling outlet 1 ->
 * "10000", outlet 5 -> "00001"). This is the authoritative state, unlike '$S'
 * (a set/echo buffer). Publishing from here keeps HA correct even when an
 * outlet changes outside this daemon (schedule, power button, reboot). */
static void poll_outlets(void)
{
	char r[64];
	if (pic_cmd("Q", r, sizeof r) <= 0) {
		if (pic_ok) {
			pic_ok = 0;
			apply_leds();
		}
		return;
	}
	/* validate: expect at least nout leading '0'/'1' digits */
	for (int i = 0; i < C.nout; i++)
		if (r[i] != '0' && r[i] != '1') {
			if (pic_ok) {
				pic_ok = 0;
				apply_leds();
			}
			return;
		}
	if (!pic_ok) {
		pic_ok = 1;
		apply_leds();
	} /* PIC recovered */
	int changed = 0;
	for (int i = 0; i < C.nout; i++) {
		int on = (r[i] == '1');
		if (on != outlet_state[i]) {
			outlet_state[i] = on;
			changed = 1;
		}
		/* Publish every cycle (not only on change): a set-time publish from the
         * mqtt callback thread can be dropped, leaving HA's retained state stale
         * until the next change. Re-asserting the authoritative $Q readback each
         * poll self-corrects that within one interval. */
		publish_outlet(i);
	}
	if (changed)
		save_state(); /* keep "last" tracking external changes */
}

int main(int argc, char **argv)
{
	const char *cfgpath = (argc > 1) ? argv[1] : "/var/etc/wbctl.conf";
	load_cfg(cfgpath);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	serfd = ser_open(C.serial);
	if (serfd < 0) {
		fprintf(stderr, "cannot open serial %s\n", C.serial);
	}
	/* model probe (best-effort) */
	char model[64];
	if (pic_cmd("M", model, sizeof model) > 0)
		fprintf(stderr, "PIC model: %s\n", model);
	if (serfd >= 0) {
		restore_outlets();
		apply_leds();
	} /* power-on outlets + LEDs off, before MQTT */

	mosquitto_lib_init();
	/* MQTT client-id must be UNIQUE per device (distinct from the HA uniq_id
	 * prefix C.uid): two clients sharing an id fight over the broker via
	 * takeover, flapping the connection. Derive a stable-unique id from the
	 * SoC serial (same source as the MAC); fall back to pid. */
	char cid[96], ser[64] = "";
	{
		FILE *cf = fopen("/proc/cpuinfo", "r");
		if (cf) {
			char ln[160];
			while (fgets(ln, sizeof ln, cf)) {
				if (!strncmp(ln, "Serial", 6)) {
					char *c = strchr(ln, ':');
					if (c)
						sscanf(c + 1, "%63s", ser);
					break;
				}
			}
			fclose(cf);
		}
	}
	if (ser[0])
		snprintf(cid, sizeof cid, "%s-%s", C.uid, ser);
	else
		snprintf(cid, sizeof cid, "%s-%d", C.uid, (int)getpid());
	fprintf(stderr, "mqtt client-id: %s\n", cid);
	mq = mosquitto_new(cid, true, NULL);
	if (C.user[0])
		mosquitto_username_pw_set(mq, C.user,
					  C.pass[0] ? C.pass : NULL);
	char st[96];
	snprintf(st, sizeof st, "%s/status", C.base);
	mosquitto_will_set(mq, st, 7, "offline", 0, 1); /* LWT */
	mosquitto_connect_callback_set(mq, on_connect);
	mosquitto_disconnect_callback_set(mq, on_disconnect);
	mosquitto_message_callback_set(mq, on_message);
	while (running &&
	       mosquitto_connect(mq, C.host, C.port, 60) != MOSQ_ERR_SUCCESS) {
		fprintf(stderr, "mqtt connect %s:%d failed, retry in 5s\n",
			C.host, C.port);
		sleep(5);
	}
	mosquitto_loop_start(mq);

	time_t last = 0;
	int bt = 0;
	while (running) {
		time_t now = time(NULL);
		for (int i = 0; i < C.nout;
		     i++) /* complete scheduled power-cycles */
			if (reset_at[i] && now >= reset_at[i]) {
				reset_at[i] = 0;
				set_outlet(i + 1, 1);
			}
		if (now - last >= C.poll) {
			last = now;
			poll_outlets();
			poll_meter();
			for (int i = 0; i < NLED; i++)
				publish_led(i); /* re-assert LED select state */
		}
		/* ~600ms blink tick: drives flashing auto states; apply_leds only
         * writes $L when the pattern actually changes. */
		if (++bt >= 3) {
			bt = 0;
			blink_on = !blink_on;
			apply_leds();
		}
		usleep(200000);
	}
	snprintf(st, sizeof st, "%s/status", C.base);
	pub(st, "offline", 1);
	mosquitto_loop_stop(mq, true);
	mosquitto_disconnect(mq);
	mosquitto_destroy(mq);
	mosquitto_lib_cleanup();
	if (serfd >= 0)
		close(serfd);
	return 0;
}
