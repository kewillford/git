#include "cache.h"
#include "config.h"
#include "dir.h"
#include "ewah/ewok.h"
#include "fsmonitor.h"
#include "run-command.h"
#include "strbuf.h"

#define INDEX_EXTENSION_VERSION	(1)
#define HOOK_INTERFACE_VERSION	(1)

struct trace_key trace_fsmonitor = TRACE_KEY_INIT(FSMONITOR);

static void fsmonitor_ewah_callback(size_t pos, void *is)
{
	struct index_state *istate = (struct index_state *)is;
	struct cache_entry *ce;

	if (pos >= istate->cache_nr)
		BUG("fsmonitor_dirty has more entries than the index (%"PRIuMAX" >= %u)",
		    (uintmax_t)pos, istate->cache_nr);

	ce = istate->cache[pos];
	ce->ce_flags &= ~CE_FSMONITOR_VALID;
}

int read_fsmonitor_extension(struct index_state *istate, const void *data,
	unsigned long sz)
{
	const char *index = data;
	uint32_t hdr_version;
	uint32_t ewah_size;
	struct ewah_bitmap *fsmonitor_dirty;
	int ret;

	if (sz < sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t))
		return error("corrupt fsmonitor extension (too short)");

	hdr_version = get_be32(index);
	index += sizeof(uint32_t);
	if (hdr_version != INDEX_EXTENSION_VERSION)
		return error("bad fsmonitor version %d", hdr_version);

	istate->fsmonitor_last_update = get_be64(index);
	index += sizeof(uint64_t);

	ewah_size = get_be32(index);
	index += sizeof(uint32_t);

	fsmonitor_dirty = ewah_new();
	ret = ewah_read_mmap(fsmonitor_dirty, index, ewah_size);
	if (ret != ewah_size) {
		ewah_free(fsmonitor_dirty);
		return error("failed to parse ewah bitmap reading fsmonitor index extension");
	}
	istate->fsmonitor_dirty = fsmonitor_dirty;

	if (!istate->split_index &&
	    istate->fsmonitor_dirty->bit_size > istate->cache_nr)
		BUG("fsmonitor_dirty has more entries than the index (%"PRIuMAX" > %u)",
		    (uintmax_t)istate->fsmonitor_dirty->bit_size, istate->cache_nr);

	trace_printf_key(&trace_fsmonitor, "read fsmonitor extension successful");
	return 0;
}

void fill_fsmonitor_bitmap(struct index_state *istate)
{
	unsigned int i, skipped = 0;
	istate->fsmonitor_dirty = ewah_new();
	for (i = 0; i < istate->cache_nr; i++) {
		if (istate->cache[i]->ce_flags & CE_REMOVE)
			skipped++;
		else if (!(istate->cache[i]->ce_flags & CE_FSMONITOR_VALID))
			ewah_set(istate->fsmonitor_dirty, i - skipped);
	}
}

void write_fsmonitor_extension(struct strbuf *sb, struct index_state *istate)
{
	uint32_t hdr_version;
	uint64_t tm;
	uint32_t ewah_start;
	uint32_t ewah_size = 0;
	int fixup = 0;

	if (!istate->split_index &&
	    istate->fsmonitor_dirty->bit_size > istate->cache_nr)
		BUG("fsmonitor_dirty has more entries than the index (%"PRIuMAX" > %u)",
		    (uintmax_t)istate->fsmonitor_dirty->bit_size, istate->cache_nr);

	put_be32(&hdr_version, INDEX_EXTENSION_VERSION);
	strbuf_add(sb, &hdr_version, sizeof(uint32_t));

	put_be64(&tm, istate->fsmonitor_last_update);
	strbuf_add(sb, &tm, sizeof(uint64_t));
	fixup = sb->len;
	strbuf_add(sb, &ewah_size, sizeof(uint32_t)); /* we'll fix this up later */

	ewah_start = sb->len;
	ewah_serialize_strbuf(istate->fsmonitor_dirty, sb);
	ewah_free(istate->fsmonitor_dirty);
	istate->fsmonitor_dirty = NULL;

	/* fix up size field */
	put_be32(&ewah_size, sb->len - ewah_start);
	memcpy(sb->buf + fixup, &ewah_size, sizeof(uint32_t));

	trace_printf_key(&trace_fsmonitor, "write fsmonitor extension successful");
}

