#!/bin/sh
g="$(git rev-parse --git-dir 2>/dev/null)" || exit 1

# Situations:
#	REBASE-INTERACTIVE
#		"edit"
#			has the edit yet occurred ? (commit != original commit, no idea how to check)
#			Y:	git rebase continue
#			N:	Is there a diff?
#				Y:	Are any changes staged?
#					Y:	git commit --amend
#					N:	echo "Stage changes using git add"
#				N:	echo "Make changes to cause this tree to look how you want, or,
#					     "use git commit --amend to edit the message"
#		"(conflict)"
#			Are there unresolved conflicts?
#			Y:	Can we tell whether the conflicts occur in files which would have
#				conflict markers?
#				Y: Are there any conflict markers?
#					Y: "Edit these files to remove the conflicts: (list files)"
#					N: "It appears that the conflict has been resolved. Use git add to"
#				   	"confirm that you have resolved the conflicts"
#				N: "Edit these files to remove the conflicts: (list files), then use git add"
#				   "to confirm that they have been resolved"
#			N:	git rebase continue
#	MERGE
#		(conflict):
#			Same as rebase conflict, but with "git commit" instead of "git rebase continue" at the end
