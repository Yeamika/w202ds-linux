// SPDX-License-Identifier: GPL-2.0-only
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/tty.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop_attach(int sig)
{
	(void)sig;
	stopped = 1;
}

int main(int argc, char **argv)
{
	struct termios saved, config;
	struct sigaction sa = { .sa_handler = stop_attach };
	int fd, saved_ldisc, ldisc = N_DEVELOPMENT, result = 0;

	if (argc != 2 || strcmp(argv[1], "/dev/ttyS2")) {
		fprintf(stderr, "usage: %s /dev/ttyS2\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }
	if (ioctl(fd, TIOCEXCL) < 0 || tcgetattr(fd, &saved) < 0 ||
	    ioctl(fd, TIOCGETD, &saved_ldisc) < 0) {
		perror("save/exclusive tty");
		close(fd);
		return 1;
	}
	if (saved_ldisc != N_TTY) {
		fprintf(stderr, "tty already uses line discipline %d\n", saved_ldisc);
		ioctl(fd, TIOCNXCL);
		close(fd);
		return 1;
	}
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	config = saved;
	cfmakeraw(&config);
	config.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
	config.c_cflag |= CS8 | CREAD | CLOCAL;
	config.c_cc[VMIN] = 1;
	config.c_cc[VTIME] = 0;
	cfsetispeed(&config, B115200);
	cfsetospeed(&config, B115200);
	if (tcsetattr(fd, TCSANOW, &config) < 0) {
		perror("set termios");
		ioctl(fd, TIOCNXCL);
		close(fd);
		return 1;
	}
	if (ioctl(fd, TIOCSETD, &ldisc) < 0) {
		perror("attach w202_zsk ldisc");
		tcsetattr(fd, TCSANOW, &saved);
		ioctl(fd, TIOCNXCL);
		close(fd);
		return 1;
	}
	printf("W202_ZSK_ATTACHED tty=%s baud=115200 ldisc=%d\n", argv[1], ldisc);
	fflush(stdout);
	while (!stopped) {
		struct timespec interval = { .tv_sec = 1 };
		nanosleep(&interval, NULL);
	}
	if (ioctl(fd, TIOCSETD, &saved_ldisc) < 0) { perror("restore ldisc"); result = 1; }
	if (tcsetattr(fd, TCSANOW, &saved) < 0) { perror("restore termios"); result = 1; }
	ioctl(fd, TIOCNXCL);
	close(fd);
	puts("W202_ZSK_DETACHED");
	return result;
}
