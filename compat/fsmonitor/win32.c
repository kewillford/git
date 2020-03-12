#include "cache.h"
#include "fsmonitor.h"


static int normalize_path(FILE_NOTIFY_INFORMATION *info, struct strbuf *normalized_path)
{
	/* Convert to UTF-8 */
	int len;

	strbuf_reset(normalized_path);
	strbuf_grow(normalized_path, 32768);
	len = WideCharToMultiByte(CP_UTF8, 0, info->FileName,
				  info->FileNameLength / sizeof(WCHAR),
				  normalized_path->buf, strbuf_avail(normalized_path) - 1, NULL, NULL);

	if (len == 0 || len >= 32768 - 1)
		return error("could not convert '%.*S' to UTF-8",
			     (int)(info->FileNameLength / sizeof(WCHAR)),
			     info->FileName);

	strbuf_setlen(normalized_path, len);
	return strbuf_normalize_path(normalized_path);
}

static int process_entry(struct fsmonitor_daemon_state *state,
			 struct strbuf *path,
			 struct fsmonitor_queue_item **queue_pointer,
			 uint64_t time)
{
	if (fsmonitor_queue_path(state, queue_pointer, path->buf, path->len, time) < 0)
		return error("could not queue '%s'; exiting", path->buf);

	return 0;
}

int fsmonitor_listen_stop(struct fsmonitor_daemon_state *state)
{
	if (!TerminateThread(state->watcher_thread.handle, 1))
		return -1;

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
	int i;

	dir = CreateFileW(L".", desired_access, share_mode, NULL, OPEN_EXISTING,
			  FILE_FLAG_BACKUP_SEMANTICS, NULL);
	pthread_mutex_unlock(&state->initial_mutex);

	for (;;) {
		struct fsmonitor_queue_item dummy, *queue = &dummy;
		struct strbuf path = STRBUF_INIT;
		struct string_list cookie_release_list = STRING_LIST_INIT_DUP;
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
			normalize_path(info, &path);

			if (info->Action == FILE_ACTION_REMOVED &&
			    path.len == 4 &&
			    !strcmp(path.buf, ".git")) {
				CloseHandle(dir);
				/* force-quit */
				exit(0);
			}

			if (info->Action != FILE_ACTION_REMOVED &&
			    path.len > 4 && starts_with(path.buf, ".git/") &&
			    starts_with(path.buf + 5, FSMONITOR_COOKIE_PREFIX)) {
				string_list_append(&cookie_release_list, path.buf + 5);
			} else if (strcmp(path.buf, ".git") && !starts_with(path.buf, ".git/")) {
				if (process_entry(state, &path, &queue, time) < 0) {
					CloseHandle(dir);
					state->error_code = -1;
					return state;
				}
			}

			if (!info->NextEntryOffset)
				break;
			p += info->NextEntryOffset;
		}

		/* Only update the queue if it changed */
		if (queue != &dummy) {
			pthread_mutex_lock(&state->queue_update_lock);
			if (state->first)
				state->first->previous = dummy.previous;
			dummy.previous->next = state->first;
			state->first = queue;
			state->latest_update = time;
			pthread_mutex_unlock(&state->queue_update_lock);
		}

		for (i = 0; i < cookie_release_list.nr; i++) {
			fsmonitor_cookie_seen_trigger(state, cookie_release_list.items[i].string);
		}

		string_list_clear(&cookie_release_list, 0);
		strbuf_release(&path);
	}

	return state;
}
