// SPDX-License-Identifier: GPL-2.0-only
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;

static void stop_capture(int sig)
{
	(void)sig;
	stopped = 1;
}

static int64_t monotonic_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
		perror("clock_gettime");
		exit(1);
	}
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	struct termios saved, config;
	struct pollfd pfd;
	struct sigaction sa = { .sa_handler = stop_capture };
	int fd, out, seconds, baud, ret = 0;
	speed_t speed;
	int64_t start, deadline;
	size_t total = 0;

	if (argc != 5) {
		fprintf(stderr, "usage: %s TTY BAUD SECONDS OUTPUT.raw\n", argv[0]);
		return 2;
	}
	baud = atoi(argv[2]);
	seconds = atoi(argv[3]);
	if (seconds < 1 || seconds > 300)
		return 2;
	switch (baud) {
	case 9600: speed = B9600; break;
	case 19200: speed = B19200; break;
	case 38400: speed = B38400; break;
	case 57600: speed = B57600; break;
	case 115200: speed = B115200; break;
	case 230400: speed = B230400; break;
	default: fprintf(stderr, "unsupported baud\n"); return 2;
	}
	fd = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) { perror("open tty"); return 1; }
	if (ioctl(fd, TIOCEXCL) < 0) { perror("exclusive tty"); close(fd); return 1; }
	if (tcgetattr(fd, &saved) < 0) { perror("tcgetattr"); close(fd); return 1; }
	out = open(argv[4], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (out < 0) { perror("open output"); close(fd); return 1; }
	config = saved;
	cfmakeraw(&config);
	config.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
	config.c_cflag |= CS8 | CREAD | CLOCAL;
	config.c_cc[VMIN] = 0;
	config.c_cc[VTIME] = 0;
	cfsetispeed(&config, speed);
	cfsetospeed(&config, speed);
	if (tcsetattr(fd, TCSANOW, &config) < 0) { perror("tcsetattr"); close(out); close(fd); return 1; }
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	pfd = (struct pollfd){ .fd = fd, .events = POLLIN };
	start = monotonic_ms();
	deadline = start + (int64_t)seconds * 1000;
	printf("CAPTURE tty=%s baud=%d seconds=%d output=%s (no UART transmit)\n", argv[1], baud, seconds, argv[4]);
	fflush(stdout);
	while (!stopped && monotonic_ms() < deadline) {
		unsigned char data[256];
		ssize_t n;
		int ready = poll(&pfd, 1, 200);
		if (ready < 0) { if (errno == EINTR) continue; perror("poll"); ret = 1; break; }
		if (!ready) continue;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { fprintf(stderr, "tty revents=%x\n", pfd.revents); ret = 1; break; }
		n = read(fd, data, sizeof(data));
		if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; perror("read"); ret = 1; break; }
		if (!n) continue;
		for (ssize_t done = 0; done < n;) {
			ssize_t written = write(out, data + done, (size_t)(n - done));
			if (written < 0 && errno == EINTR) continue;
			if (written <= 0) { perror("write output"); ret = 1; stopped = 1; break; }
			done += written;
		}
		total += (size_t)n;
		printf("%lldms %zd:", (long long)(monotonic_ms() - start), n);
		for (ssize_t i = 0; i < n; i++) printf(" %02x", data[i]);
		putchar('\n');
		fflush(stdout);
	}
	if (tcsetattr(fd, TCSANOW, &saved) < 0) { perror("restore termios"); ret = 1; }
	ioctl(fd, TIOCNXCL);
	close(out);
	close(fd);
	printf("CAPTURE_DONE bytes=%zu status=%d\n", total, ret);
	return ret;
}
