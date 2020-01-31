#!/bin/sh

test_description='internal file system watcher'

. ./test-lib.sh

git fsmonitor--daemon --is-supported || {
	skip_all="The internal FSMonitor is not supported on this platform"
	test_done
}

# Tell the fsmonitor--daemon to stop, even if `--stop` failed
test_atexit 'test -n "$debug" || rm -r .git'

test_expect_success 'can start and stop the daemon' '
	test_when_finished \
		"test_might_fail git -C test fsmonitor--daemon --stop" &&
	git init test &&
	(
		cd test &&
		: start the daemon implicitly by querying it &&
		GIT_TRACE2_EVENT="$PWD/../.git/trace" \
		git fsmonitor--daemon --query 1 0 >actual &&
		grep "fsmonitor.*serve" ../.git/trace &&
		git fsmonitor--daemon --is-running &&
		printf / >expect &&
		test_cmp expect actual
	) &&
	rm -rf test/.git &&
	test_must_fail git -C test fsmonitor--daemon --is-running
'

# Note, after "git reset --hard HEAD" no extensions exist other than 'TREE'
# "git update-index --fsmonitor" can be used to get the extension written
# before testing the results.

clean_repo () {
	git reset --hard HEAD &&
	git clean -fd &&
	sleep 1 &&
	>.git/trace
}

test_expect_success 'setup' '
	: >tracked &&
	: >modified &&
	: >delete &&
	: >rename &&
	mkdir dir1 &&
	: >dir1/tracked &&
	: >dir1/modified &&
	: >dir1/delete &&
	: >dir1/rename &&
	mkdir dir2 &&
	: >dir2/tracked &&
	: >dir2/modified &&
	: >dir2/delete &&
	: >dir2/rename &&
	git -c core.fsmonitor= add . &&
	test_tick &&
	git -c core.fsmonitor= commit -m initial &&
	git config core.fsmonitor :internal: &&
	git update-index --fsmonitor &&
	cat >.gitignore <<-\EOF &&
	.gitignore
	expect*
	actual*
	EOF
	>.git/trace &&
	echo 1 >modified &&
	echo 2 >dir1/modified &&
	echo 3 >dir2/modified &&
	>dir1/untracked &&
	git fsmonitor--daemon --stop &&
	test_must_fail git fsmonitor--daemon --is-running
'

edit_files() {
	echo 1 >modified
	echo 2 >dir1/modified
	echo 3 >dir2/modified
	>dir1/untracked
}

delete_files() {
	rm -f delete
	rm -f dir1/delete
	rm -f dir2/delete
}

rename_files() {
	mv rename renamed
	mv dir1/rename dir1/renamed
	mv dir2/rename dir2/renamed
}

test_expect_success 'internal fsmonitor works' '
	GIT_INDEX=.git/fresh-index git read-tree master &&
	GIT_INDEX=.git/fresh-index git -c core.fsmonitor= status >expect &&
	GIT_TRACE2_EVENT="$PWD/.git/trace" git status >actual &&
	git fsmonitor--daemon --is-running &&
	test_cmp expect actual
'

test_expect_success 'edit some files' '
	clean_repo &&
	edit_files &&
	GIT_INDEX=.git/fresh-index git read-tree master &&
	GIT_INDEX=.git/fresh-index git -c core.fsmonitor= status >expect &&
	GIT_TRACE2_EVENT="$PWD/.git/trace" git status >actual &&
	test_cmp expect actual &&
	sleep 1 &&
	grep :\"dir1/modified\" .git/trace &&
	grep :\"dir2/modified\" .git/trace &&
	grep :\"modified\" .git/trace &&
	grep :\"dir1/untracked\" .git/trace
'

test_expect_success 'delete some files' '
	clean_repo &&
	delete_files &&
	GIT_INDEX=.git/fresh-index git read-tree master &&
	GIT_INDEX=.git/fresh-index git -c core.fsmonitor= status >expect &&
	GIT_TRACE2_EVENT="$PWD/.git/trace" git status >actual &&
	test_cmp expect actual &&
	sleep 1 &&
	grep :\"dir1/delete\" .git/trace &&
	grep :\"dir2/delete\" .git/trace &&
	grep :\"delete\" .git/trace
'

test_expect_success 'can stop internal fsmonitor' '
	if git fsmonitor--daemon --is-running
	then
		git fsmonitor--daemon --stop
	fi &&
	sleep 1 &&
	test_must_fail git fsmonitor--daemon --is-running
'

test_done