/*
 * Call the query-fsmonitor hook passing the time of the last saved results.
 */
static int query_fsmonitor(int version, uint64_t last_update, struct strbuf *query_result)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	if (!core_fsmonitor)
		return -1;

	argv_array_push(&cp.args, core_fsmonitor);
	argv_array_pushf(&cp.args, "%d", version);
	argv_array_pushf(&cp.args, "%" PRIuMAX, (uintmax_t)last_update);
	cp.use_shell = 1;
	cp.dir = get_git_work_tree();

	return capture_command(&cp, query_result, 1024);
}

static void fsmonitor_refresh_callback(struct index_state *istate, const char *name)
{
	int pos = index_name_pos(istate, name, strlen(name));

	if (pos >= 0) {
		struct cache_entry *ce = istate->cache[pos];
		ce->ce_flags &= ~CE_FSMONITOR_VALID;
	}

	/*
	 * Mark the untracked cache dirty even if it wasn't found in the index
	 * as it could be a new untracked file.
	 */
	trace_printf_key(&trace_fsmonitor, "fsmonitor_refresh_callback '%s'", name);
	untracked_cache_invalidate_path(istate, name, 0);
}

void refresh_fsmonitor(struct index_state *istate)
{
	struct strbuf query_result = STRBUF_INIT;
	int query_success = 0;
	size_t bol; /* beginning of line */
	uint64_t last_update;
	char *buf;
	unsigned int i;

	if (!core_fsmonitor || istate->fsmonitor_has_run_once)
		return;
	istate->fsmonitor_has_run_once = 1;

	trace_printf_key(&trace_fsmonitor, "refresh fsmonitor");
	/*
	 * This could be racy so save the date/time now and query_fsmonitor
	 * should be inclusive to ensure we don't miss potential changes.
	 */
	last_update = getnanotime();

	/*
	 * If we have a last update time, call query_fsmonitor for the set of
	 * changes since that time, else assume everything is possibly dirty
	 * and check it all.
	 */
	if (istate->fsmonitor_last_update) {
		query_success = !query_fsmonitor(HOOK_INTERFACE_VERSION,
			istate->fsmonitor_last_update, &query_result);
		trace_performance_since(last_update, "fsmonitor process '%s'", core_fsmonitor);
		trace_printf_key(&trace_fsmonitor, "fsmonitor process '%s' returned %s",
			core_fsmonitor, query_success ? "success" : "failure");
	}

	/* a fsmonitor process can return '/' to indicate all entries are invalid */
	if (query_success && query_result.buf[0] != '/') {
		/* Mark all entries returned by the monitor as dirty */
		buf = query_result.buf;
		bol = 0;
		for (i = 0; i < query_result.len; i++) {
			if (buf[i] != '\0')
				continue;
			fsmonitor_refresh_callback(istate, buf + bol);
			bol = i + 1;
		}
		if (bol < query_result.len)
			fsmonitor_refresh_callback(istate, buf + bol);

		/* Now mark the untracked cache for fsmonitor usage */
		if (istate->untracked)
			istate->untracked->use_fsmonitor = 1;
	} else {

		/* We only want to run the post index changed hook if we've actually changed entries, so keep track
		 * if we actually changed entries or not */
		int is_cache_changed = 0;
		/* Mark all entries invalid */
		for (i = 0; i < istate->cache_nr; i++) {
			if (istate->cache[i]->ce_flags & CE_FSMONITOR_VALID) {
				is_cache_changed = 1;
				istate->cache[i]->ce_flags &= ~CE_FSMONITOR_VALID;
			}
		}

		/* If we're going to check every file, ensure we save the results */
		if (is_cache_changed)
			istate->cache_changed |= FSMONITOR_CHANGED;

		if (istate->untracked)
			istate->untracked->use_fsmonitor = 0;
	}
	strbuf_release(&query_result);

	/* Now that we've updated istate, save the last_update time */
	istate->fsmonitor_last_update = last_update;
}

