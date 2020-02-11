#include "cache.h"
#include "simple-ipc.h"
#include "strbuf.h"

#ifdef GIT_WINDOWS_NATIVE
static int initialize_pipe_name(const char *path, wchar_t *wpath, size_t alloc)
{
	int off = 0;
	struct strbuf realpath = STRBUF_INIT;

	if (!strbuf_realpath(&realpath, path, 0))
		return error(_("could not normalize '%s'"), path);

	off = swprintf(wpath, alloc, L"\\\\.\\pipe\\");
	if (xutftowcs(wpath + off, realpath.buf, alloc - off) < 0)
		return error(_("could not determine pipe path for '%s'"),
			     realpath.buf);

	/* Handle drive prefix */
	if (wpath[off] && wpath[off + 1] == L':') {
		wpath[off + 1] = L'_';
		off += 2;
	}

	for (; wpath[off]; off++)
		if (wpath[off] == L'/')
			wpath[off] = L'\\';

	strbuf_release(&realpath);
	return 0;
}

static int is_active(wchar_t *pipe_path)
{
	return WaitNamedPipeW(pipe_path, 1) ||
		GetLastError() != ERROR_FILE_NOT_FOUND;
}

int ipc_is_active(const char *path)
{
	wchar_t pipe_path[MAX_PATH];

	if (initialize_pipe_name(path, pipe_path, ARRAY_SIZE(pipe_path)) < 0)
		return 0;

	return is_active(pipe_path);
}

struct ipc_handle_client_data {
	struct ipc_command_listener *server;
	HANDLE pipe;
};

static int reply(void *reply_data, const char *response)
{
	struct ipc_handle_client_data *data = reply_data;

	if (WriteFile(data->pipe, response, strlen(response), NULL, NULL))
		return 0;
	errno = EIO;
	return -1;
}

static DWORD WINAPI ipc_handle_client(LPVOID param)
{
	struct ipc_handle_client_data *data = param;
	char buffer[1024];
	DWORD offset = 0, length;
	int ret = 0;

	for (;;) {
		if (offset >= ARRAY_SIZE(buffer)) {
			ret = error(_("IPC client message too long: '%.*s'"),
				    (int)offset, buffer);
			break;
		}
		if (!ReadFile(data->pipe,
			      buffer + offset, sizeof(buffer) - offset,
			      &length, NULL)) {
			DWORD err = GetLastError();
error("ReadFile failed; got '%.*s' %ld", (int)length, buffer + offset, length);
			if (err != ERROR_BROKEN_PIPE)
				ret = error(_("read error (IPC) %ld"), err);
			break;
		}

		if (!length || buffer[length - 1] == '\0') {
			ret = data->server->handle_client(data->server,
							  buffer, reply, data);
			if (ret == SIMPLE_IPC_QUIT)
				data->server->active = 0;
			break;
		}

		offset += length;
	}
	FlushFileBuffers(data->pipe);
	DisconnectNamedPipe(data->pipe);
	return ret;
}

int ipc_listen_for_commands(struct ipc_command_listener *server)
{
	struct ipc_handle_client_data data = { server };

	if (initialize_pipe_name(server->path, server->pipe_path,
				 ARRAY_SIZE(server->pipe_path)) < 0)
		return -1;

	if (is_active(server->pipe_path))
		return error(_("server already running at %s"), server->path);

	data.pipe = CreateNamedPipeW(server->pipe_path,
		PIPE_ACCESS_INBOUND | PIPE_ACCESS_OUTBOUND,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_WAIT,
		PIPE_UNLIMITED_INSTANCES, 1024, 1024, 0, NULL);
	if (data.pipe == INVALID_HANDLE_VALUE)
		return error(_("could not create pipe '%s'"),
				server->path);

	server->active = 1;
	while (server->active) {
		int ret;

		if (!ConnectNamedPipe(data.pipe, NULL) &&
		    GetLastError() != ERROR_PIPE_CONNECTED) {
			error(_("could not connect to client (%ld)"),
			      GetLastError());
			continue;
		}

		ret = ipc_handle_client(&data);
		if (ret == SIMPLE_IPC_QUIT)
			break;

		if (ret == -1)
			error("could not handle client");
	}

	CloseHandle(data.pipe);

	return 0;
}

