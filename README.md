# VTest2

# WIP WARNING

As long as this WARNING is present, this repository is not considered stable and
might undergo force-pushes. Once it is stable, this warning will be removed and
the repository will become available as https://github.com/vtest/VTest2

# The HTTP test-program formerly known as Varnishtest (reiterated)

This project is the second iteration of the (almost) unvarnished varnishtest
program, made available as a stand-alone program because it can be used to test
all sorts of HTTP clients, servers and proxies.

If you are coming from the original [vtest](https://github.com/vtest/VTest) (now
also known as vtest1), it should be a plug-in replacement.

Poul-Henning & Nils

# Build Dependencies

## Linux

- libpcre2-dev

# BACKGROUND CORRESPONDENCE

`Date:    Wed, 09 Jan 2019 10:36:14 +0000`

`From:    Poul-Henning Kamp <phk@phk.freebsd.dk>`

`Subject: Re: vtest status`

Personally I am not very keen on turning vtest into a "real" project
with releases, package-building and all that, because it would cause
me a lot of work which I don't think brings enough advantages.

The problem for me is that vtc_varnish has a very incestuos
relationship with varnish internals, internals which we do not want
to turn into documented or even open APIs, and that means that
vtc_varnish and varnishd must match versions.

A stand-alone vtest-package would either need to be compiled against
a specific version of varnish, or have some way to dynamically load
vtc_varnish from from the varnish source/package it is being used
against.

But that just moves the problem to the other side of the line, where
we need to open the internals of vtest up as a documented and
versioned API...

Some day (H3?) all that work may make sense, but I don't feel we
are there yet, at least not as far as cost/benefit is concerned.

My suggestion for now, is to let vtest live as a "source code
library" on github and not build and distribute it as stand-alone
packages.

Instead it will be up to the projects which use it to import
and build in their own projects.

That way HAproxy and leave vtc_varnish out of their compilation so
varnish does not become a build-dependency (or you can conditionally
compile vtc_varnish in, if it is already there.)

And we can compile it in Varnish including vtc_varnish, and include
vtc_haproxy in similar fashion. (actually, compilation of 
vtc_haproxy does not need haproxy to be installed, does it ?)

We should still provide a Makefile in the vtest github project which
compiles as much as possible (ie: vtc_varnish if it can find varnish
installed), that will make work on the shared sources easier for
all of us.

If we decide to do things that way, maybe you should call your
compiled version something like "hatest", while we stick with
"varnishtest", so we can reserve the "vtest" name for the future
runtime-extensible all-singing-and-dancing thing ?

# HISTORY

It was realized that vtest1 maintenance did not go particularly smooth:
Varnish-Cache and vtest1 were using similar, but not identical code bases and
synchronizing them was not exactly a fulfilling task.

Vtest2 was recreated using a different methodology than vtest1 in order to have
as much of a change history with (parts of) the original commits as possible:
`git filter-repo` was used with `--paths-from-file`
[git-filter-repo-files.spec](tools/sync/git-filter-repo-files.spec). This
created the full history of vtest from Varnish-Cache as before the original
vtest inception. While this new history contained most commits in their original
form (because most vtest development was still happening in Varnish-Cache,
actually), it missed some original commits to vtest1, like [this
example](https://github.com/vtest/VTest/commit/89fc145edb5054bd603df8c543877bf54cc76bfa),
which went into Varnish-Cache [like
so](https://github.com/varnishcache/varnish-cache/commit/9784b3984f117250417ee50405ac59d26637e043).
So, in an effort to be good FOSSitizens, in those cases where vtest1 had the
original commit, that was used instead by employing mostly manual `git rebase
-i` work with additional minor polish.

# UPDATING AND SYNCING

Most of the sync problems should go away once [vtest2 becomes a submodule of
Varnish-Cache](https://github.com/varnishcache/varnish-cache/issues/3983): Once
that has happened, Varnish-Cache maintainers will first make additions to
vtest2, and then update the submodule in Varnish-Cache. Other contributions will
also go directly to vtest2.

Yet even in this new scenario, the code in the [lib](lib/) directory of the
vtest2 code base still comes from Varnish-Cache. To simplify updating this
shared code, a [script](update-code-from-vc.sh) has been written, which
basically uses `git filter-repo`, `git format-patch` and `git am` to
automatically apply patches to the shared code base from Varnish-Cache to vtest.

## `make update`: An easy to use sync recipe

The update process can simply be invoked by running `make update`. By default,
this clones Varnish-Cache from github - if you want to avoid that and use your
local repository, you can set and export the `VARNISHSRC` environment variable
to point to your local Varnish-Cache repository (but make sure you do not push
any changes which are not in the public Varnish-Cache repository).

The script is very verbose on purpose to help troubleshooting. If all goes well,
however, its output can be disregarded.

If the script succeeds, it ends with:

```
SUCCESS - commits have been added
```

You should now find in your local git repository one commit for each added
commit from Varnish-Cache concerning [lib/](lib/) and one commit with the
message _Updated code from varnish-cache_ which updates the [base
commit](tools/sync/base_commit) for the next invocation of the update tool.

If there are no changes to apply, the script fails with:

```
error: empty commit set passed
fatal: cherry-pick failed
```

## Updating until Varnish-Cache uses vtest as a submodule

Until Varnish-Cache switches to using vtest as a submodule, the update script
also carries over changes to [src/](src/) from
[bin/varnishtest](https://github.com/varnishcache/varnish-cache/tree/master/bin/varnishtest).
This is a one way process, no automation has been added for the
vtest2->Varnish-Cache direction because that will become obsolete with the
switch to submodule use.

Once the switch has happened, all entries in
[git-filter-repo-files.spec](tools/sync/git-filter-repo-files.spec) concerning
[src/](src/) can be removed.




# TODO:

* Detect content of config.h as required

# NOT-TODO:

* Autocrappery

*EOF*
