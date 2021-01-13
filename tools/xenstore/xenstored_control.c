/*
Interactive commands for Xen Store Daemon.
    Copyright (C) 2017 Juergen Gross, SUSE Linux GmbH

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; If not, see <http://www.gnu.org/licenses/>.
*/

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils.h"
#include "talloc.h"
#include "xenstored_core.h"
#include "xenstored_control.h"

struct live_update {
	/* For verification the correct connection is acting. */
	struct connection *conn;

#ifdef __MINIOS__
	void *kernel;
	unsigned int kernel_size;
	unsigned int kernel_off;
#else
	char *filename;
#endif

	char *cmdline;

	/* Start parameters. */
	bool force;
	unsigned int timeout;
	time_t started_at;
};

static struct live_update *lu_status;

static int lu_destroy(void *data)
{
	lu_status = NULL;

	return 0;
}

static const char *lu_begin(struct connection *conn)
{
	if (lu_status)
		return "live-update session already active.";

	lu_status = talloc_zero(conn, struct live_update);
	if (!lu_status)
		return "Allocation failure.";
	lu_status->conn = conn;
	talloc_set_destructor(lu_status, lu_destroy);

	return NULL;
}

struct cmd_s {
	char *cmd;
	int (*func)(void *, struct connection *, char **, int);
	char *pars;
	/*
	 * max_pars can be used to limit the size of the parameter vector,
	 * e.g. in case of large binary parts in the parameters.
	 * The command is included in the count, so 1 means just the command
	 * without any parameter.
	 * 0 == no limit (the default)
	 */
	unsigned int max_pars;
};

static int do_control_check(void *ctx, struct connection *conn,
			    char **vec, int num)
{
	if (num)
		return EINVAL;

	check_store();

	send_ack(conn, XS_CONTROL);
	return 0;
}

static int do_control_log(void *ctx, struct connection *conn,
			  char **vec, int num)
{
	if (num != 1)
		return EINVAL;

	if (!strcmp(vec[0], "on"))
		reopen_log();
	else if (!strcmp(vec[0], "off"))
		close_log();
	else
		return EINVAL;

	send_ack(conn, XS_CONTROL);
	return 0;
}

#ifdef __MINIOS__
static int do_control_memreport(void *ctx, struct connection *conn,
				char **vec, int num)
{
	if (num)
		return EINVAL;

	talloc_report_full(NULL, stdout);

	send_ack(conn, XS_CONTROL);
	return 0;
}
#else
static int do_control_logfile(void *ctx, struct connection *conn,
			      char **vec, int num)
{
	if (num != 1)
		return EINVAL;

	close_log();
	talloc_free(tracefile);
	tracefile = talloc_strdup(NULL, vec[0]);
	reopen_log();

	send_ack(conn, XS_CONTROL);
	return 0;
}

static int do_control_memreport(void *ctx, struct connection *conn,
				char **vec, int num)
{
	FILE *fp;
	int fd;

	if (num > 1)
		return EINVAL;

	if (num == 0) {
		if (tracefd < 0) {
			if (!tracefile)
				return EBADF;
			fp = fopen(tracefile, "a");
		} else {
			/*
			 * Use dup() in order to avoid closing the file later
			 * with fclose() which will release stream resources.
			 */
			fd = dup(tracefd);
			if (fd < 0)
				return EBADF;
			fp = fdopen(fd, "a");
			if (!fp)
				close(fd);
		}
	} else
		fp = fopen(vec[0], "a");

	if (!fp)
		return EBADF;

	talloc_report_full(NULL, fp);
	fclose(fp);

	send_ack(conn, XS_CONTROL);
	return 0;
}
#endif

static int do_control_print(void *ctx, struct connection *conn,
			    char **vec, int num)
{
	if (num != 1)
		return EINVAL;

	xprintf("control: %s", vec[0]);

	send_ack(conn, XS_CONTROL);
	return 0;
}

static const char *lu_abort(const void *ctx, struct connection *conn)
{
	syslog(LOG_INFO, "live-update: abort\n");

	if (!lu_status)
		return "No live-update session active.";

	/* Destructor will do the real abort handling. */
	talloc_free(lu_status);

	return NULL;
}

static const char *lu_cmdline(const void *ctx, struct connection *conn,
			      const char *cmdline)
{
	syslog(LOG_INFO, "live-update: cmdline %s\n", cmdline);

	if (!lu_status || lu_status->conn != conn)
		return "Not in live-update session.";

	lu_status->cmdline = talloc_strdup(lu_status, cmdline);
	if (!lu_status->cmdline)
		return "Allocation failure.";

	return NULL;
}

