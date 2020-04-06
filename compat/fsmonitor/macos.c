#if defined(__GNUC__)
/*
 * It is possible to #include CoreFoundation/CoreFoundation.h when compiling
 * with clang, but not with GCC as of time of writing.
 *
 * See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=93082 for details.
 */
typedef unsigned int FSEventStreamCreateFlags;
#define kFSEventStreamEventFlagNone               0x00000000
#define kFSEventStreamEventFlagMustScanSubDirs    0x00000001
#define kFSEventStreamEventFlagUserDropped        0x00000002
#define kFSEventStreamEventFlagKernelDropped      0x00000004
#define kFSEventStreamEventFlagEventIdsWrapped    0x00000008
#define kFSEventStreamEventFlagHistoryDone        0x00000010
#define kFSEventStreamEventFlagRootChanged        0x00000020
#define kFSEventStreamEventFlagMount              0x00000040
#define kFSEventStreamEventFlagUnmount            0x00000080
#define kFSEventStreamEventFlagItemCreated        0x00000100
#define kFSEventStreamEventFlagItemRemoved        0x00000200
#define kFSEventStreamEventFlagItemInodeMetaMod   0x00000400
#define kFSEventStreamEventFlagItemRenamed        0x00000800
#define kFSEventStreamEventFlagItemModified       0x00001000
#define kFSEventStreamEventFlagItemFinderInfoMod  0x00002000
#define kFSEventStreamEventFlagItemChangeOwner    0x00004000
#define kFSEventStreamEventFlagItemXattrMod       0x00008000
#define kFSEventStreamEventFlagItemIsFile         0x00010000
#define kFSEventStreamEventFlagItemIsDir          0x00020000
#define kFSEventStreamEventFlagItemIsSymlink      0x00040000
#define kFSEventStreamEventFlagOwnEvent           0x00080000
#define kFSEventStreamEventFlagItemIsHardlink     0x00100000
#define kFSEventStreamEventFlagItemIsLastHardlink 0x00200000
#define kFSEventStreamEventFlagItemCloned         0x00400000

typedef struct __FSEventStream *FSEventStreamRef;
typedef const FSEventStreamRef ConstFSEventStreamRef;

typedef unsigned int CFStringEncoding;
#define kCFStringEncodingUTF8 0x08000100

typedef const struct __CFString *CFStringRef;
typedef const struct __CFArray *CFArrayRef;
typedef const struct __CFRunLoop *CFRunLoopRef;

struct FSEventStreamContext {
    long long version;
    void *cb_data, *retain, *release, *copy_description;
};

typedef struct FSEventStreamContext FSEventStreamContext;
typedef unsigned int FSEventStreamEventFlags;
#define kFSEventStreamCreateFlagNoDefer 0x02
#define kFSEventStreamCreateFlagWatchRoot 0x04
#define kFSEventStreamCreateFlagFileEvents 0x10

typedef unsigned long long FSEventStreamEventId;
#define kFSEventStreamEventIdSinceNow 0xFFFFFFFFFFFFFFFFULL

typedef void (*FSEventStreamCallback)(ConstFSEventStreamRef streamRef,
				      void *context,
				      __SIZE_TYPE__ num_of_events,
				      void *event_paths,
				      const FSEventStreamEventFlags event_flags[],
				      const FSEventStreamEventId event_ids[]);
typedef double CFTimeInterval;
FSEventStreamRef FSEventStreamCreate(void *allocator,
				     FSEventStreamCallback callback,
				     FSEventStreamContext *context,
				     CFArrayRef paths_to_watch,
				     FSEventStreamEventId since_when,
				     CFTimeInterval latency,
				     FSEventStreamCreateFlags flags);
CFStringRef CFStringCreateWithCString(void *allocator, const char *string,
				      CFStringEncoding encoding);
CFArrayRef CFArrayCreate(void *allocator, const void **items, long long count,
			 void *callbacks);
void CFRunLoopRun(void);
void CFRunLoopStop(CFRunLoopRef run_loop);
CFRunLoopRef CFRunLoopGetCurrent(void);
extern CFStringRef kCFRunLoopDefaultMode;
void FSEventStreamScheduleWithRunLoop(FSEventStreamRef stream,
				      CFRunLoopRef run_loop,
				      CFStringRef run_loop_mode);
unsigned char FSEventStreamStart(FSEventStreamRef stream);
void FSEventStreamStop(FSEventStreamRef stream);
void FSEventStreamInvalidate(FSEventStreamRef stream);
void FSEventStreamRelease(FSEventStreamRef stream);
#else
/*
 * Let Apple's headers declare `isalnum()` first, before
 * Git's headers override it via a constant
 */
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#endif

#include "cache.h"
#include "fsmonitor.h"

static struct strbuf watch_dir = STRBUF_INIT;
static FSEventStreamRef stream;

static void trace2_message(const char *key, const char *message)
{
	trace2_data_string("fsmonitor-macos", the_repository, key, message);
}

