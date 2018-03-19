#!/bin/sh

test_description='projectionChanged hook'

. ./test-lib.sh

test_expect_success 'setup' '
	mkdir -p dir1 &&
	touch dir1/file1.txt &&
	touch dir1/file2.txt &&
	git add . &&
	git commit -m "initial"
'

test_expect_success 'test status, add, commit, others dont trigger hook' '
	mkdir -p .git/hooks &&
	write_script .git/hooks/projectionChanged <<-\EOF &&
		echo "ERROR: projectionChanged called." >testfailure
		exit 1
	EOF
	mkdir -p dir2 &&
	touch dir2/file1.txt &&
	touch dir2/file2.txt &&
	git status &&
	test_path_is_missing testfailure &&
	git add . &&
	test_path_is_missing testfailure &&
	git commit -m "second" &&
	test_path_is_missing testfailure &&
	git checkout -- dir1/file1.txt &&
	test_path_is_missing testfailure &&
	git update-index &&
	test_path_is_missing testfailure &&
	git reset --soft &&
	test_path_is_missing testfailure
'

test_expect_success 'test checkout and reset trigger the hook' '
	write_script .git/hooks/projectionChanged <<-\EOF &&
		if test "$1" -eq 1 && test "$2" -eq 1; then
			echo "Invalid combination of flags passed to hook; updated_workdir and reset_mixed are both set." >testfailure
			exit 1
		fi
		if test "$1" -eq 0 && test "$2" -eq 0; then
			echo "Invalid combination of flags passed to hook; neither updated_workdir or reset_mixed are set." >testfailure
			exit 2
		fi
		if test "$1" -eq 1; then
			if ! test -f "$GIT_DIR/index.lock"; then
				echo "updated_workdir set but $GIT_DIR/index.lock does not exist" >testfailure
				exit 3
			fi
		else
			echo "update_workdir should be set for checkout" >testfailure
			exit 4
		fi
		echo "success" >testsuccess
	EOF
	git checkout -b test &&
	test_path_is_file testsuccess && rm -f testsuccess &&
	test_path_is_missing testfailure &&
	git checkout master &&
	test_path_is_file testsuccess && rm -f testsuccess &&
	test_path_is_missing testfailure &&
	git checkout HEAD &&
	test_path_is_file testsuccess && rm -f testsuccess &&
	test_path_is_missing testfailure &&
	git reset --hard &&
	test_path_is_file testsuccess && rm -f testsuccess &&
	test_path_is_missing testfailure
'

test_expect_success 'test reset --mixed triggers the hook' '
	write_script .git/hooks/projectionChanged <<-\EOF &&
		if test "$1" -eq 1 && test "$2" -eq 1; then
			echo "Invalid combination of flags passed to hook; updated_workdir and reset_mixed are both set." >testfailure
			exit 1
		fi
		if test "$1" -eq 0 && test "$2" -eq 0; then
			echo "Invalid combination of flags passed to hook; neither updated_workdir or reset_mixed are set." >testfailure
			exit 2
		fi
		if test "$2" -eq 1; then
			if ! test -f "$GIT_DIR/index"; then
				echo "reset_mixed set but $GIT_DIR/index does not exist" >testfailure
				exit 3
			fi
		else
			echo "reset_mixed should be set for reset --mixed" >testfailure
			exit 4
		fi
		echo "success" >testsuccess
	EOF
	git reset --mixed head~1 &&
	test_path_is_file testsuccess && rm -f testsuccess &&
	test_path_is_missing testfailure
'

test_done