#ifdef __MINIOS__
static const char *lu_binary_alloc(const void *ctx, struct connection *conn,
				   unsigned long size)
{
	const char *ret;

	syslog(LOG_INFO, "live-update: binary size %lu\n", size);

	ret = lu_begin(conn);
	if (ret)
		return ret;

	lu_status->kernel = talloc_size(lu_status, size);
	if (!lu_status->kernel)
		return "Allocation failure.";

	lu_status->kernel_size = size;
	lu_status->kernel_off = 0;

	return NULL;
}

static const char *lu_binary_save(const void *ctx, struct connection *conn,
				  unsigned int size, const char *data)
{
	if (!lu_status || lu_status->conn != conn)
		return "Not in live-update session.";

	if (lu_status->kernel_off + size > lu_status->kernel_size)
		return "Too much kernel data.";

	memcpy(lu_status->kernel + lu_status->kernel_off, data, size);
	lu_status->kernel_off += size;

	return NULL;
}

static const char *lu_arch(const void *ctx, struct connection *conn,
			   char **vec, int num)
{
	if (num == 2 && !strcmp(vec[0], "-b"))
		return lu_binary_alloc(ctx, conn, atol(vec[1]));
	if (num > 2 && !strcmp(vec[0], "-d"))
		return lu_binary_save(ctx, conn, atoi(vec[1]), vec[2]);

	errno = EINVAL;
	return NULL;
}
#else
static const char *lu_binary(const void *ctx, struct connection *conn,
			     const char *filename)
{
	const char *ret;
	struct stat statbuf;

	syslog(LOG_INFO, "live-update: binary %s\n", filename);

	if (stat(filename, &statbuf))
		return "File not accessible.";
	if (!(statbuf.st_mode & (S_IXOTH | S_IXGRP | S_IXUSR)))
		return "File not executable.";

	ret = lu_begin(conn);
	if (ret)
		return ret;

	lu_status->filename = talloc_strdup(lu_status, filename);
	if (!lu_status->filename)
		return "Allocation failure.";

	return NULL;
}

static const char *lu_arch(const void *ctx, struct connection *conn,
			   char **vec, int num)
{
	if (num == 2 && !strcmp(vec[0], "-f"))
		return lu_binary(ctx, conn, vec[1]);

	errno = EINVAL;
	return NULL;
}
#endif

static bool lu_check_lu_allowed(void)
{
	return true;
}

static const char *lu_reject_reason(const void *ctx)
{
	return "BUSY";
}

static const char *lu_dump_state(const void *ctx, struct connection *conn)
{
	return NULL;
}

static const char *lu_activate_binary(const void *ctx)
{
	return "Not yet implemented.";
}

static bool do_lu_start(struct delayed_request *req)
{
	time_t now = time(NULL);
	const char *ret;
	char *resp;

	if (!lu_check_lu_allowed()) {
		if (now < lu_status->started_at + lu_status->timeout)
			return false;
		if (!lu_status->force) {
			ret = lu_reject_reason(req);
			goto out;
		}
	}

	/* Dump out internal state, including "OK" for live update. */
	ret = lu_dump_state(req->in, lu_status->conn);
	if (!ret) {
		/* Perform the activation of new binary. */
		ret = lu_activate_binary(req->in);
	}

	/* We will reach this point only in case of failure. */
 out:
	talloc_free(lu_status);

	resp = talloc_strdup(req->in, ret);
	send_reply(lu_status->conn, XS_CONTROL, resp, strlen(resp) + 1);

	return true;
}

static const char *lu_start(const void *ctx, struct connection *conn,
			    bool force, unsigned int to)
{
	syslog(LOG_INFO, "live-update: start, force=%d, to=%u\n", force, to);

	if (!lu_status || lu_status->conn != conn)
		return "Not in live-update session.";

#ifdef __MINIOS__
	if (lu_status->kernel_size != lu_status->kernel_off)
		return "Kernel not complete.";
#endif

	lu_status->force = force;
	lu_status->timeout = to;
	lu_status->started_at = time(NULL);

	errno = delay_request(conn, conn->in, do_lu_start, NULL);

	return NULL;
}

