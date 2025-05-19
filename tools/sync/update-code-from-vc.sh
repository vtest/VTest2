#!/bin/bash

# Script to update vtest2 source files taken from varnish-cache keeping the
# original commits intact
#
# Author: Nils Goroll <slink@uplex.de>
#
# This file is in the Public Domain
#
#
# USAGE:
# - Optionally set VARNISHSRC to your varnish-cache git repository
# - Everything else should be automatic
#
set -eux

MYDIR=$(dirname "$0")
MYDIR=$(realpath "${MYDIR}")
cd "${MYDIR}"
TOP=$(git rev-parse --show-toplevel)

##############################
# BEGIN config
VARNISHSRC=${VARNISHSRC:-https://github.com/varnishcache/varnish-cache.git}
VARNISHBRANCH=master

BASEF="${MYDIR}"/base_commit
BASE=$(cat "${BASEF}")
SPEC="${MYDIR}"/git-filter-repo-files.spec
# END config
##############################

tagbase="tag-ucfvc"
branchbase="branch-ucfvc"
workdir=$(mktemp -d)

function onexit {
	cd
	rm -rf "${workdir}"
}

trap onexit EXIT

cd "${workdir}"
git clone --no-tags -o origin --branch "${VARNISHBRANCH}" --single-branch "${VARNISHSRC}"
pushd varnish-cache

git remote remove origin
git tag "${tagbase}-base" "${BASE}"
git rebase -X theirs "${tagbase}-base"
git tag "${tagbase}-head"
NEWBASE=$(git rev-parse "${tagbase}-head")
git checkout -b "${branchbase}-squash" "${tagbase}-base"
git reset --soft "$(git rev-list --max-parents=0 HEAD)"
git commit -m "SQUASH"
git tag "${tagbase}-squash"
git cherry-pick "${tagbase}-base..${tagbase}-head"
git tag -d "${tagbase}-head" "${tagbase}-base"
git branch -D "${VARNISHBRANCH}"
git branch -a	# dignostic only
git tag -l	# dignostic only
popd

# now we have a simplified git history with only new commits since BASE
# - rewrite to vtest paths in a new repo
git init vc2vt
git filter-repo \
	--source varnish-cache \
	--target vc2vt \
	--paths-from-file "${SPEC}"
cd vc2vt
git checkout "${branchbase}-squash"
git tag -l

# vc2vt now has all the new commits from varnish-cache but with the vtest
# path scheme
patchdir=$(mktemp --tmpdir -d ucfvc_patches.XXXXXXXXXX)
git format-patch -o "${patchdir}" "${tagbase}-squash"
 
# apply
cd "${TOP}"
git am "${patchdir}"/*
echo "${NEWBASE}" >"${BASEF}"
git commit -m 'Updated code from varnish-cache' "${BASEF}"
rm -rf "${patchdir}"
set +x
echo SUCCESS - commits have been added
