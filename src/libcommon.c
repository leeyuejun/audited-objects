// Copyright (c) 2005-2011 David Boyce.  All rights reserved.

/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/// @file
/// @brief The part of the auditor library which is common to
/// both Unix and Windows.

#include "ca.c"

#include "code.c"

#include "moment.c"

#include "pn.c"

#include "prefs.c"

#include "prop.c"

#include "pa.c"

#include "ps.c"

#include "re.c"

#include "sha1.c"

#include "util.c"

#include "vb.c"

#include "ACK.h"

#if defined(_WIN32)
#include <winsock2.h>
#else				/*!_WIN32 */
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif	/*!_WIN32*/

#define DONE				"{DONE}\n"

// These may require some fancy stepping in order to maintain
// thread safety within a potentially multithreaded host program.
// The general strategy is to initialize static data once during
// the single-threaded constructor and treat them as read-only
// afterwards but that is not always an option.
static unsigned long Depth;
static ca_o CurrentCA;
static int AuditFD = -1;
static void *IgnorePathRE;
static void *IgnoreProgRE;
static void *AllowedWritePathRE;

/// The socket through which all communication to the monitor takes place.
SOCKET ReportSocket = INVALID_SOCKET;

// This static flag indicates whether the auditor is active or quiescent.
// The default kind of activation is when the auditor is turned on from
// process start to process end. The other activation mode is when a long-
// running process, typically an IDE, may run with the auditor present
// but inactive during editing work but then turn it on, do a build,
// and turn it back off. This latter mode is more complex, less well
// tested, and not yet documented but it would be great if it could
// be made to work reliably.
// Actually even 'request mode' has two variants. One is when we
// match an activtion RE internally and turn ourselves on, so to speak.
// The other is when the IDE makes an explicit call saying "start
// auditing now". Neither is well tested though.
static enum {
    LIBAO_INACTIVE,		///< The auditor is not active.
    LIBAO_ACTIVE_BY_DEFAULT,	///< The auditor is active throughout.
    LIBAO_ACTIVE_BY_REQUEST,	///< The auditor was turned on by request.
} Activated = LIBAO_INACTIVE;

/// From each audited command the auditor library must "steal"
/// one file descriptor in order to send the audit packet back
/// to the monitor. We try not to take one of the "popular"
/// (low-numbered) ones but we don't want anything over 256 either
/// in case of low ceilings, and we also want to stay away from
/// 256,255,etc in case some clever tool likes to start at the
/// top and work downwards. So we begin by using an ordinary,
/// undistinguished descriptor such as this one.
#define PREFERRED_FD		212

/// A flag to _audit_end() indicating that we do not expect to exit.
#define NOT_EXITING		0

/// A flag to _audit_end() indicating that we're about to exit.
#define EXITING			1

static void _audit_end(CCS, int, long);

/// Un-exported API to ask whether the auditor is globally active.
/// @return true if so activated
static int
_auditor_isActiveByDefault()
{
    return Activated == LIBAO_ACTIVE_BY_DEFAULT;
}

/// Un-exported API to ask whether the auditor is was activated by request.
/// @return true if so activated
static int
_auditor_isActiveByRequest()
{
    return Activated == LIBAO_ACTIVE_BY_REQUEST;
}

/// Un-exported API to ask whether the auditor is currently active.
/// @return true if activated
static int
_auditor_isActive()
{
    return Activated != LIBAO_INACTIVE;
}

/// Exported API to request temporary auditor activation.
void
auditor_setActive(void)
{
    Activated = LIBAO_ACTIVE_BY_REQUEST;
}

/// Un-exported API to make the auditor globally active.
static void
_auditor_setActiveByDefault(void)
{
    Activated = LIBAO_ACTIVE_BY_DEFAULT;
}

/// Exported API to make the auditor inactive.
void
auditor_setInactive(void)
{
    Activated = LIBAO_INACTIVE;
}

