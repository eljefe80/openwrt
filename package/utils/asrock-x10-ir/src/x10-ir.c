// SPDX-License-Identifier: GPL-2.0-only
/*
 * x10-ir - drive the IR blaster MCU (Sonix SN8F27E93L) of the ASRock X10
 *
 * Clean-room reimplementation of the stock /sbin/test_ir protocol:
 *
 *  - I2C slave 0x50 on the bit-banged i2c-gpio bus, plain read()/write()
 *    transactions, no register phase.
 *  - Host frame:  FF 61 00 LenLo LenHi CMD DATA[N] CS F0
 *                 Len = N + 2, CS = low byte of sum(00, Len, CMD, DATA)
 *  - MCU frames are read as 8-byte status frames (code in byte 5) or as
 *    data frames (payload length = Len - 2, payload at byte 6).
 *  - Before every read the MCU signals ready by driving gpio60 high.
 *  - gpio46 is the MCU's active-low reset.
 *
 * Learned codes are printed as space separated hex bytes, the same format
 * the stock tool kept in /tmp/irdatax, and "send" accepts that format.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>

#define IR_I2C_ADDR		0x50
#define IR_GPIOCHIP_LABEL	"1000000.pinctrl"
#define IR_GPIO_READY		60
#define IR_GPIO_RESET		46
#define IR_LOCK_FILE		"/var/lock/x10-ir.lock"

#define CMD_SEND		0x02
#define CMD_VERSION		0x10
#define CMD_LED			0x12
#define CMD_LEARN		0x30

#define ST_ACK			0x7f
#define ST_SEND_DONE		0x81

#define STATUS_LEN		8
#define VERSION_READ_LEN	24
#define LEARN_READ_LEN		512
#define MAX_PAYLOAD		(LEARN_READ_LEN - 8)

static int verbose;
static int ready_timeout_ms = 2000;
static int learn_timeout_ms = 30000;

static void dump(const char *tag, const uint8_t *buf, size_t len)
{
	size_t i;

	if (!verbose)
		return;

	fprintf(stderr, "%s", tag);
	for (i = 0; i < len; i++)
		fprintf(stderr, " %02X", buf[i]);
	fprintf(stderr, "\n");
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* GPIO, via the character device uAPI */

static int gpiochip_open(void)
{
	struct gpiochip_info info;
	char path[32];
	int i, fd;

	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/gpiochip%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (!ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) &&
		    !strcmp(info.label, IR_GPIOCHIP_LABEL))
			return fd;

		close(fd);
	}

	fprintf(stderr, "x10-ir: gpiochip %s not found\n", IR_GPIOCHIP_LABEL);
	return -1;
}

static int gpio_request(unsigned int line, uint64_t flags)
{
	struct gpio_v2_line_request req;
	int chip, ret;

	chip = gpiochip_open();
	if (chip < 0)
		return -1;

	memset(&req, 0, sizeof(req));
	req.offsets[0] = line;
	req.num_lines = 1;
	req.config.flags = flags;
	strncpy(req.consumer, "x10-ir", sizeof(req.consumer) - 1);

	ret = ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req);
	close(chip);
	if (ret < 0) {
		fprintf(stderr, "x10-ir: requesting gpio%u: %s\n",
			line, strerror(errno));
		return -1;
	}

	return req.fd;
}

static int gpio_get(int fd)
{
	struct gpio_v2_line_values v = { .mask = 1 };

	if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0)
		return -1;

	return v.bits & 1;
}

static int gpio_set(int fd, int value)
{
	struct gpio_v2_line_values v = { .mask = 1, .bits = value ? 1 : 0 };

	return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

static int mcu_reset(void)
{
	int fd;

	fd = gpio_request(IR_GPIO_RESET, GPIO_V2_LINE_FLAG_OUTPUT);
	if (fd < 0)
		return -1;

	gpio_set(fd, 0);
	sleep_ms(200);
	gpio_set(fd, 1);
	sleep_ms(200);
	close(fd);

	/* stock firmware leaves the reset line as an input afterwards */
	fd = gpio_request(IR_GPIO_RESET, GPIO_V2_LINE_FLAG_INPUT);
	if (fd >= 0)
		close(fd);

	return 0;
}

static int wait_ready(int ready_fd, int timeout_ms)
{
	long deadline = now_ms() + timeout_ms;
	int v;

	for (;;) {
		v = gpio_get(ready_fd);
		if (v < 0) {
			fprintf(stderr, "x10-ir: reading ready line: %s\n",
				strerror(errno));
			return -1;
		}
		if (v)
			return 0;
		if (now_ms() >= deadline) {
			fprintf(stderr, "x10-ir: MCU not ready after %d ms\n",
				timeout_ms);
			return -1;
		}
		sleep_ms(1);
	}
}

/* I2C transport */

static int i2c_find_bus(void)
{
	char path[300], name[64];
	struct dirent *de;
	int bus = -1;
	DIR *dir;
	FILE *f;

	dir = opendir("/sys/bus/i2c/devices");
	if (!dir)
		return -1;

	while ((de = readdir(dir))) {
		if (strncmp(de->d_name, "i2c-", 4))
			continue;

		snprintf(path, sizeof(path), "/sys/bus/i2c/devices/%s/name",
			 de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) &&
		    !strncmp(name, "i2c-gpio", 8))
			bus = atoi(de->d_name + 4);
		fclose(f);

		if (bus >= 0)
			break;
	}
	closedir(dir);

	return bus;
}

