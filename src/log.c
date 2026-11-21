#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspintrman.h>
#include "log.h"

#ifndef BUILD_TIMESTAMP
#error "BUILD_TIMESTAMP must be defined by the Makefile"
#endif

#define LOG_RING_SIZE		(8 * 1024)
#define LOG_LINE_MAX		256
#define LOG_WRITER_PRIORITY	0x60
#define LOG_WRITER_STACK	(8 * 1024)
#define LOG_FLUSH_PERIOD_US	(100 * 1000)
#define LOG_STOP_TIMEOUT_US	(500 * 1000)
#define LOG_PATH_MAX		256
#define LOG_DRAIN_WAIT_US	(300 * 1000)
static char log_path[LOG_PATH_MAX];
static char ring[LOG_RING_SIZE];
static volatile unsigned int ring_head;	/* next write position (producers) */
static volatile unsigned int ring_tail;	/* next read position (writer)     */
static volatile unsigned int dropped;
static volatile int writer_run;
static SceUID writer_thid = -1;
static SceUID drain_sema = -1;	/* serialises ring_drain() between writer thread and log_flush() */

/*
 * Minimal formatter (snprintf semantics: always NUL-terminates, returns the
 * length the full output would have had). Supports %% %s %d %u %x %X %p with an
 * optional 0 flag and field width, which is all this plugin uses. It replaces
 * newlib's vsnprintf, which is a large part of the module's footprint in
 * kernel memory.
 */
struct fmt_out {
	char *buf;
	unsigned int size;
	unsigned int n;
};

static void fmt_putc(struct fmt_out *o, char c)
{
	if (o->n + 1 < o->size)
		o->buf[o->n] = c;
	o->n++;
}

static void fmt_number(struct fmt_out *o, unsigned int v, unsigned int base, int upper,
		       int negative, int zero_pad, unsigned int width)
{
	char digits[12];
	unsigned int nd = 0, total, pad;
	const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";

	do {
		digits[nd++] = set[v % base];
		v /= base;
	} while (v);

	total = nd + (negative ? 1 : 0);
	pad = width > total ? width - total : 0;

	if (negative && zero_pad)
		fmt_putc(o, '-');
	while (pad--)
		fmt_putc(o, zero_pad ? '0' : ' ');
	if (negative && !zero_pad)
		fmt_putc(o, '-');
	while (nd)
		fmt_putc(o, digits[--nd]);
}