static void log_flags_set(struct strbuf *dir, const FSEventStreamEventFlags flag) {
	struct strbuf msg = STRBUF_INIT;
	strbuf_addf(&msg, "%s flags: %u = ", dir->buf, flag);

	if (flag & kFSEventStreamEventFlagMustScanSubDirs)
		strbuf_addstr(&msg, "MustScanSubDirs|");
	if (flag & kFSEventStreamEventFlagUserDropped)
		strbuf_addstr(&msg, "UserDropped|");
	if (flag & kFSEventStreamEventFlagKernelDropped)
		strbuf_addstr(&msg, "KernelDropped|");
	if (flag & kFSEventStreamEventFlagEventIdsWrapped)
		strbuf_addstr(&msg, "EventIdsWrapped|");
	if (flag & kFSEventStreamEventFlagHistoryDone)
		strbuf_addstr(&msg, "HistoryDone|");
	if (flag & kFSEventStreamEventFlagRootChanged)
		strbuf_addstr(&msg, "RootChanged|");
	if (flag & kFSEventStreamEventFlagMount)
		strbuf_addstr(&msg, "Mount|");
	if (flag & kFSEventStreamEventFlagUnmount)
		strbuf_addstr(&msg, "Unmount|");
	if (flag & kFSEventStreamEventFlagItemChangeOwner)
		strbuf_addstr(&msg, "ItemChangeOwner|");
	if (flag & kFSEventStreamEventFlagItemCreated)
		strbuf_addstr(&msg, "ItemCreated|");
	if (flag & kFSEventStreamEventFlagItemFinderInfoMod)
		strbuf_addstr(&msg, "ItemFinderInfoMod|");
	if (flag & kFSEventStreamEventFlagItemInodeMetaMod)
		strbuf_addstr(&msg, "ItemInodeMetaMod|");
	if (flag & kFSEventStreamEventFlagItemIsDir)
		strbuf_addstr(&msg, "ItemIsDir|");
	if (flag & kFSEventStreamEventFlagItemIsFile)
		strbuf_addstr(&msg, "ItemIsFile|");
	if (flag & kFSEventStreamEventFlagItemIsHardlink)
		strbuf_addstr(&msg, "ItemIsHardlink|");
	if (flag & kFSEventStreamEventFlagItemIsLastHardlink)
		strbuf_addstr(&msg, "ItemIsLastHardlink|");
	if (flag & kFSEventStreamEventFlagItemIsSymlink)
		strbuf_addstr(&msg, "ItemIsSymlink|");
	if (flag & kFSEventStreamEventFlagItemModified)
		strbuf_addstr(&msg, "ItemModified|");
	if (flag & kFSEventStreamEventFlagItemRemoved)
		strbuf_addstr(&msg, "ItemRemoved|");
	if (flag & kFSEventStreamEventFlagItemRenamed)
		strbuf_addstr(&msg, "ItemRenamed|");
	if (flag & kFSEventStreamEventFlagItemXattrMod)
		strbuf_addstr(&msg, "ItemXattrMod|");
	if (flag & kFSEventStreamEventFlagOwnEvent)
		strbuf_addstr(&msg, "OwnEvent|");
	if (flag & kFSEventStreamEventFlagItemCloned)
		strbuf_addstr(&msg, "ItemCloned|");

	trace2_message("fsevent", msg.buf);
}

static void fsevent_callback(ConstFSEventStreamRef streamRef,
			     void *ctx,
			     size_t num_of_events,
			     void * event_paths,
			     const FSEventStreamEventFlags event_flags[],
			     const FSEventStreamEventId event_ids[])
{
	int i;
	struct stat st;
	char **paths = (char **)event_paths;
	struct fsmonitor_queue_item dummy, *queue = &dummy;
	uint64_t time = getnanotime();
	struct fsmonitor_daemon_state *state = ctx;
	struct strbuf work_str = STRBUF_INIT;
	struct string_list cookie_release_list = STRING_LIST_INIT_DUP;

	/* Ensure strictly increasing timestamps */
	if (time <= state->latest_update)
		time = state->latest_update + 1;

	for (i = 0; i < num_of_events; i++) {
		strbuf_addstr(&work_str, paths[i]);
		strbuf_remove(&work_str, 0, watch_dir.len);
		if (strlen(paths[i]) != watch_dir.len)
			strbuf_remove(&work_str, 0, 1);

		if ((event_flags[i] & kFSEventStreamEventFlagRootChanged) ||
		    ((event_flags[i] & kFSEventStreamEventFlagItemRemoved) &&
		    !strcmp(work_str.buf, ".git") &&
		    lstat(paths[i], &st))) {
			trace2_message("message", ".git directory being removed so quitting.");
			exit(0);
		}

		if ((event_flags[i] & kFSEventStreamEventFlagKernelDropped) ||
		    (event_flags[i] & kFSEventStreamEventFlagUserDropped)) {
			trace2_message("message", "Dropped event");
			fsmonitor_queue_path(state, &queue, "/", 1, time);
		}

		if (strcmp(work_str.buf, ".git") && !starts_with(work_str.buf, ".git/")) {
			log_flags_set(&work_str, event_flags[i]);
			fsmonitor_queue_path(state, &queue,
					     work_str.buf, work_str.len, time);
		} else if (starts_with(work_str.buf, ".git/") &&
			   starts_with(work_str.buf + 5, FSMONITOR_COOKIE_PREFIX) &&
			   !(event_flags[i] & kFSEventStreamEventFlagItemRemoved)) {
			string_list_append(&cookie_release_list, work_str.buf + 5);
		}

		strbuf_reset(&work_str);
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
