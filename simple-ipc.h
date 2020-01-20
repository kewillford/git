#ifndef GIT_IPC_H
#define GIT_IPC_H

#if defined(GIT_WINDOWS_NATIVE) || !defined(NO_UNIX_SOCKETS)
#define SUPPORTS_SIMPLE_IPC
#endif

/* Return this from `handle_client()` to stop listening. */
#define SIMPLE_IPC_QUIT -2

struct ipc_command_listener {
	/* The path to identify the server; typically lives inside .git/ */
	const char *path;
#ifdef GIT_WINDOWS_NATIVE
	wchar_t pipe_path[MAX_PATH];
#endif
	int active;
	int (*handle_client)(struct ipc_command_listener *data,
			     const char *command,
			     int (*reply)(void *reply_data,
					  const char *answer, size_t len),
			     void *reply_data);
};

/*
 * Open an Inter-Process Communication server.
 *
 * These two functions implement very simplistic communication between two
 * processes. The communication channel is identified by a `path` that may or
 * may not be a _real_ path.
 *
 * The communication is very simple: the client sends a plain-text message and
 * the server sends back a plain-text answer, then the communication is closed.
 */
int ipc_listen_for_commands(struct ipc_command_listener *data);
int ipc_send_command(const char *path, const char *message,
		     struct strbuf *answer);
int ipc_is_active(const char *path);

#endif