void add_fsmonitor(struct index_state *istate)
{
	unsigned int i;

	if (!istate->fsmonitor_last_update) {
		trace_printf_key(&trace_fsmonitor, "add fsmonitor");
		istate->cache_changed |= FSMONITOR_CHANGED;
		istate->fsmonitor_last_update = getnanotime();

		/* reset the fsmonitor state */
		for (i = 0; i < istate->cache_nr; i++)
			istate->cache[i]->ce_flags &= ~CE_FSMONITOR_VALID;

		/* reset the untracked cache */
		if (istate->untracked) {
			add_untracked_cache(istate);
			istate->untracked->use_fsmonitor = 1;
		}

		/* Update the fsmonitor state */
		refresh_fsmonitor(istate);
	}
}

void remove_fsmonitor(struct index_state *istate)
{
	if (istate->fsmonitor_last_update) {
		trace_printf_key(&trace_fsmonitor, "remove fsmonitor");
		istate->cache_changed |= FSMONITOR_CHANGED;
		istate->fsmonitor_last_update = 0;
	}
}

void tweak_fsmonitor(struct index_state *istate)
{
	unsigned int i;
	int fsmonitor_enabled = git_config_get_fsmonitor();

	if (istate->fsmonitor_dirty) {
		if (fsmonitor_enabled) {
			/* Mark all entries valid */
			for (i = 0; i < istate->cache_nr; i++) {
				istate->cache[i]->ce_flags |= CE_FSMONITOR_VALID;
			}

			/* Mark all previously saved entries as dirty */
			if (istate->fsmonitor_dirty->bit_size > istate->cache_nr)
				BUG("fsmonitor_dirty has more entries than the index (%"PRIuMAX" > %u)",
				    (uintmax_t)istate->fsmonitor_dirty->bit_size, istate->cache_nr);
			ewah_each_bit(istate->fsmonitor_dirty, fsmonitor_ewah_callback, istate);

			refresh_fsmonitor(istate);
		}

		ewah_free(istate->fsmonitor_dirty);
		istate->fsmonitor_dirty = NULL;
	}

	switch (fsmonitor_enabled) {
	case -1: /* keep: do nothing */
		break;
	case 0: /* false */
		remove_fsmonitor(istate);
		break;
	case 1: /* true */
		add_fsmonitor(istate);
		break;
	default: /* unknown value: do nothing */
		break;
	}
}

#ifdef HAVE_FSMONITOR_DAEMON_BACKEND
#include "simple-ipc.h"

GIT_PATH_FUNC(git_path_fsmonitor, "fsmonitor")

int fsmonitor_query_daemon(uint64_t since, struct strbuf *answer)
{
	struct strbuf command = STRBUF_INIT;
	int ret = 0;

	strbuf_addf(&command, "%ld %" PRIuMAX, FSMONITOR_VERSION,
		    (uintmax_t)since);
	ret = ipc_send_command(git_path_fsmonitor(),
				command.buf, answer);
	strbuf_release(&command);
	return ret;
}

int fsmonitor_daemon_is_running(void)
{
	return ipc_is_active(git_path_fsmonitor());
}

/* Let's spin up a new server, returning when it is listening */
int fsmonitor_spawn_daemon(void)
{
#ifndef GIT_WINDOWS_NATIVE
	const char *args[] = { "fsmonitor--daemon", "--start", NULL };

	return run_command_v_opt_tr2(args, RUN_COMMAND_NO_STDIN | RUN_GIT_CMD,
				    "fsmonitor");
#else
	const char *args[] = { "git", "fsmonitor--daemon", "--run", NULL };
	int in = open("/dev/null", O_RDONLY);
	int out = open("/dev/null", O_WRONLY);
	int ret = 0;
	pid_t pid = mingw_spawnvpe("git", args, NULL, NULL, in, out, out);
	HANDLE process;

	if (pid < 0)
		ret = error(_("could not spawn the fsmonitor daemon"));

	close(in);
	close(out);

	process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
	if (!process)
		return error(_("could not spawn fsmonitor--daemon"));

	/* poll is_running() */
	while (!ret && !fsmonitor_daemon_is_running()) {
		DWORD exit_code;

		if (!GetExitCodeProcess(process, &exit_code)) {
			CloseHandle(process);
			return error(_("could not query status of spawned "
				       "fsmonitor--daemon"));
		}

		if (exit_code != STILL_ACTIVE) {
			CloseHandle(process);
			return error(_("fsmonitor--daemon --run stopped; "
				       "exit code: %ld"), exit_code);
		}

		sleep_millisec(50);
	}

	return ret;
#endif
}
#endif
