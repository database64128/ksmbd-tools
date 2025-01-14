// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 */

#include <syslog.h>

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stdio.h>
#include <ksmbdtools.h>

int log_level = PR_INFO;
int ksmbd_health_status;

static const char *app_name = "unknown";
static int log_open;

typedef void (*logger)(int level, const char *fmt, va_list list);

char *ksmbd_conv_charsets[KSMBD_CHARSET_MAX + 1] = {
	"UTF-8",
	"UTF-16LE",
	"UCS-2LE",
	"UTF-16BE",
	"UCS-2BE",
	"OOPS"
};

static int syslog_level(int level)
{
	if (level == PR_ERROR)
		return LOG_ERR;
	if (level == PR_INFO)
		return LOG_INFO;
	if (level == PR_DEBUG)
		return LOG_DEBUG;

	return LOG_ERR;
}

G_GNUC_PRINTF(2, 0)
static void __pr_log_stdio(int level, const char *fmt, va_list list)
{
	char buf[1024];

	vsnprintf(buf, sizeof(buf), fmt, list);
	printf("%s", buf);
}

G_GNUC_PRINTF(2, 0)
static void __pr_log_syslog(int level, const char *fmt, va_list list)
{
	vsyslog(syslog_level(level), fmt, list);
}

static logger __logger = __pr_log_stdio;

void set_logger_app_name(const char *an)
{
	app_name = an;
}

const char *get_logger_app_name(void)
{
	return app_name;
}

void __pr_log(int level, const char *fmt, ...)
{
	va_list list;

	va_start(list, fmt);
	__logger(level, fmt, list);
	va_end(list);
}

void pr_logger_init(int flag)
{
	if (flag == PR_LOGGER_SYSLOG) {
		if (log_open) {
			closelog();
			log_open = 0;
		}
		openlog("ksmbd", LOG_NDELAY, LOG_LOCAL5);
		__logger = __pr_log_syslog;
		log_open = 1;
	}
}

int set_log_level(int level)
{
	int old_level;

	if (log_level == PR_DEBUG)
		return log_level;

	old_level = log_level;
	log_level = level;
	return old_level;
}

#if TRACING_DUMP_NL_MSG
#define PR_HEX_DUMP_WIDTH	160
void pr_hex_dump(const void *mem, size_t sz)
{
	char xline[PR_HEX_DUMP_WIDTH];
	char sline[PR_HEX_DUMP_WIDTH];
	int xi = 0, si = 0, mi = 0;

	while (mi < sz) {
		char c = *((char *)mem + mi);

		mi++;
		xi += sprintf(xline + xi, "%02X ", 0xff & c);
		if (c > ' ' && c < '~')
			si += sprintf(sline + si, "%c", c);
		else
			si += sprintf(sline + si, ".");
		if (xi >= PR_HEX_DUMP_WIDTH / 2) {
			pr_err("%s         %s\n", xline, sline);
			xi = 0;
			si = 0;
		}
	}

	if (xi) {
		int sz = PR_HEX_DUMP_WIDTH / 2 - xi + 1;

		if (sz > 0) {
			memset(xline + xi, ' ', sz);
			xline[PR_HEX_DUMP_WIDTH / 2 + 1] = 0x00;
		}
		pr_err("%s         %s\n", xline, sline);
	}
}
#else
void pr_hex_dump(const void *mem, size_t sz)
{
}
#endif

char *base64_encode(unsigned char *src, size_t srclen)
{
	return g_base64_encode(src, srclen);
}

unsigned char *base64_decode(char const *src, size_t *dstlen)
{
	unsigned char *ret = g_base64_decode(src, dstlen);

	if (ret)
		ret[*dstlen] = 0x00;
	return ret;
}

static int codeset_has_altname(int codeset)
{
	if (codeset == KSMBD_CHARSET_UTF16LE ||
			codeset == KSMBD_CHARSET_UTF16BE)
		return 1;
	return 0;
}

gchar *ksmbd_gconvert(const gchar *str,
		      gssize       str_len,
		      int          to_codeset,
		      int          from_codeset,
		      gsize       *bytes_read,
		      gsize       *bytes_written)
{
	gchar *converted;
	GError *err;

retry:
	err = NULL;
	if (from_codeset >= KSMBD_CHARSET_MAX) {
		pr_err("Unknown source codeset: %d\n", from_codeset);
		return NULL;
	}

	if (to_codeset >= KSMBD_CHARSET_MAX) {
		pr_err("Unknown target codeset: %d\n", to_codeset);
		return NULL;
	}

	converted = g_convert(str,
			      str_len,
			      ksmbd_conv_charsets[to_codeset],
			      ksmbd_conv_charsets[from_codeset],
			      bytes_read,
			      bytes_written,
			      &err);
	if (err) {
		int has_altname = 0;

		if (codeset_has_altname(to_codeset)) {
			to_codeset++;
			has_altname = 1;
		}

		if (codeset_has_altname(from_codeset)) {
			from_codeset++;
			has_altname = 1;
		}

		pr_info("%s\n", err->message);
		g_error_free(err);

		if (has_altname) {
			pr_info("Will try '%s' and '%s'\n",
				ksmbd_conv_charsets[to_codeset],
				ksmbd_conv_charsets[from_codeset]);
			goto retry;
		}

		pr_err("Can't convert string: %s\n", err->message);
		g_error_free(err);
		return NULL;
	}

	return converted;
}

int send_signal_to_ksmbd_mountd(int signo)
{
	int fd, ret = -EINVAL;
	char pid_buf[10] = {0};
	int pid;

	fd = open(KSMBD_LOCK_FILE, O_RDONLY);
	if (fd < 0) {
		pr_err("Can't open `%s': %m\n", KSMBD_LOCK_FILE);
		return ret;
	}

	if (read(fd, &pid_buf, sizeof(pid_buf)) == -1) {
		pr_err("Unable to read manager PID: %m\n");
		goto out;
	}

	pid = strtol(pid_buf, NULL, 10);
	pr_debug("Send signal %d (%s) to PID %d\n",
		 signo, strsignal(signo), pid);
	if (kill(pid, signo) == -1) {
		ret = -errno;
		if (signo)
			pr_err("Unable to send signal %d (%s) to PID %d: %m\n",
					signo, strsignal(signo), pid);
		goto out;
	}

	ret = 0;
out:
	close(fd);
	return ret;
}

int test_file_access(char *conf)
{
	int fd;

	fd = open(conf, O_RDWR | O_CREAT, S_IRWXU | S_IRGRP);
	if (fd < 0) {
		pr_err("Can't open `%s': %m\n", conf);
		return -EINVAL;
	}

	close(fd);
	return 0;
}
