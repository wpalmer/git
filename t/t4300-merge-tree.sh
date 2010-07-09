#!/bin/sh
#
# Copyright (c) 2010 Will Palmer
#

test_description='git merge-tree'
. ./test-lib.sh

test_expect_success setup '
	test_commit "initial"
'

test_expect_success 'both added same' '
	git reset --hard initial
	test_commit "same-A" "ONE" "AAA" 

	git reset --hard initial
	test_commit "same-B" "ONE" "AAA"

	git merge-tree initial same-A same-B
'

test_expect_success 'both added conflict' '
	git reset --hard initial
	test_commit "diff-A" "ONE" "AAA" 

	git reset --hard initial
	test_commit "diff-B" "ONE" "BBB"

	git merge-tree initial diff-A diff-B
'

test_expect_success 'nothing similar' '
	git reset --hard initial
	test_commit "no-common-A" "ONE" "AAA" 

	git reset --hard initial
	test_commit "no-common-B" "TWO" "BBB"

	git merge-tree initial no-common-A no-common-B
'

test_done
