#!/bin/sh

USAGE='[-<n> | -a] [--only-orphans] [<branch-name>]'
LONG_USAGE='git-reflog-tree formats reflog entries into a format
more-useful for tracking down "lost" changes. By default, it will limit
its output to the last 50 entries, though specifying -<n> or -a on the
command-line can alter how many are used. If --only-orphans is specified,
then only commits which are "not reachable", and is more useful for
finding commits which were "lost" after being made outside of a branch.'

SUBDIRECTORY_OK=Yes
OPTIONS_SPEC=
. git-sh-setup

num_commits=
only_orphans=0
branch_name=
while test $# != 0
do
	case "$1" in
		-a|--all)
			test -z "$num_commits" || usage
			num_commits=-1
		;;
		-[0-9]|-[0-9][0-9]|-[0-9][0-9][0-9]|-[0-9][0-9][0-9][0-9])
			test -z "$num_commits" || usage
			num_commits="${1#-}"
		;;
		--only-orphans)
			only_orphans=1
		;;
		-*)
			usage
		;;
		*)
			test -z "$branch_name" -a -n "$1" || usage
			branch_name="$1"
		;;
	esac
	shift
done

test -z "$branch_name" && branch_name="HEAD"
test -z "$num_commits" && num_commits="$(git config --int reflog-tree.limit)"
test -z "$num_commits" && num_commits=50
if test "$num_commits" = "-1"
then
	num_commits=
else
	num_commits=-"$num_commits"
fi

common=
did_merge=0
refs="$(git log -g --pretty='format:%H' $num_commits "$branch_name")"
for commit in $refs
do
	if test -z "$common"
	then
		common="$commit"
	else
		common="$(git merge-base "$common" "$commit")"
		did_merge=1
	fi
done

if test "$did_merge" = "1"
then
	not_common="--not $common"
else
	not_common=
fi

if test "$only_orphans" = "1"
then
	parents=
	for commit in $(git for-each-ref --format='%(objectname)')
	do
		parents="$parents $commit^!"
	done

	git log --oneline --graph "$branch_name" $refs $parents $not_common
else
	git log --oneline --graph $refs $not_common
fi
