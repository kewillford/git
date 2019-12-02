#include "cache.h"
#include "fsmonitor.h"

static int process_entry(struct fsmonitor_daemon_state *state,
			 FILE_NOTIFY_INFORMATION *info,
			 struct fsmonitor_queue_item **queue_pointer,
			 uint64_t time)
{
	char path[32768];
	/* Convert to UTF-8 */
	int len, i;

	len = WideCharToMultiByte(CP_UTF8, 0, info->FileName,
				  info->FileNameLength / sizeof(WCHAR),
				  path, sizeof(path) - 1, NULL, NULL);

	if (len == 0 || len >= ARRAY_SIZE(path) - 1)
		return error("could not convert '%.*S' to UTF-8",
			     (int)(info->FileNameLength / sizeof(WCHAR)),
			     info->FileName);

	path[len] = '\0'; /* NUL-terminate */
	for (i = 0; i < len; i++) /* convert backslashes to forward slashes */
		if (path[i] == '\\')
			path[i] = '/';

	if (fsmonitor_queue_path(state, queue_pointer, path, len, time) < 0)
		return error("could not queue '%s'; exiting", path);

	return 0;
}

struct fsmonitor_daemon_state *fsmonitor_listen(struct fsmonitor_daemon_state *state)
{
	HANDLE dir;
	char buffer[65536 * sizeof(wchar_t)], *p;
	DWORD desired_access = FILE_LIST_DIRECTORY;
	DWORD share_mode =
		FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE;
	DWORD count = 0;

	dir = CreateFileW(L".", desired_access, share_mode, NULL, OPEN_EXISTING,
			  FILE_FLAG_BACKUP_SEMANTICS, NULL);
	pthread_mutex_unlock(&state->initial_mutex);

	for (;;) {
		struct fsmonitor_queue_item dummy, *queue = &dummy;
		uint64_t time = getnanotime();

		/* Ensure strictly increasing timestamps */
		if (time <= state->latest_update)
			time = state->latest_update + 1;

		if (!ReadDirectoryChangesW(dir, buffer, sizeof(buffer), TRUE,
					   FILE_NOTIFY_CHANGE_FILE_NAME |
					   FILE_NOTIFY_CHANGE_DIR_NAME |
					   FILE_NOTIFY_CHANGE_ATTRIBUTES |
					   FILE_NOTIFY_CHANGE_SIZE |
					   FILE_NOTIFY_CHANGE_LAST_WRITE |
					   FILE_NOTIFY_CHANGE_CREATION,
					   &count, NULL, NULL)) {
			error("Reading Directory Change failed");
			continue;
		}

		p = buffer;
		for (;;) {
			FILE_NOTIFY_INFORMATION *info = (void *)p;

			if (process_entry(state, info, &queue, time) < 0) {
				CloseHandle(dir);
				state->error_code = -1;
				return state;
			}

			if (!info->NextEntryOffset)
				break;
			p += info->NextEntryOffset;
		}

		/* Shouldn't happen; defensive programming */
		if (queue == &dummy)
			continue;

		pthread_mutex_lock(&state->queue_update_lock);
		if (state->first)
			state->first->previous = dummy.previous;
		dummy.previous->next = state->first;
		state->first = queue;
		state->latest_update = time;
		pthread_mutex_unlock(&state->queue_update_lock);
	}

	return state;
}
