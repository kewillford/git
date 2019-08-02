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

	if (!WriteFile(pipe, message, strlen(message) + 1, &length, NULL) ||
			length != strlen(message) + 1) {
		ret = error(_("could not send '%s' (%ld)"), message,
			    GetLastError());
		goto leave_send_command;
	}
	FlushFileBuffers(pipe);

	for (;;) {
		size_t alloc = 16384;

		strbuf_grow(answer, alloc);
		if (!ReadFile(pipe, answer->buf + answer->len, alloc, &length,
			      NULL)) {
			DWORD err = GetLastError();
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

leave_send_command:
	CloseHandle(pipe);
	return ret < 0 ? -1 : 0;
}
#endif
