#!/bin/sh
g="$(git rev-parse --git-dir 2>/dev/null)" || exit 1

# Situations:
#	REBASE-INTERACTIVE
#		git rebase abort
#	MERGE
#		what's a safe way to do this? reset --hard is too much of a sledgehammer, but might be
#		appropriate in some situations. Checkout --patch ?
#	BISECT
#		git bisect reset