// Internal service routine. Called from intercepted system calls
// to register a file access (read, write, link, unlink, etc).
static void
_pa_record(CCS call, CCS path, CCS extra, int fd, op_e op)
{
    pn_o pn = NULL;
    ps_o ps;
    pa_o pa;

    if (!_auditor_isActive()) {
	vb_printf(VB_REC, "not active: %c,%s,%s", op, call, path);
	return;
    }

    pn = pn_new(path, 1);
    path = pn_get_abs(pn);

    if (_ignore_path(path)) {
	// We needed to derive a fully qualified path before doing this check.
	vb_printf(VB_REC, "ignoring: %c,%s,%s", op, call, path);
	pn_destroy(pn);
    } else if (!CurrentCA) {
	// This was moved down to after _ignore_path() to avoid
	// certain funky loops, such as when ao is run under the OS X
	// malloc debugger which writes a malloc_history file into /tmp
	// during exit processing.
	putil_int("PA after EOA: call=%s pid=%lu path=%s",
		  call, (unsigned long)getpid(), path);
    } else {
	vb_printf(VB_REC, "recording: %c,%s,%s", op, call, path);

	pa = pa_new();
	pa_set_op(pa, op);
	pa_set_call(pa, call);
	pa_set_pid(pa, ca_get_cmdid(CurrentCA));
	pa_set_ppid(pa, ca_get_pcmdid(CurrentCA));
	pa_set_depth(pa, ca_get_depth(CurrentCA));
	pa_set_pccode(pa, ca_get_pccode(CurrentCA));
	pa_set_ccode(pa, ca_get_ccode(CurrentCA));
	pa_set_fd(pa, fd);

	// Only store the thread id for a threaded program (on Windows
	// they're all threaded). We believe 0 to be an invalid thread
	// id which is thus used to indicate "unthreaded".
#if defined(_WIN32)
	// This won't work right because it's not "stable", meaning that
	// the Win32 thread id seems to be a random number. We need to
	// synthesize something that's the same from run to run (even this
	// must assume the host program creates threads in a static order).
	pa_set_tid(pa, (unsigned long)GetCurrentThreadId());
#else	/*_WIN32*/
	if (pthread_self_real) {
	    pa_set_tid(pa, (unsigned long)pthread_self_real());
	} else {
	    pa_set_tid(pa, 0);
	}
#endif	/*_WIN32*/

	ps = ps_new();
	ps_set_pn(ps, pn);

	// If this was a link op we may have a 2nd path to remember.
	if (extra) {
	    if (op == OP_SYMLINK) {
		ps_set_target(ps, extra);
	    } else {
		ps_set_pn2(ps, pn_new(extra, 1));
	    }
	}

	pa_set_ps(pa, ps);

	if (op == OP_UNLINK) {
	    ps_set_unlinked(ps);
	} else if (op == OP_MKDIR) {
	    ps_set_dir(ps);
	} else if (op == OP_SYMLINK) {
	    ps_set_symlinked(ps);
	} else if (op == OP_LINK) {
	    ps_set_linked(ps);
	}

	if (!pa_is_read(pa)) {
	    moment_s tstamp;

	    // Read ops don't need to be time-stamped.
	    moment_get_systime(&tstamp);
	    pa_set_timestamp(pa, tstamp);

	    // User may choose to limit legal pathnames for writes.
	    if (AllowedWritePathRE && !re_match__(AllowedWritePathRE, path)) {
		putil_die("disallowed write to '%s' per /%s/", path,
		    prop_get_str(P_ALLOWED_WRITE_PATH_RE));
	    }
	}

	// Finish by recording this path action.
	_thread_mutex_lock();
	ca_record_pa(CurrentCA, pa);
	_thread_mutex_unlock();
    }
}

