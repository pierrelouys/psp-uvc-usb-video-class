#ifndef UVC_LOG_H
#define UVC_LOG_H

/*
 * File logger. Messages are queued in a ring buffer and written to
 * "<plugin path without extension>.log" by a low-priority writer thread, so
 * logging never blocks the USB control path or the video loop on Memory Stick
 * I/O and never touches the display.
 *
 * The first line of every session is the build date/time with timezone.
 */

/* module_path is the path this PRX was loaded from (main()'s argv[0]). */
int log_init(const char *module_path);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Write everything queued so far to the file immediately. */
void log_flush(void);
/* Flush everything queued so far and stop the writer thread. */
void log_shutdown(void);

#define LOG(...) log_printf(__VA_ARGS__)

#endif
