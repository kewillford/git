/*
 * built-in fsmonitor daemon
 *
 * Monitor filesystem changes to update the Git index intelligently.
 *
 * Copyright (c) 2019 Johannes Schindelin
 */

#include "builtin.h"
#include "parse-options.h"
#include "fsmonitor.h"
#include "simple-ipc.h"

static const char * const builtin_fsmonitor__daemon_usage[] = {
	N_("git fsmonitor--daemon [<options>]"),
	NULL
};

#ifndef HAVE_FSMONITOR_DAEMON_BACKEND
#define FSMONITOR_DAEMON_IS_SUPPORTED 0

static int fsmonitor_run_daemon(void)
{
	die(_("no native fsmonitor daemon available"));
}
#else
#define FSMONITOR_DAEMON_IS_SUPPORTED 1

struct ipc_data {
	struct ipc_command_listener data;
	struct fsmonitor_daemon_state *state;
};

static int handle_client(struct ipc_command_listener *data,
			 const char *command,
			 int (*reply)(void *reply_data, const char *message),
			 void *reply_data)
{
	struct fsmonitor_daemon_state *state = ((struct ipc_data *)data)->state;
	unsigned long version;
	uintmax_t since;
	char *p;
	struct fsmonitor_queue_item *queue;

	version = strtoul(command, &p, 10);
	if (version != FSMONITOR_VERSION) {
		reply(reply_data, "/");
		error(_("fsmonitor: unhandled version (%lu, command: %s)"),
		      version, command);
		return -1;
	}
	while (isspace(*p))
		p++;
	since = strtoumax(p, &p, 10);
	/*
	 * TODO: set the initial timestamp properly,
	 * return "/" if since <= timestamp
	 */
	if (!since || *p) {
		reply(reply_data, "/");
		error(_("fsmonitor: %s (%" PRIuMAX", command: %s, rest %s)"),
		      *p ? "extra stuff" : "incorrect/early timestamp",
		      since, command, p);
		return -1;
	}

	pthread_mutex_lock(&state->queue_update_lock);
	queue = state->first;
	pthread_mutex_unlock(&state->queue_update_lock);

	/* TODO: use a hashmap to avoid reporting duplicates */
	while (queue && queue->time >= since) {
		/* write the path, followed by a NUL */
		if (reply(reply_data, queue->path->path) < 0)
			break;
		queue = queue->next;
	}

	return 0;
}

static int paths_cmp(const void *data, const struct hashmap_entry *he1,
		     const struct hashmap_entry *he2, const void *keydata)
{
	const struct fsmonitor_path *a =
		container_of(he1, const struct fsmonitor_path, entry);
	const struct fsmonitor_path *b =
		container_of(he2, const struct fsmonitor_path, entry);

	return strcmp(a->path, keydata ? keydata : b->path);
}

#define FNV32_BASE ((unsigned int) 0x811c9dc5)
#define FNV32_PRIME ((unsigned int) 0x01000193)

int fsmonitor_queue_path(struct fsmonitor_daemon_state *state,
			 struct fsmonitor_queue_item **queue,
			 const char *path, uint64_t time)
{
	struct fsmonitor_path lookup, *e;
	struct fsmonitor_queue_item *item;

	hashmap_entry_init(&lookup.entry, strhash(path));
	lookup.path = path;
	e = hashmap_get_entry(&state->paths, &lookup, entry, NULL);

	if (!e) {
		FLEXPTR_ALLOC_STR(e, path, path);
		hashmap_put(&state->paths, &e->entry);
	}

	item = xmalloc(sizeof(*item));
	item->path = e;
	item->time = time;
	item->previous = NULL;
	item->next = *queue;
	(*queue)->previous = item;
	*queue = item;

	return 0;
}

static int fsmonitor_run_daemon(void)
{
	pthread_t thread;
	struct fsmonitor_daemon_state state = { { 0 } };
	struct ipc_data ipc_data = {
		.data = {
			.path = git_path_fsmonitor(),
			.handle_client = handle_client,
		},
		.state = &state,
	};

	hashmap_init(&state.paths, paths_cmp, NULL, 0);
	pthread_mutex_init(&state.queue_update_lock, NULL);
	pthread_mutex_init(&state.initial_mutex, NULL);
	pthread_mutex_lock(&state.initial_mutex);

	if (pthread_create(&thread, NULL,
			   (void *(*)(void *)) fsmonitor_listen, &state) < 0)
		return error(_("could not start fsmonitor listener thread"));

	/* wait for the thread to signal that it is ready */
	pthread_mutex_lock(&state.initial_mutex);
	pthread_mutex_unlock(&state.initial_mutex);

	return ipc_listen_for_commands(&ipc_data.data);
}
#endif

int cmd_fsmonitor__daemon(int argc, const char **argv, const char *prefix)
{
	enum daemon_mode { IS_SUPPORTED = 0 } mode = IS_SUPPORTED;
	struct option options[] = {
		OPT_CMDMODE(0, "is-supported", &mode,
			    N_("determine internal fsmonitor on this platform"),
			    IS_SUPPORTED),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	argc = parse_options(argc, argv, prefix, options,
			     builtin_fsmonitor__daemon_usage, 0);
	if (argc != 0)
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	if (mode == IS_SUPPORTED)
		return !FSMONITOR_DAEMON_IS_SUPPORTED;

	return !!fsmonitor_run_daemon();
}