static int i2c_open(int bus)
{
	char path[32];
	int fd;

	if (bus < 0)
		bus = i2c_find_bus();
	if (bus < 0) {
		fprintf(stderr, "x10-ir: i2c-gpio bus not found\n");
		return -1;
	}

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "x10-ir: %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (ioctl(fd, I2C_SLAVE, IR_I2C_ADDR) < 0) {
		fprintf(stderr, "x10-ir: I2C_SLAVE 0x%02x: %s\n",
			IR_I2C_ADDR, strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

static int send_frame(int i2c, uint8_t cmd, const uint8_t *data, size_t n)
{
	uint8_t frame[MAX_PAYLOAD + 8];
	unsigned int sum = 0;
	size_t i, len = n + 2;

	if (n > MAX_PAYLOAD) {
		fprintf(stderr, "x10-ir: payload of %zu bytes too long\n", n);
		return -1;
	}

	frame[0] = 0xff;
	frame[1] = 0x61;
	frame[2] = 0x00;
	frame[3] = len & 0xff;
	frame[4] = len >> 8;
	frame[5] = cmd;
	if (n)
		memcpy(&frame[6], data, n);
	for (i = 2; i < 6 + n; i++)
		sum += frame[i];
	frame[6 + n] = sum & 0xff;
	frame[7 + n] = 0xf0;

	dump("tx:", frame, n + 8);

	if (write(i2c, frame, n + 8) == (ssize_t)(n + 8))
		return 0;

	/* like the stock tool: reset the MCU once and retry */
	if (mcu_reset() == 0 && write(i2c, frame, n + 8) == (ssize_t)(n + 8))
		return 0;

	fprintf(stderr, "x10-ir: I2C write failed: %s\n", strerror(errno));
	return -1;
}

static int read_frame(int i2c, int ready_fd, int timeout_ms,
		      uint8_t *buf, size_t len)
{
	if (wait_ready(ready_fd, timeout_ms))
		return -1;

	if (read(i2c, buf, len) != (ssize_t)len) {
		fprintf(stderr, "x10-ir: I2C read failed: %s\n",
			strerror(errno));
		return -1;
	}

	dump("rx:", buf, len);
	return 0;
}

static int expect_status(int i2c, int ready_fd, int timeout_ms, uint8_t code)
{
	uint8_t st[STATUS_LEN];

	if (read_frame(i2c, ready_fd, timeout_ms, st, sizeof(st)))
		return -1;

	if (st[5] != code) {
		fprintf(stderr, "x10-ir: expected status 0x%02x, got 0x%02x\n",
			code, st[5]);
		return -1;
	}

	return 0;
}

/* returns the payload length, or -1 */
static int read_data(int i2c, int ready_fd, int timeout_ms,
		     uint8_t *buf, size_t len)
{
	int plen;

	if (expect_status(i2c, ready_fd, ready_timeout_ms, ST_ACK))
		return -1;

	if (read_frame(i2c, ready_fd, timeout_ms, buf, len))
		return -1;

	plen = ((buf[4] << 8) | buf[3]) - 2;
	if (plen < 0 || (size_t)plen > len - 6) {
		fprintf(stderr, "x10-ir: bad data frame length %d\n", plen);
		return -1;
	}

	return plen;
}

/* commands */

static int cmd_version(int i2c, int ready_fd)
{
	uint8_t buf[VERSION_READ_LEN];
	int plen;

	if (send_frame(i2c, CMD_VERSION, NULL, 0))
		return -1;

	plen = read_data(i2c, ready_fd, ready_timeout_ms, buf, sizeof(buf));
	if (plen < 0)
		return -1;

	printf("%.*s\n", plen, (char *)&buf[6]);
	return 0;
}

static int parse_hex_byte(const char *s, uint8_t *out)
{
	unsigned long v;
	char *end;

	errno = 0;
	v = strtoul(s, &end, 16);
	if (errno || end == s || *end || v > 0xff) {
		fprintf(stderr, "x10-ir: invalid hex byte '%s'\n", s);
		return -1;
	}

	*out = v;
	return 0;
}

static int cmd_led(int i2c, int ready_fd, int argc, char **argv)
{
	uint8_t data[16];
	int i;

	if (argc < 1 || argc > (int)sizeof(data)) {
		fprintf(stderr, "x10-ir: led needs 1-%zu hex bytes\n",
			sizeof(data));
		return -1;
	}

	for (i = 0; i < argc; i++)
		if (parse_hex_byte(argv[i], &data[i]))
			return -1;

	if (send_frame(i2c, CMD_LED, data, argc))
		return -1;

	return expect_status(i2c, ready_fd, ready_timeout_ms, ST_ACK);
}

static int cmd_learn(int i2c, int ready_fd)
{
	uint8_t buf[LEARN_READ_LEN];
	int plen, i;

	if (send_frame(i2c, CMD_LEARN, NULL, 0))
		return -1;

	fprintf(stderr, "x10-ir: waiting up to %d ms for an IR code...\n",
		learn_timeout_ms);

	plen = read_data(i2c, ready_fd, learn_timeout_ms, buf, sizeof(buf));
	if (plen < 0)
		return -1;
	if (plen == 0) {
		fprintf(stderr, "x10-ir: no IR code captured\n");
		return -1;
	}

	for (i = 0; i < plen; i++)
		printf(i ? " %02X" : "%02X", buf[6 + i]);
	printf("\n");

	return 0;
}

static int cmd_send(int i2c, int ready_fd, int argc, char **argv)
{
	uint8_t data[MAX_PAYLOAD];
	char line[1024], *tok, *save;
	size_t n = 0;
	FILE *f = stdin;
	int ret = -1;

	if (argc > 0 && strcmp(argv[0], "-")) {
		f = fopen(argv[0], "r");
		if (!f) {
			fprintf(stderr, "x10-ir: %s: %s\n",
				argv[0], strerror(errno));
			return -1;
		}
	}

	while (fgets(line, sizeof(line), f)) {
		for (tok = strtok_r(line, " \t\r\n", &save); tok;
		     tok = strtok_r(NULL, " \t\r\n", &save)) {
			if (n >= sizeof(data)) {
				fprintf(stderr, "x10-ir: code longer than %zu bytes\n",
					sizeof(data));
				goto out;
			}
			if (parse_hex_byte(tok, &data[n++]))
				goto out;
		}
	}

	if (!n) {
		fprintf(stderr, "x10-ir: empty IR code\n");
		goto out;
	}

	if (send_frame(i2c, CMD_SEND, data, n) ||
	    expect_status(i2c, ready_fd, ready_timeout_ms, ST_ACK) ||
	    expect_status(i2c, ready_fd, ready_timeout_ms, ST_SEND_DONE))
		goto out;

	ret = 0;
out:
	if (f != stdin)
		fclose(f);
	return ret;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: x10-ir [-v] [-b bus] [-t ms] [-T ms] <command>\n"
		"\n"
		"commands:\n"
		"  version            print the MCU firmware version\n"
		"  led <hex>...       set the MCU-driven LED (stock: 00 <red> <blue>)\n"
		"  learn              capture a code, print it as hex bytes\n"
		"  send [file|-]      transmit a code printed by learn\n"
		"  reset              pulse the MCU reset line\n"
		"\n"
		"  -b bus   I2C bus number (default: the i2c-gpio adapter)\n"
		"  -t ms    ready-line timeout (default %d)\n"
		"  -T ms    learn timeout (default %d)\n"
		"  -v       dump I2C frames to stderr\n",
		ready_timeout_ms, learn_timeout_ms);
}

int main(int argc, char **argv)
{
	int bus = -1, i2c, ready_fd, lock_fd, ret, opt;
	const char *cmd;

	while ((opt = getopt(argc, argv, "b:t:T:vh")) != -1) {
		switch (opt) {
		case 'b':
			bus = atoi(optarg);
			break;
		case 't':
			ready_timeout_ms = atoi(optarg);
			break;
		case 'T':
			learn_timeout_ms = atoi(optarg);
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
			return opt == 'h' ? 0 : 2;
		}
	}

	if (optind >= argc) {
		usage();
		return 2;
	}

	cmd = argv[optind++];
	argc -= optind;
	argv += optind;

	/* one transaction at a time, as the stock ir_cmd wrapper did */
	lock_fd = open(IR_LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock_fd < 0 || flock(lock_fd, LOCK_EX) < 0) {
		fprintf(stderr, "x10-ir: locking %s: %s\n",
			IR_LOCK_FILE, strerror(errno));
		return 1;
	}

	if (!strcmp(cmd, "reset"))
		return mcu_reset() ? 1 : 0;

	i2c = i2c_open(bus);
	if (i2c < 0)
		return 1;

	ready_fd = gpio_request(IR_GPIO_READY, GPIO_V2_LINE_FLAG_INPUT);
	if (ready_fd < 0) {
		close(i2c);
		return 1;
	}

	if (!strcmp(cmd, "version")) {
		ret = cmd_version(i2c, ready_fd);
	} else if (!strcmp(cmd, "led")) {
		ret = cmd_led(i2c, ready_fd, argc, argv);
	} else if (!strcmp(cmd, "learn")) {
		ret = cmd_learn(i2c, ready_fd);
	} else if (!strcmp(cmd, "send")) {
		ret = cmd_send(i2c, ready_fd, argc, argv);
	} else {
		usage();
		ret = -1;
	}

	close(ready_fd);
	close(i2c);

	return ret ? 1 : 0;
}
