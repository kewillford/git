#!/bin/sh

test_description='simple command server'

. ./test-lib.sh

test_lazy_prereq SUPPORTS_SIMPLE_IPC '
	test-tool simple-ipc SUPPORTS_SIMPLE_IPC
'

test_expect_success SUPPORTS_SIMPLE_IPC 'simple command server' '
	{ test-tool simple-ipc daemon & } &&

	test-tool simple-ipc is-active &&

	test-tool simple-ipc send ping >actual &&
	echo pong >expect &&
	test_cmp expect actual &&

	test-tool simple-ipc send quit &&
	test_must_fail test-tool simple-ipc send ping
'

test_done