int ipc_send_command(const char *path, const char *message, struct strbuf *answer)
{
	wchar_t wpath[MAX_PATH];
	HANDLE pipe = INVALID_HANDLE_VALUE;
	DWORD mode = PIPE_READMODE_BYTE, length;
	int ret = 0;

	trace2_region_enter("simple-ipc", "send", the_repository);
	trace2_data_string("simple-ipc", the_repository, "path", path);
	trace2_data_string("simple-ipc", the_repository, "message", message);

	if (initialize_pipe_name(path, wpath, ARRAY_SIZE(wpath)) < 0) {
		ret = -1;
		goto leave_send_command;
	}

	for (;;) {
		pipe = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE,
			0, NULL, OPEN_EXISTING, 0, NULL);
		if (pipe != INVALID_HANDLE_VALUE)
			break;
		if (GetLastError() != ERROR_PIPE_BUSY) {
			ret = error(_("could not open %s (%ld)"),
				    path, GetLastError());
			goto leave_send_command;
		}

		if (!WaitNamedPipeW(wpath, 5000)) {
			ret = error(_("timed out: %s"), path);
			goto leave_send_command;
		}
	}

	if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
		ret = error(_("could not switch pipe to byte mode: %s"), path);
		goto leave_send_command;
	}

error("writing '%s'", message);
	if (!WriteFile(pipe, message, strlen(message) + 1, &length, NULL) ||
			length != strlen(message) + 1) {
		ret = error(_("could not send '%s' (%ld)"), message,
			    GetLastError());
		goto leave_send_command;
	}
	FlushFileBuffers(pipe);

error("reading");
	for (;;) {
		size_t alloc = 16384;

		strbuf_grow(answer, alloc);
		if (!ReadFile(pipe, answer->buf + answer->len, alloc, &length,
			      NULL)) {
			DWORD err = GetLastError();
error("err: %ld", err);
			errno = err_win_to_posix(err);
			if (err != ERROR_BROKEN_PIPE &&
			    err != ERROR_PIPE_NOT_CONNECTED)
				ret = error(_("IPC read error: %ld"), err);
			break;
		}

		if (!length)
			break;
		answer->len += length;
	}
	strbuf_setlen(answer, answer->len);
error("answer: '%s'", answer->buf);
	trace2_data_string("simple-ipc", the_repository, "answer", answer->buf);

leave_send_command:
	trace2_region_leave("simple-ipc", "send", the_repository);

	CloseHandle(pipe);
	return ret < 0 ? -1 : 0;
}
#elif !defined(NO_UNIX_SOCKETS)
#include "unix-socket.h"
#include "pkt-line.h"
#include "sigchain.h"

static const char *fsmonitor_listener_path;

int ipc_is_active(const char *path)
{
	struct stat st;

	return !lstat(path, &st) && (st.st_mode & S_IFMT) == S_IFSOCK;
}

static void set_socket_blocking_flag(int fd, int make_nonblocking)
{
	int flags;

	flags = fcntl(fd, F_GETFL, NULL);

	if (flags < 0)
		die(_("fcntl failed"));

	if (make_nonblocking)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;

	if (fcntl(fd, F_SETFL, flags) < 0)
		die(_("fcntl failed"));
}

static int reply(void *reply_data, const char *response)
{
	int fd = *(int *)reply_data;
	return packet_write_fmt_gently(fd, "%s", response);
}

/* in ms */
#define LISTEN_TIMEOUT 50000
#define RESPONSE_TIMEOUT 1000

