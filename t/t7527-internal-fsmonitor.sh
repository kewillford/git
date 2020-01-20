#!/bin/sh

test_description='internal file system watcher'

. ./test-lib.sh

git fsmonitor--daemon --is-supported || {
	skip_all="The internal FSMonitor is not supported on this platform"
	test_done
}

test_expect_success 'can start and stop the daemon' '
	test_when_finished \
		"test_might_fail git -C test fsmonitor--daemon --stop" &&
	git init test &&
	(
		cd test &&
		: start the daemon implicitly by querying it &&
		git fsmonitor--daemon --query 1 0 >actual &&
		git fsmonitor--daemon --is-running &&
		nul_to_q <actual >actual.filtered &&
		printf 0Q/Q >expect &&
		test_cmp expect actual.filtered
	)
'

test_done