static int log_vsnprintf(char *buf, unsigned int size, const char *fmt, va_list ap)
{
	struct fmt_out o = { buf, size, 0 };

	while (*fmt) {
		int zero_pad = 0;
		unsigned int width = 0;

		if (*fmt != '%') {
			fmt_putc(&o, *fmt++);
			continue;
		}
		fmt++;
		if (*fmt == '0') {
			zero_pad = 1;
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');

		switch (*fmt) {
		case 'd': {
			int v = va_arg(ap, int);
			fmt_number(&o, v < 0 ? 0u - (unsigned int)v : (unsigned int)v, 10, 0,
				   v < 0, zero_pad, width);
			break;
		}
		case 'u':
			fmt_number(&o, va_arg(ap, unsigned int), 10, 0, 0, zero_pad, width);
			break;
		case 'x':
		case 'X':
			fmt_number(&o, va_arg(ap, unsigned int), 16, *fmt == 'X', 0, zero_pad, width);
			break;
		case 'p':
			fmt_putc(&o, '0');
			fmt_putc(&o, 'x');
			fmt_number(&o, (unsigned int)(uintptr_t)va_arg(ap, void *), 16, 0, 0, 0, 0);
			break;
		case 's': {
			const char *str = va_arg(ap, const char *);
			if (!str)
				str = "(null)";
			while (*str)
				fmt_putc(&o, *str++);
			break;
		}
		case '%':
			fmt_putc(&o, '%');
			break;
		case '\0':
			goto done;
		default: /* unsupported: keep the text visible rather than guess */
			fmt_putc(&o, '%');
			fmt_putc(&o, *fmt);
			break;
		}
		fmt++;
	}
done:
	if (o.size)
		o.buf[o.n < o.size ? o.n : o.size - 1] = '\0';
	return (int)o.n;
}

static int log_snprintf(char *buf, unsigned int size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = log_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

static int log_file_append(const char *data, unsigned int size)
{
	SceUID fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	int ret;

	if (fd < 0)
		return fd;
	ret = sceIoWrite(fd, data, size);
	sceIoClose(fd);
	return ret < 0 ? ret : 0;
}

static void ring_push(const char *s, unsigned int len)
{
	unsigned int intr = sceKernelCpuSuspendIntr();
	unsigned int free_space = LOG_RING_SIZE - (ring_head - ring_tail);

	if (len > free_space) {
		dropped++;
	} else {
		unsigned int i;
		for (i = 0; i < len; i++)
			ring[(ring_head + i) % LOG_RING_SIZE] = s[i];
		ring_head += len;
	}
	sceKernelCpuResumeIntr(intr);
}

/* Drain the ring to the file. Returns the first I/O error, or 0. */
static int ring_drain_unlocked(void)
{
	char chunk[512];
	int err = 0;

	for (;;) {
		unsigned int n = 0;
		unsigned int intr = sceKernelCpuSuspendIntr();
		unsigned int avail = ring_head - ring_tail;

		while (n < avail && n < sizeof(chunk)) {
			chunk[n] = ring[(ring_tail + n) % LOG_RING_SIZE];
			n++;
		}
		ring_tail += n;
		sceKernelCpuResumeIntr(intr);

		if (n == 0)
			break;
		if (!err)
			err = log_file_append(chunk, n);
	}

	if (dropped) {
		char msg[64];
		unsigned int d;
		unsigned int intr = sceKernelCpuSuspendIntr();
		d = dropped;
		dropped = 0;
		sceKernelCpuResumeIntr(intr);
		log_snprintf(msg, sizeof(msg), "[log ring overflow: %u message(s) dropped]\n", d);
		if (!err)
			err = log_file_append(msg, strlen(msg));
	}
	return err;
}

static int ring_drain(void)
{
	SceUInt timeout;
	int ret;

	if (drain_sema < 0)
		return ring_drain_unlocked();
	/* Bounded: a writer thread stuck in Memory Stick I/O must not be able to
	 * block module_stop / module_reboot_before. */
	timeout = LOG_DRAIN_WAIT_US;
	ret = sceKernelWaitSema(drain_sema, 1, &timeout);
	if (ret < 0)
		return ret;
	ret = ring_drain_unlocked();
	sceKernelSignalSema(drain_sema, 1);
	return ret;
}

static int log_writer_thread(SceSize args, void *argp)
{
	while (writer_run) {
		sceKernelDelayThread(LOG_FLUSH_PERIOD_US);
		ring_drain();
	}
	ring_drain();
	return 0;
}

/* "<dir>/<name>.prx" -> "<dir>/<name>.log" */
static int build_log_path(const char *module_path)
{
	size_t len;
	const char *dot, *slash;

	if (module_path == NULL || strchr(module_path, ':') == NULL)
		return -1;
	len = strlen(module_path);
	if (len + 4 >= LOG_PATH_MAX)
		return -1;

	slash = strrchr(module_path, '/');
	dot = strrchr(module_path, '.');
	if (dot == NULL || (slash != NULL && dot < slash))
		dot = module_path + len;

	memcpy(log_path, module_path, dot - module_path);
	strcpy(log_path + (dot - module_path), ".log");
	return 0;
}

/*
 * Returns < 0 if logging could not be set up. That is not fatal for the caller:
 * log_printf() is a no-op until a session is open, so the plugin keeps running
 * without a log rather than exiting.
 */
int log_init(const char *module_path)
{
	char hdr[128];
	int ret;

	if (build_log_path(module_path) < 0)
		return -1;

	ring_head = ring_tail = 0;
	dropped = 0;

	/* The build timestamp precedes any other output of the session. */
	log_snprintf(hdr, sizeof(hdr), "\n=== UVC build %s ===\n", BUILD_TIMESTAMP);
	ret = log_file_append(hdr, strlen(hdr));
	if (ret < 0)
		return ret;

	drain_sema = sceKernelCreateSema("uvc_log_drain", 0, 1, 1, NULL);
	writer_run = 1;
	writer_thid = sceKernelCreateThread("uvc_log", log_writer_thread, LOG_WRITER_PRIORITY,
					    LOG_WRITER_STACK, 0, NULL);
	if (writer_thid < 0) {
		ret = writer_thid;
		writer_thid = -1;
		return ret;
	}
	ret = sceKernelStartThread(writer_thid, 0, NULL);
	if (ret < 0) {
		sceKernelDeleteThread(writer_thid);
		writer_thid = -1;
		return ret;
	}
	return 0;
}

void log_printf(const char *fmt, ...)
{
	char line[LOG_LINE_MAX];
	va_list ap;
	int n;

	if (writer_thid < 0)
		return;

	n = log_snprintf(line, sizeof(line), "[%10u] ", (unsigned int)sceKernelGetSystemTimeLow());
	va_start(ap, fmt);
	n += log_vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof(line)) {
		n = sizeof(line) - 1;
		line[n - 1] = '\n'; /* truncated: keep one message per line */
	}
	ring_push(line, n);
}

/* Write everything queued so far to the file, now, on the calling thread. */
void log_flush(void)
{
	if (writer_thid < 0)
		return;
	ring_drain();
}

void log_shutdown(void)
{
	SceUInt timeout = LOG_STOP_TIMEOUT_US;

	if (writer_thid < 0)
		return;
	writer_run = 0;
	if (sceKernelWaitThreadEnd(writer_thid, &timeout) < 0)
		sceKernelTerminateThread(writer_thid);
	sceKernelDeleteThread(writer_thid);
	writer_thid = -1;
	ring_drain();
	if (drain_sema >= 0)
		sceKernelDeleteSema(drain_sema);
	drain_sema = -1;
}
