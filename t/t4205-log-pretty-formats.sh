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
test_expect_success "reset color flags" "git config --unset color.ui"

test_expect_success "%H with --abbrev-commit should be synonym for %h" \
	"git log -1 --pretty='format:%H' --abbrev-commit > expected &&
	git log -1 --pretty='format:%h' > actual &&
	test_cmp expected actual"

test_expect_success "%H with --abbrev-commit should respect --abbrev" \
	'test 20 = $(git log -1 --pretty="format:%H" --abbrev-commit --abbrev=20 | wc -c)'

test_expect_success "%h should respect --abbrev" \
	'test 20 = $(git log -1 --pretty="format:%h" --abbrev-commit --abbrev=20 | wc -c)'

test_expect_success "log --pretty=raw should NOT respect --abbrev-commit" \
	'git log -1 --pretty=raw > expected &&
	git log -1 --pretty=raw --abbrev-commit > actual &&
	test_cmp expected actual'

test_expect_success "alias builtin format" \
	"git log --pretty=oneline >expected &&
	git config pretty.test-alias oneline &&
	git log --pretty=test-alias >actual &&
	test_cmp expected actual"

test_expect_success "alias masking builtin format" \
	"git log --pretty=oneline >expected &&
	git config pretty.oneline '%H' &&
	git log --pretty=oneline >actual &&
	test_cmp expected actual"

test_expect_success "alias user-defined format" \
	"git log --pretty='format:%h' >expected &&
	git config pretty.test-alias 'format:%h' &&
	git log --pretty=test-alias >actual &&
	test_cmp expected actual"

test_expect_success "alias user-defined tformat" \
	"git log --pretty='tformat:%h' >expected &&
	git config pretty.test-alias 'tformat:%h' &&
	git log --pretty=test-alias >actual &&
	test_cmp expected actual"

test_expect_code 128 "alias non-existant format" \
	"git config pretty.test-alias format-that-will-never-exist &&
	git log --pretty=test-alias"

test_expect_success "alias of an alias" \
	"git log --pretty='tformat:%h' >expected &&
	git config pretty.test-foo 'tformat:%h' &&
	git config pretty.test-bar test-foo &&
	git log --pretty=test-bar >actual &&
	test_cmp expected actual"

test_expect_success "alias masking an alias" \
	"git log --pretty=format:'Two %H' >expected &&
	git config pretty.duplicate 'format:One %H' &&
	git config --add pretty.duplicate 'format:Two %H' &&
	git log --pretty=duplicate >actual &&
	test_cmp expected actual"

test_expect_code 128 "alias loop" \
	"git config pretty.test-foo test-bar &&
	git config pretty.test-bar test-foo &&
	git log --pretty=test-foo"

test_done
