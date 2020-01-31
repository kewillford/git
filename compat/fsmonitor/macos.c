/*
 * Let Apple's headers declare `isalnum()` first, before
 * Git's headers override it via a constant
 */
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

#include "cache.h"
#include "fsmonitor.h"

static struct strbuf watch_dir = STRBUF_INIT;
static FSEventStreamRef stream;

static void fsevent_callback(ConstFSEventStreamRef streamRef,
			     void *ctx,
			     size_t num_of_events,
			     void * event_paths,
			     const FSEventStreamEventFlags event_flags[],
			     const FSEventStreamEventId event_ids[])
{
	int i;
	char **paths = (char **)event_paths;
	struct fsmonitor_queue_item dummy, *queue = &dummy;
	uint64_t time = getnanotime();
	struct fsmonitor_daemon_state *state = ctx;
	struct strbuf work_str = STRBUF_INIT;

	/* Ensure strictly increasing timestamps */
	if (time <= state->latest_update)
		time = state->latest_update + 1;

	for (i = 0; i < num_of_events; i++) {
		strbuf_addstr(&work_str, paths[i]);
		strbuf_remove(&work_str, 0, watch_dir.len);
		if (strlen(paths[i]) != watch_dir.len)
			strbuf_remove(&work_str, 0, 1);

		if (!strcmp(work_str.buf, ".git") && (event_flags[i] & kFSEventStreamEventFlagItemRemoved)) {
			trace2_printf(".git directory being removed so quitting\n");
			exit(0);
		}

		if ((event_flags[i] & kFSEventStreamEventFlagKernelDropped) ||
		    (event_flags[i] & kFSEventStreamEventFlagUserDropped)) {
			trace2_printf("Dropped event %llu flags %u\n", event_ids[i], event_flags[i]);
			fsmonitor_queue_path(state, &queue, "/", 1, time);
		}

		if (strcmp(work_str.buf, ".git") && !starts_with(work_str.buf, ".git/")) {
			trace2_printf("Change %llu in '%s' flags %u\n", event_ids[i], work_str.buf, event_flags[i]);
			fsmonitor_queue_path(state, &queue,
					     work_str.buf, work_str.len, time);
		} else {
			trace2_printf("Skipping %llu in '%s' flags %u\n", event_ids[i], work_str.buf, event_flags[i]);
		}

		strbuf_reset(&work_str);
	}

	/* Shouldn't happen; defensive programming */
	if (queue == &dummy)
		return;

	pthread_mutex_lock(&state->queue_update_lock);
	if (state->first)
		state->first->previous = dummy.previous;
	dummy.previous->next = state->first;
	state->first = queue;
	state->latest_update = time;
	pthread_mutex_unlock(&state->queue_update_lock);

	strbuf_release(&work_str);
}

struct fsmonitor_daemon_state *fsmonitor_listen(struct fsmonitor_daemon_state *state)
{
	FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagNoDefer |
		kFSEventStreamCreateFlagWatchRoot |
		kFSEventStreamCreateFlagFileEvents;
	CFStringRef watch_path;
	CFArrayRef paths_to_watch;
	FSEventStreamContext ctx = {
		0,
		state,
		NULL,
		NULL,
		NULL
	};

	trace2_region_enter("fsmonitor", "fsevents", the_repository);
	strbuf_addstr(&watch_dir, get_git_work_tree());
	trace2_printf("Start watching: '%s' for fsevents", watch_dir.buf);

	watch_path = CFStringCreateWithCString(NULL, watch_dir.buf, kCFStringEncodingUTF8);
	paths_to_watch = CFArrayCreate(NULL, (const void **)&watch_path, 1, NULL);
	stream = FSEventStreamCreate(NULL, fsevent_callback, &ctx, paths_to_watch,
				     kFSEventStreamEventIdSinceNow, 0.1, flags);
	if (stream == NULL)
		die("Unable to create FSEventStream.");

	FSEventStreamScheduleWithRunLoop(stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	if (!FSEventStreamStart(stream))
		die("Failed to start the FSEventStream");

	pthread_mutex_unlock(&state->initial_mutex);

	CFRunLoopRun();

	trace2_region_leave("fsmonitor", "fsevents", the_repository);
	return state;
}

int fsmonitor_listen_stop(struct fsmonitor_daemon_state *state)
{
	CFRunLoopStop(CFRunLoopGetCurrent());
	FSEventStreamStop(stream);
	FSEventStreamInvalidate(stream);
	FSEventStreamRelease(stream);
	return 0;
}
