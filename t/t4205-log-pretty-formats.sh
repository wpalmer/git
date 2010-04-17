#!/bin/sh
#
# Released into Public Domain by Will Palmer 2010
#

test_description='Test pretty formats'
. ./test-lib.sh

test_expect_success "set up basic repos" \
	">foo &&
	>bar &&
	git add foo &&
	test_tick &&
	git commit -m initial &&
	git add bar &&
	test_tick &&
	git commit -m 'add bar'"

for flag in false true always; do
for color in red green blue reset; do

	make_expected="git config --get-color no.such.slot $color >expected"
	test_expect_success "%C$color with color.ui $flag" \
		"$make_expected &&
		git config color.ui $flag &&
		git log -1 --pretty=format:'%C$color' > actual &&
		test_cmp expected actual"


	test_expect_success "%C($color) with color.ui $flag" \
		"$make_expected &&
		git config color.ui $flag &&
		git log -1 --pretty=format:'%C($color)' > actual &&
		test_cmp expected actual"

	[ ! "$flag" = "always" ] && make_expected=">expected"
	test_expect_success "%C?$color with color.ui $flag" \
		"$make_expected &&
		git config color.ui $flag &&
		git log -1 --pretty=format:'%C?$color' > actual &&
		test_cmp expected actual"

	test_expect_success "%C?($color) with color.ui $flag" \
		"$make_expected &&
		git config color.ui $flag &&
		git log -1 --pretty=format:'%C?($color)' > actual &&
		test_cmp expected actual"

done
done

test_done