// NOTE: The current design opens two sockets per audited command;
// one to deliver the SOA at initialization time, after which we
// block until it's closed by the monitor, and then another at
// finalization time to deliver everything else. I've considered
// trying to keep the one socket open in between on the theory
// (untested) that opening two sockets has a measurable cost, but
// so far am holding back for a few reasons: (1) it's not been
// shown to matter, (2) by holding sockets open from cmd start to
// end we increase the risk of running out of file descriptors
// in the monitor. This may only become an issue in deeply nested
// process trees, such as a deep recursive make, but it could happen.
// Also, (3) there are some buffering and ordering issues I don't
// understand well enough. These can be very subtle. The rule is that
// SOA absolutely MUST be seen before (a) its matching EOA and
// (b) any audit data from its children. Since the monitor loops
// through file descriptors in order (sockmin=>sockmax), if we hold
// some sockets open then is it possible a child might get issued
// a lower-numbered descriptor than its parent and thus be seen
// first? I haven't thought, or tested, it out. However, the auditor
// code is designed to be easily converted to use a single socket
// in the event the change is someday made.
static void
_monitor_open(SOCKET *sockp, CCS call)
{
    struct sockaddr_in dest_addr;
    const char *host;
    const char *portstr;
    char *nextport;
    u_short port;
    char host_port[256] = "???";
    int tries;

    UNUSED(call);

    if (*sockp != INVALID_SOCKET) {
	vb_printf(VB_MON, "ALREADY OPEN: SOCKET %d", *sockp);
	return;
    }

    util_socket_lib_init();

    if (((*sockp = socket(PF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)) {
	putil_syserr(2, "socket");
    }

    host = prop_get_str(P_MONITOR_HOST);
    portstr = prop_get_str(P_MONITOR_PORT);

    for (tries = 0; portstr && *portstr; tries++) {
	port = (u_short)strtoul(portstr, &nextport, 0);
	portstr = nextport + 1;
	snprintf(host_port, charlen(host_port), "%s:%u", host, port);

	if (tries) {
	    vb_printf(VB_MON, "Retrying on %s in %lu",
		host_port, (unsigned long)getpid());
	}

	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.sin_family = PF_INET;
	dest_addr.sin_addr.s_addr = inet_addr(host);
	dest_addr.sin_port = htons(port);

	if (!connect(*sockp, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr))) {
	    vb_printf(VB_MON, "OPENED: SOCKET %d", *sockp);
	    return;
	}

#if defined(_WIN32)
	if (WSAGetLastError() == WSAETIMEDOUT) {
	    continue;
	}
#else				/*!_WIN32 */
	if (errno == ETIMEDOUT || errno == EINTR) {
	    vb_printf(VB_STD, "RETRY CONNECT");
	    continue;
	} else if (errno == EISCONN) {
	    vb_printf(VB_MON, "CONNECTED: SOCKET %d", *sockp);
	    return;
	}
#endif	/*_WIN32*/
    }

    putil_syserr(2, host_port);
}

// Internal service routine.
static void
_monitor_send(SOCKET *sockp, const void *buf, size_t len)
{
    vb_printf(VB_MON, "SENDING TO: SOCKET %d", *sockp);
    if (util_send_all(*sockp, buf, len, 0) < 0) {
	putil_syserr(2, "send");
    }
}

// Internal service routine.
static void
_monitor_recv_line(SOCKET *sockp, const void *vbuf, size_t len)
{
    char *buf, *p;
    ssize_t ret;

    vb_printf(VB_MON, "RECVING FROM: SOCKET %d", *sockp);
    buf = (char *)vbuf;
    for (p = buf; ;) {
	ret = recv(*sockp, p, len - (p - buf), 0);
	if (ret == INVALID_SOCKET) {
#if defined(EINTR)
	    if (errno == EINTR) {
		continue;
	    }
#endif	/*!EINTR*/
	    putil_syserr(2, "recv()");
	    break;
	} else if (ret == 0) {
	    break;
	} else {
	    char *e;

	    if ((e = strchr(p, '\n'))) {
		*e = '\0';
		break;
	    } else {
		p += ret;
	    }
	}
    }
}

// Internal service routine.
static void
_monitor_flush(SOCKET *sockp)
{
    vb_printf(VB_MON, "FLUSHING: SOCKET %d", *sockp);
    (void)shutdown(*sockp, 1);
}

// Internal service routine.
static void
_monitor_close(SOCKET *sockp, int exiting)
{
    vb_printf(VB_MON, "CLOSING: SOCKET %d", *sockp);
    if (*sockp == INVALID_SOCKET) {
	return;
    }

#if defined(_WIN32)
    closesocket(*sockp);
#else				/*!_WIN32 */
    close(*sockp);
#endif	/*_WIN32*/

    // Despite what MSDN implies, I do not believe there's
    // any need to call WSACleanup() in the Windows auditor.
    // We always need sockets right up until exit time, at
    // which point the DLL will be unloaded anyway. The
    // situation is quite analogous to freeing memory; if
    // it happens just before exit it's redundant. Meanwhile
    // the complexities of counting calls to WSAStartup()
    // can be irritating and potentially threaten thread
    // safety. So, unless and until it becomes a problem,
    // we are going to let the system handle socket cleanup.
    if (exiting) {
	//util_socket_lib_fini();
    }

    *sockp = INVALID_SOCKET;
}

// Internal service routine. Return a file descriptor suitable
// for writing the audit record into.
static int
_audit_open(void)
{
    int fd;

    if (!_auditor_isActive()) {
	return -1;
    }

    // For debugging purposes we allow a mode where output is written
    // directly to a file descriptor. In the normal case it's sent to
    // a socket and received by the monitor.
    if (prop_is_true(P_NO_MONITOR)) {
	CCS ofile;

	if ((ofile = prop_get_str(P_OUTPUT_FILE))) {
	    if (!strcmp(ofile, "-")) {
		fd = STDOUT_FILENO;
	    } else if (!strcmp(ofile, "=")) {
		fd = STDERR_FILENO;
	    } else {
#if defined(_WIN32)
		// Haven't decided how to handle this for win32 yet.
		fd = STDERR_FILENO;
#else	/*_WIN32*/
		// If the file already exists, just append to it.
		// If not, make a new file with the pid appended.
		fd = open_real(ofile, O_WRONLY | O_APPEND, 0);
		if (fd == -1) {
		    char *obuf;

		    asprintf(&obuf, "%s.%lu", ofile, (unsigned long)getpid());
		    fd = open_real(obuf, O_CREAT | O_WRONLY | O_APPEND, 0666);
		    if (fd == -1) {
			putil_syserr(2, obuf);
		    }
		    putil_free(obuf);
		}
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
#endif	/*_WIN32*/
	    }
	} else {
	    fd = STDERR_FILENO;
	}
    } else {
	int i;

#if defined(_WIN32)
	CS tfn;
	HANDLE fh;

	// We can't use tmpfile() on Windows because it uses
	// CreateFile() and we'd fall into our own hook.
	// Also it creates the file in the root of the current
	// drive, which fails in ClearCase dynamic views and probably
	// other situations too. We step carefully here to avoid
	// falling into the CreateFile hook.
	for (fd = -1, i = 0; fd == -1 && i < 10; i++) {
	    if (!(tfn = _tempnam(NULL, "ao."))) {
		putil_syserr(2, "tempnam()");
	    }
	    fh = CreateFileA_real(tfn,
				  GENERIC_READ | GENERIC_WRITE | DELETE,
				  FILE_SHARE_READ | FILE_SHARE_WRITE |
				  FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
				  FILE_ATTRIBUTE_TEMPORARY |
				  FILE_FLAG_DELETE_ON_CLOSE, NULL);
	    if (fh == (HANDLE)-1) {
		putil_win32err(2, GetLastError(), tfn);
	    }
	    if ((fd = _open_osfhandle((intptr_t)fh, _O_RDWR)) == -1) {
		putil_syserr(2, "_open_osfhandle()");
	    }
	    free(tfn);
	}

	if (fd == -1) {
	    putil_syserr(2, "tempnam()");
	}
#else	/*_WIN32*/
	FILE *tfp;

	// Create an unnamed/unlinked temp file. Unfortunately this
	// returns us a stream and not a descriptor. We try not to use
	// up stdio streams because they're limited to 256 in many cases,
	// so we convert it to a high-numbered descriptor and close
	// the stream.
	if (!(tfp = tmpfile())) {
	    putil_syserr(2, "tmpfile");
	}

	// If our preferred fd is not in use, try using it.
	// Try a few above that next - this is in case of a
	// fork where we want the child side to have its own
	// temp file in order to keep the contents from being
	// scrambled.
	for (fd = -1, i = 0; i < 10; i++) {
	    if (lseek(PREFERRED_FD + i, 0, SEEK_CUR) == -1) {
		if (dup2(fileno(tfp), PREFERRED_FD + i) != -1) {
		    fd = PREFERRED_FD + i;
		    break;
		}
	    }
	}

	// If all the above efforts failed, just ask the system
	// for the next available descriptor.
	if (fd == -1) {
	    if ((fd = dup(fileno(tfp))) == -1) {
		putil_syserr(2, "dup");
	    }
	}

	// Now the stream can go away to preserve limited stdio count.
	if (fclose(tfp)) {
	    putil_syserr(2, "fclose");
	}

	// Each new cmd and/or process must create its own temp file.
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
#endif	/*_WIN32*/
    }

    return fd;
}

// Internal service routine. Send an SOA record to the appropriate
// place (to monitor via socket or debug file).
static void
_audit_start(CCS call)
{
    CCS hdr;
    CS soa_hdr;

    if (!_auditor_isActive()) {
	return;
    }

    // Allocate and format the cmdline SOA header.
    hdr = ca_format_header(CurrentCA);
    if (!hdr || asprintf(&soa_hdr, "%s%s", SOA, hdr) < 0) {
	putil_syserr(2, NULL);
    }
    putil_free(hdr);

    // This is an optimization hack - we use lower case to indicate
    // that this command is part of an aggregation and thus need not
    // be shopped for.
    if (prop_is_true(P_AGGREGATED_SUBCMD)) {
	soa_hdr[1] = 's';
    }

    // Acquire the descriptor we'll be using for audit reporting.
    AuditFD = _audit_open();

    // Write the SOA record into the file descriptor or socket.
    // Writes and sends need to be separated because you can't write()
    // to a socket on Windows and can't send() to a file anywhere.
    if (!prop_is_true(P_NO_MONITOR)) {
	char ack_soa[ACK_BUFFER_SIZE], *p;

	// The SOA must arrive at the monitor before the SOAs of any
	// children.  Thus we do not stash it for later delivery
	// but send it synchronously now. This is unfortunate
	// since two socket negotiations may be significantly more
	// expensive than one, though that has not been proven.
	// It's worth thinking this through again to see if there's
	// way around it, either by keeping the socket open or
	// allowing SOA to be sent in the same packet as EOA.
	_monitor_open(&ReportSocket, call);
	_monitor_send(&ReportSocket, soa_hdr, strlen(soa_hdr));
	putil_free(soa_hdr);

	// Block until monitor acknowledges receipt of SOA.
	// This is needed to make sure it doesn't see EOA first.
	// We require the received message to be terminated with a newline;
	// we don't actually want the newline but it's a good way to
	// be sure we've read the whole message.
	_monitor_flush(&ReportSocket);
	_monitor_recv_line(&ReportSocket, ack_soa, sizeof(ack_soa));
	_monitor_close(&ReportSocket, 0);

	vb_printf(VB_MON, "CONTINUING [%s] WITH %s",
	    ack_soa, ca_get_line(CurrentCA));

	if (!strcmp(ack_soa, ACK_FAILURE)) {
	    _audit_end("exit", EXITING, 2);
	    exit(2);
	} else if (!strcmp(ack_soa, ACK_OK_AGG)) {
	    // Optimization: suppress shopping in aggregated children.
	    if (prop_has_value(P_AGGREGATED_SUBCMD)) {
		prop_mod_ulong(P_AGGREGATED_SUBCMD, 1, NULL);
	    }
	} else if (!strcmp(ack_soa, ACK_OK)) {
	    // Everything normal - carry on.
	} else {
	    // We now know the command was successfully recycled so
	    // we can quit and go home early.
	    if ((p = strstr(ack_soa, FS1))) {
		*p = '\0';
		ca_set_recycled(CurrentCA, ack_soa);
	    }

	    // This should trigger an immediate EOA with a
	    // field showing which PTX it was recycled from.
	    // Exiting is VERY tricky here with all the variants
	    // and hooks.
	    // This seems to work but could maybe be done better.
	    _audit_end("exit", EXITING, 0);
	    exit(0);
	}
    } else {
	if (write(AuditFD, soa_hdr, strlen(soa_hdr)) == -1) {
	    putil_syserr(2, "write");
	}
	putil_free(soa_hdr);
    }

    return;
}

// Dump and flush whatever file data we've acquired. This is done
// just before the process disappears through either exit or exec.
// It's also done before fork, in which case it may be partial.
// We must also deal with path accesses collected on the
// child side of a fork before an exec, if any.
// Before any child process is created or this process
// is overlaid via exec, we must make sure the current
// audit state is not lost (exec) or duplicated (fork).
// In addition, it's critical that the SOA be seen by the
// monitor BEFORE any audit data arrives from a subsidiary cmd.
// Thus the SOA must be sent ASAP but the file data need only
// be collected for later transmission.
static void
_audit_flush(CCS call)
{
    UNUSED(call);

    if (!_auditor_isActive()) {
	return;
    }

    // Technically we may not need to synchronize here when called
    // during exit processing, but when called from anywhere else
    // we do, so the simplest implementation is to lock within this
    // function rather than at most of the calls to it.
    // This lock protects writes to both CurrentCA *and* AuditFD.
    _thread_mutex_lock();

    // This is an example of something we might want to do during
    // debugging if a host process is catching SIGSEGV (as Sun's
    // JVM does for instance). Of course a case could be made
    // that we could reset *all* signal dispositions during finish
    // processing since the application thinks it's done.
    //    assert(signal(SIGSEGV, SIG_DFL) != SIG_ERR);

    // Walk the tree, printing each pa node to the fd.
    // Note - ca_get_recycled is most likely redundant with ca_get_pa_count.
    if (!ca_get_recycled(CurrentCA) && ca_get_pa_count(CurrentCA)) {
	ca_write(CurrentCA, AuditFD);
    }

    // Let the lock go.
    _thread_mutex_unlock();
}

// This is called when a process is "ending" - either exit()
// or exec(). The difference is that you never know whether an
// exec will succeed.
static void
_audit_end(CCS call, int exiting, long status)
{
    int n;
    char ack_eoa[ACK_BUFFER_SIZE];

    if (!_auditor_isActive()) {
#if defined(_WIN32)
	// Very special case: if (a) we're on Windows, (b) this
	// command is not "activated", (c) it's the top-level command,
	// (d) it's exiting, and (e) we're talking with the
	// monitor, then we need to finish by sending it a DONE
	// token.  On Unix the monitor can catch SIGCHILD but
	// not on Windows.
	if (Depth == 0 && exiting && !prop_is_true(P_NO_MONITOR)) {
	    _monitor_open(&ReportSocket, call);
	    _monitor_send(&ReportSocket, DONE, strlen(DONE));
	    _monitor_flush(&ReportSocket);
	    _monitor_close(&ReportSocket, 1);
	}
#endif	/*_WIN32*/
	return;
    }

    // This will start and/or flush the audit as required.
    _audit_flush(call);

#if defined(SIGPIPE) && defined(SIG_IGN)
    // This fixes a subtle problem: on a successful recycling event,
    // the monitor will cause the audited command to exit early. If
    // that program has a child which is writing to it through a pipe,
    // the effect of exiting the parent will be a SIGPIPE in the
    // child which will typically cause it to fail. Thus, when we're
    // about to finish up we surreptitiously force the audited
    // process to ignore SIGPIPE. Unlikely to break anything
    // since we're exiting anyway but yes it's an intrusive hack.
    // TODO - this placement seems overly aggressive because it will
    // ignore SIGPIPE on all exits and execs, whereas in theory
    // we should only need this when exiting early due to
    // recycle. But I tried moving it once and it broke so I moved
    // it back and added this note. Maybe I put it in the wrong
    // place. Analyze someday.
    // A downloading build of OpenSSL 1.0 triggers this problem, at
    // least on Ubuntu and at one time on Solaris 10 X86.
    {
	struct sigaction sigpipe;

	sigpipe.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sigpipe, NULL);
    }
#endif

    if (!prop_is_true(P_NO_MONITOR)) {
	if (AuditFD != -1 && CurrentCA && !ca_get_recycled(CurrentCA)) {
	    char buf[1024];

	    _monitor_open(&ReportSocket, call);

	    // Rewind the temp file and send its contents to the monitor.
	    if (lseek(AuditFD, 0, SEEK_SET) == (off_t)-1) {
		putil_syserr(2, "lseek");
	    }

	    while ((n = read(AuditFD, buf, sizeof(buf))) != 0) {
		if (n == -1) {
#if defined(EINTR)
		    if (errno == EINTR) {
			continue;
		    }
#endif	/*!EINTR*/
		    putil_syserr(2, "read");
		}
		_monitor_send(&ReportSocket, buf, n);
	    }
	}
    }

    // Write the EOA marker if we're exiting AND an SOA was sent.
    if (exiting && ca_get_started(CurrentCA)) {
	CCS hdr;
	CS eoa_hdr;

	// Allocate and format the cmdline header.
	hdr = ca_format_header(CurrentCA);
	assert(hdr);
	if (asprintf(&eoa_hdr, "%s[%ld]%s", EOA, status, hdr) < 0) {
	    putil_syserr(2, NULL);
	}
	putil_free(hdr);

	if (ReportSocket == INVALID_SOCKET) {
	    if (write(AuditFD, eoa_hdr, strlen(eoa_hdr)) == -1) {
		putil_syserr(2, "write");
	    }
	} else {
	    _monitor_send(&ReportSocket, eoa_hdr, strlen(eoa_hdr));
	}

	putil_free(eoa_hdr);
	ca_destroy(CurrentCA);
	CurrentCA = NULL;
	close(AuditFD);
	AuditFD = -1;
    } else if (!prop_is_true(P_NO_MONITOR)) {
	// In case more data is written to the temp file, reposition
	// its pointer at the beginning.
	if (lseek(AuditFD, 0, SEEK_SET) == (off_t)-1) {
	    putil_syserr(2, "lseek");
	}
#if defined(_WIN32)
	if (_chsize(AuditFD, 0)) {
	    putil_syserr(2, "_chsize");
	}
#else	/*_WIN32*/
	if (ftruncate(AuditFD, 0)) {
	    putil_syserr(2, "ftruncate");
	}
#endif	/*!_WIN32*/
    }

    // No ACK needed in no-monitor mode ...
    if (ReportSocket == INVALID_SOCKET) {
	return;
    }

    // Block until monitor acknowledges receipt of EOA by closing
    // the other end.
    _monitor_flush(&ReportSocket);
    _monitor_recv_line(&ReportSocket, ack_eoa, sizeof(ack_eoa));

    // And of course close this end when done.
    _monitor_close(&ReportSocket, 1);
}