static void unlink_listener_path(void)
{
	if (fsmonitor_listener_path)
		unlink(fsmonitor_listener_path);
}

int ipc_listen_for_commands(struct ipc_command_listener *listener)
{
	int ret = 0, fd;

	fd = unix_stream_listen(listener->path);
	if (fd < 0)
		return error_errno(_("could not set up socket for %s"),
				   listener->path);

	fsmonitor_listener_path = listener->path;
	atexit(unlink_listener_path);

	trace2_region_enter("simple-ipc", "listen", the_repository);
	while (1) {
		struct pollfd pollfd;
		int result, client_fd;
		int flags;
		char buf[4096];
		int bytes_read;

		/* Wait for a request */
		pollfd.fd = fd;
		pollfd.events = POLLIN;
		result = poll(&pollfd, 1, LISTEN_TIMEOUT);
		if (result < 0) {
			if (errno == EINTR)
				/*
				 * This can lead to an overlong keepalive,
				 * but that is better than a premature exit.
				 */
				continue;
			return error_errno(_("poll() failed"));
		} else if (result == 0)
			/* timeout */
			continue;

		client_fd = accept(fd, NULL, NULL);
		if (client_fd < 0)
			/*
			 * An error here is unlikely -- it probably
			 * indicates that the connecting process has
			 * already dropped the connection.
			 */
			continue;

		/*
		 * Our connection to the client is blocking since a client
		 * can always be killed by SIGINT or similar.
		 */
		set_socket_blocking_flag(client_fd, 0);

		flags = PACKET_READ_GENTLE_ON_EOF |
			PACKET_READ_CHOMP_NEWLINE |
			PACKET_READ_NEVER_DIE;
		bytes_read = packet_read(client_fd, NULL, NULL, buf,
					 sizeof(buf), flags);

		if (bytes_read > 0) {
			/* ensure string termination */
			buf[bytes_read] = 0;
			ret = listener->handle_client(listener, buf, reply,
						      &client_fd);
			if (ret == SIMPLE_IPC_QUIT) {
				close(client_fd);
				break;
			}
		} else {
			/*
			 * No command from client.  Probably it's just
			 * a liveness check or client error.  Just
			 * close up.
			 */
		}
		close(client_fd);
	}

	close(fd);
	return ret == SIMPLE_IPC_QUIT ? 0 : ret;
}

int ipc_send_command(const char *path, const char *message,
		     struct strbuf *answer)
{
	int fd = unix_stream_connect(path);
	int ret = 0;

	trace2_region_enter("simple-ipc", "send", the_repository);
	trace2_data_string("simple-ipc", the_repository, "path", path);
	trace2_data_string("simple-ipc", the_repository, "message", message);

	sigchain_push(SIGPIPE, SIG_IGN);
	if (fd < 0 ||
	    write_packetized_from_buf(message, strlen(message), fd) < 0)
		ret = -1;
	else if (answer) {
		struct pollfd pollfd;

		/* Now wait for a reply */
		pollfd.fd = fd;
		pollfd.events = POLLIN;

		if (poll(&pollfd, 1, RESPONSE_TIMEOUT) <= 0)
			/* No reply or error, giving up */
			ret = -1;
		else {
			int bytes_read;

			if (!answer->alloc)
				strbuf_grow(answer, 4096);

			bytes_read = packet_read(fd, NULL, NULL, answer->buf,
						 answer->alloc,
						 PACKET_READ_GENTLE_ON_EOF |
						 PACKET_READ_CHOMP_NEWLINE |
						 PACKET_READ_NEVER_DIE);

			if (bytes_read < 0)
				ret = -1;
			else
				strbuf_setlen(answer, bytes_read);

		}
		trace2_data_string("simple-ipc", the_repository, "answer",
				   answer->buf);
	}

	trace2_region_leave("simple-ipc", "send", the_repository);

	if (fd >= 0)
		close(fd);
	sigchain_pop(SIGPIPE);
	return ret;
}
#endif