static int do_control_lu(void *ctx, struct connection *conn,
			 char **vec, int num)
{
	const char *resp;
	const char *ret = NULL;
	unsigned int i;
	bool force = false;
	unsigned int to = 0;

	if (num < 1)
		return EINVAL;

	if (!strcmp(vec[0], "-a")) {
		if (num == 1)
			ret = lu_abort(ctx, conn);
		else
			return EINVAL;
	} else if (!strcmp(vec[0], "-c")) {
		if (num == 2)
			ret = lu_cmdline(ctx, conn, vec[1]);
		else
			return EINVAL;
	} else if (!strcmp(vec[0], "-s")) {
		for (i = 1; i < num; i++) {
			if (!strcmp(vec[i], "-F"))
				force = true;
			else if (!strcmp(vec[i], "-t") && i < num - 1) {
				i++;
				to = atoi(vec[i]);
			} else
				return EINVAL;
		}
		ret = lu_start(ctx, conn, force, to);
		if (errno)
			return errno;
		if (!ret)
			return 0;
	} else {
		errno = 0;
		ret = lu_arch(ctx, conn, vec, num);
		if (errno)
			return errno;
	}

	if (!ret)
		ret = "OK";
	resp = talloc_strdup(ctx, ret);
	send_reply(conn, XS_CONTROL, resp, strlen(resp) + 1);
	return 0;
}

static int do_control_help(void *, struct connection *, char **, int);

static struct cmd_s cmds[] = {
	{ "check", do_control_check, "" },
	{ "log", do_control_log, "on|off" },

	/*
	 * The parameters are those of the xenstore-control utility!
	 * Depending on environment (Mini-OS or daemon) the live-update
	 * sequence is split into several sub-operations:
	 * 1. Specification of new binary
	 *    daemon:  -f <filename>
	 *    Mini-OS: -b <binary-size>
	 *             -d <size> <data-bytes> (multiple of those)
	 * 2. New command-line (optional): -c <cmdline>
	 * 3. Start of update: -s [-F] [-t <timeout>]
	 * Any sub-operation needs to respond with the string "OK" in case
	 * of success, any other response indicates failure.
	 * A started live-update sequence can be aborted via "-a" (not
	 * needed in case of failure for the first or last live-update
	 * sub-operation).
	 */
	{ "live-update", do_control_lu,
		"[-c <cmdline>] [-F] [-t <timeout>] <file>\n"
		"    Default timeout is 60 seconds.", 4 },
#ifdef __MINIOS__
	{ "memreport", do_control_memreport, "" },
#else
	{ "logfile", do_control_logfile, "<file>" },
	{ "memreport", do_control_memreport, "[<file>]" },
#endif
	{ "print", do_control_print, "<string>" },
	{ "help", do_control_help, "" },
};

static int do_control_help(void *ctx, struct connection *conn,
			   char **vec, int num)
{
	int cmd, len = 0;
	char *resp;

	if (num)
		return EINVAL;

	for (cmd = 0; cmd < ARRAY_SIZE(cmds); cmd++) {
		len += strlen(cmds[cmd].cmd) + 1;
		len += strlen(cmds[cmd].pars) + 1;
	}
	len++;

	resp = talloc_array(ctx, char, len);
	if (!resp)
		return ENOMEM;

	len = 0;
	for (cmd = 0; cmd < ARRAY_SIZE(cmds); cmd++) {
		strcpy(resp + len, cmds[cmd].cmd);
		len += strlen(cmds[cmd].cmd);
		resp[len] = '\t';
		len++;
		strcpy(resp + len, cmds[cmd].pars);
		len += strlen(cmds[cmd].pars);
		resp[len] = '\n';
		len++;
	}
	resp[len] = 0;

	send_reply(conn, XS_CONTROL, resp, len);
	return 0;
}

int do_control(struct connection *conn, struct buffered_data *in)
{
	unsigned int cmd, num, off;
	char **vec = NULL;

	if (conn->id != 0)
		return EACCES;

	off = get_string(in, 0);
	if (!off)
		return EINVAL;
	for (cmd = 0; cmd < ARRAY_SIZE(cmds); cmd++)
		if (streq(in->buffer, cmds[cmd].cmd))
			break;
	if (cmd == ARRAY_SIZE(cmds))
		return EINVAL;

	num = xs_count_strings(in->buffer, in->used);
	if (cmds[cmd].max_pars)
		num = min(num, cmds[cmd].max_pars);
	vec = talloc_array(in, char *, num);
	if (!vec)
		return ENOMEM;
	if (get_strings(in, vec, num) < num)
		return EIO;

	return cmds[cmd].func(in, conn, vec + 1, num - 1);
}