// Internal service routine.
// Note: it should be thread-safe to initialize static data here
// since this will be called at library initialization time when
// only one thread should be running.
static unsigned long
_init_auditlib(CCS call, CCS exe, CCS cmdstr)
{
    unsigned long pid;
    CCS rwd;
    CCS str;
    CCS pccode = NULL;

    // For a reason I don't understand, and would have to read
    // the Perl sources to learn, re-setting EVs in the
    // environment with putenv() causes perl to core dump.
    // However, when we modify them in place there's no problem.
    // Best guess is that perl maintains a static pointer to
    // the env block and the putenv() causes a realloc of said
    // env block leaving perl's pointer dangling.
    // The main point is, even if this isn't exactly what's
    // happening in perl, the case described could happen in any
    // program. Therefore the rule is that the auditor must *NEVER*
    // add or modify an env var via putenv(). The wrapper program must
    // make sure an EV is already present with sufficient padding
    // and the auditor can then twiddle those bits in place.
    // For instance, if an EV must hold a number we allocate 10
    // bytes (plus nul) for it in the wrapper because that's the
    // number of characters needed to represent 2**32 in decimal.

    // Initialize verbosity asap.
    vb_init();

    // Can't see any properties without initializing this first.
    prop_init(APPLICATION_NAME);

    // We do not want to take properties from files within the
    // auditor as the CWD may be changing, so everything we care
    // about should have been placed in the environment by now.
    prefs_init(exe, NULL, NULL);

    if (prop_has_value(P_DEPTH)) {
	// Find the current depth and increment it for child cmds.
	// From here on P_DEPTH is a lie for the current cmd.
	Depth = prop_get_ulong(P_DEPTH);
	prop_mod_ulong(P_DEPTH, Depth + 1, NULL);

	pccode = prop_get_str(P_PCCODE);
    } else {
	// Special case to allow "naked" testing of the auditor via e.g.
	// "LD_PRELOAD=/path/to/libAO.so some-command".
	prop_override_true(P_NO_MONITOR);
	vb_printf(VB_OFF, "running in %s mode", prop_to_name(P_NO_MONITOR));
    }

    // Initialize the hash-code generation.
    code_init();

    // We'll need this more than once, so remember it here.
    pid = getpid();

    // Optionally show commands as executed.
    if (vb_bitmatch(VB_EXEC)) {
	fprintf(vb_get_stream(), "+ %s\n", cmdstr);
    }

    // If full auditing is already activated, keep it going.
    // If not, and we have no activation RE, activate by default.
    // If we have an RE, apply it.
    if (!_auditor_isActive()) {
	void *re;

	if ((re = re_init_prop__(P_ACTIVATION_PROG_RE))) {
	    if (re_match__(re, exe)) {
		vb_printf(VB_OFF, "ACTIVATED ON [%ld] '%s'", pid, exe);
		auditor_setActive();
		// Can't remove this EV - must change it in place.
		prop_mod_str(P_ACTIVATION_PROG_RE, "", NULL);
	    } else {
		vb_printf(VB_OFF, "INACTIVATED ON [%ld] '%s'", pid, exe);
		return pid;
	    }
	    re_fini__(&re);
	} else {
	    vb_printf(VB_OFF, "ACTIVE ON [%ld] '%s'", pid, exe);
	    _auditor_setActiveByDefault();
	}
    }

    // Set up a new audit object with starting values. The cmdid
    // is always identical to the pid just after an exec (here).
    // On Unix it will diverge on fork() and converge on the
    // next exec. This is why we need to manage the parent cmdid
    // in the environment rather than just trusting getppid().
    CurrentCA = ca_new();
    ca_set_cmdid(CurrentCA, pid);
    ca_set_depth(CurrentCA, Depth);
    ca_set_pcmdid(CurrentCA, prop_get_ulong(P_PCMDID));
    ca_set_prog(CurrentCA, exe);
    ca_set_rwd(CurrentCA, rwd = util_get_rwd());
    putil_free(rwd);

    // This boolean means we've started a new cmd which will need
    // an SOA sent. Child processes should have this turned off,
    // because a process is NOT a cmd until an exec has taken place.
    ca_set_started(CurrentCA, 1);

    // Set the current hostname.
    if (!ca_get_host(CurrentCA)) {
	char hostbuf[HOST_NAME_MAX];

#if defined(_WIN32)
	DWORD namelen = charlen(hostbuf);

	if (!GetComputerNameEx(ComputerNameDnsHostname, hostbuf, &namelen)) {
	    putil_win32err(2, GetLastError(), "GetComputerNameEx()");
	}
#else	/*_WIN32*/
	if (gethostname(hostbuf, sizeof(hostbuf))) {
	    putil_syserr(2, "gethostname()");
	}
#endif	/*_WIN32*/

	ca_set_host(CurrentCA, hostbuf);
    }

    ca_set_line(CurrentCA, cmdstr);

    // If requested, and if the RE matches the current command,
    // record the current environment in the freetext field.
    if ((str = prop_get_str(P_TRACK_ENV_RE))) {
	void *cmd_re, *var_re;

	if (strrchr(str, ':')) {
	    CS tstr, p;

	    tstr = putil_strdup(str);
	    p = strrchr(tstr, ':');
	    *p = '\0';
	    cmd_re = re_init__("TRACK_ENV_CMD", tstr);
	    var_re = re_init__("TRACK_ENV_VAR", p + 1);
	    putil_free(tstr);
	} else {
	    cmd_re = re_init__("TRACK_ENV_CMD", str);
	    var_re = NULL;
	}

	if (re_match__(cmd_re, cmdstr)) {
	    CS eblock, encoded;
	    size_t elen;
	    char **envp;
	    CS end;

	    for (envp = environ, elen = 1; *envp; envp++) {
		elen += strlen(*envp) + 1;
	    }

	    eblock = (CS)putil_malloc(elen);

	    for (envp = environ, end = eblock; *envp && elen > 0; envp++) {
		if (!strstr(*envp, "TRACK_ENV")) {
		    if (!var_re || re_match__(var_re, *envp)) {
			int n;

			n = snprintf(end, elen, "%s\n", *envp);
			end += n;
			elen -= n;
		    }
		}
	    }

	    encoded = util_encode_minimal(eblock);
	    putil_free(eblock);
	    ca_set_freetext(CurrentCA, encoded);
	    putil_free(encoded);
	}

	if (cmd_re) {
	    re_fini__(&cmd_re);
	}
	if (var_re) {
	    re_fini__(&var_re);
	}
    }

    // Retrieve the grandparent and parent cmd codes from the
    // environment and place them in the current object. Commands near
    // the top of the cmd tree may have no parent or grandparent.

    // Parent command ...
    if (!pccode || CSV_FIELD_IS_NULL(pccode)) {
	ca_set_pccode(CurrentCA, NULL);
    } else {
	char pccbuf[256];

	snprintf(pccbuf, charlen(pccbuf), "%s", pccode);
	ca_set_pccode(CurrentCA, util_strtrim(pccbuf));
    }

    _audit_start(call);

    // Push the current ccode into the environment as the
    // parent ccode. From here on the environment will be a lie
    // but we have the truth in the CA object.
    if (prop_has_value(P_PCCODE))
	prop_mod_str(P_PCCODE, ca_get_ccode(CurrentCA), NULL);

    // Special case - show the program itself as a file that we read
    // (because it is and we did).
    _pa_record(call, putil_getexecpath(), NULL, -1, OP_EXEC);

    // Potential instruction from the user to ignore certain files.
    IgnorePathRE = re_init_prop__(P_AUDIT_IGNORE_PATH_RE);

    // Potential instruction from the user to ignore certain programs.
    IgnoreProgRE = re_init_prop__(P_AUDIT_IGNORE_PROG_RE);

    // Potential instruction from the user to limit legal write ops.
    AllowedWritePathRE = re_init_prop__(P_ALLOWED_WRITE_PATH_RE);

    return pid;
}
