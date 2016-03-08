===========
 drm-intel
===========

--------------------------------------------------------------
drm-intel patch and upstream merge flow and timeline explained
--------------------------------------------------------------

:Copyright: 2015 Intel Corporation
:Author: Jani Nikula <jani.nikula@intel.com>

Introduction
============

This document describes the flow and timeline of drm/i915 patches to various
upstream trees.

THIS IS A DRAFT WORK-IN-PROGRESS.

The Relevant Repositories and Branches
======================================

The Upstream Linux Kernel Repository
------------------------------------

`git://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git`

Maintained by Linus Torvalds.

master
~~~~~~

Linus' master, the upstream, or mainline. This is where all features from all
subsystems, including DRM and i915, are merged.

The upstream follows a single branch, time-based development model, with a new
kernel release occurring roughly every 10 weeks. New features are merged from
subsystem trees during the two week merge window immediately following a kernel
release. After the merge window, the new development kernel is stabilized by
only merging fixes until the next kernel release. During development, there's a
new release candidate (-rc) kernel each week.

The Upstream DRM Subsystem Repository
-------------------------------------

`git://people.freedesktop.org/~airlied/linux.git`

Maintained by Dave Airlie of Red Hat. Consists mostly of `drivers/gpu/drm`.

drm-next
~~~~~~~~

This is the branch where all new features for the DRM core and all the GPU
drivers, including drm/i915, are merged.

The drm-next branch is closed for new features at around -rc5 timeframe of the
current development kernel in preparation for the upcoming merge window for the
next kernel, when drm-next gets merged to Linus' master. Thus there's a
stabilization period of about 3-5 weeks during which only bug fixes are merged
to drm-next.

drm-fixes
~~~~~~~~~

This is the branch where all the fixes for the DRM core and all the GPU drivers
for the current development kernels are merged. drm-fixes is usually merged to
Linus' master on a weekly basis.

The Upstream i915 Driver Repository
-----------------------------------

`git://anongit.freedesktop.org/drm-intel`

Maintained by Daniel Vetter and Jani Nikula, with several developers also having
commit access for pushing `drm-intel-next-queued`. Consists mostly of
`drivers/gpu/drm/i915`.

drm-intel-next-queued (aka "dinq")
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This is the branch where all new features, as well as any non-trivial or
controversial fixes, are applied.

This branch "hides" the merge window from the drm/i915 developers; patches are
applied here regardless of the development phase of the Linus' upstream kernel.

drm-intel-next
~~~~~~~~~~~~~~

drm-intel-next-queued at some point in time.

drm-intel-next-fixes (aka "dinf")
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This branch contains drm/i915 specific fixes to drm-next after the drm/i915
features have been merged there. Fixes are first applied to
drm-intel-next-queued, and cherry-picked to drm-intel-next-fixes.

Pull requests to Dave are sent as needed, with no particular schedule.

drm-intel-fixes (aka "-fixes")
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This branch contains fixes to Linus' tree after drm-next has been merged during
the merge window. Fixes are first applied to drm-intel-next-queued, and
cherry-picked to drm-intel-fixes. The fixes are then merged through drm-fixes.
Valid from -rc1 to the kernel release.

Usually Linus releases each -rc on a Sunday, and drm-intel-fixes gets rebased on
that the following Monday. Usually this is a fast-forward. The pull request to
Dave for new fixes is typically sent on the following Thursday. This is repeated
until final release of the kernel.

This is the fastest path to getting fixes to Linus' tree. It is generally for
the regressions, cc:stable, black screens, GPU hangs only, and should pretty
much follow the stable rules.

drm-intel-nightly
~~~~~~~~~~~~~~~~~

This branch combines them all. Rebuilt every time one of the trees is pushed.

Patch and Merge Flow
====================

This chart describes the flow of patches to drm-intel branches, and the merge
flow of the commits to drm-upstream and Linus' tree.

.. Note: This requires SVG support in the browser.
.. raw:: html
	:file: drm-intel-flow.svg

Legend: Green = Linus. Red = drm-upstream. Blue = drm-intel. Black = patches.
Yellow = Additional trees from other subsystems.

Features
--------

Features are picked up and pushed to drm-intel-next-queued by committers and
maintainers. See committer guidelines below for details.

Fixes
-----

Fixes are picked up and pushed to drm-intel-next-queued by committers and
maintainers, just like any other patches. This is to ensure fixes are pushed in
a timely manner. Fixes that are relevant for stable, current development
kernels, or drm-next, will be cherry-picked by maintainers to drm-intel-fixes or
drm-intel-next-fixes.

To make this work, patches should be labeled as fixes (see below), and extra
care should be put into making fixes the first patches in series, not depending
on preparatory work or cleanup.

Labeling Fixes Before Pushing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

To label fixes that should be cherry-picked to the current -rc development
kernel or drm-next, the commit message should contain either:

	Cc: drm-intel-fixes@lists.freedesktop.org

or, if the fix is relevant for a released kernel,

	Cc: stable@vger.kernel.org

If your patch fixes a regression then please include a Fixes: line to help
maintainers where to cherry-pick a patch to. This also extremely useful for
product groups to know which bugfixes they must include. To follow the
recommended format please generate the Fixes: line using ::

        $ dim fixes $regressing_commit

If the Cc: or Fixes: was forgotten, you can still reply to the list with that,
just like any other tags, and they should be picked up by whoever pushes the
patch.

The maintainers will cherry-pick labeled patches from drm-intel-next-queued to
the appropriate branches.

If possible, the commit message should also contain a Fixes: tag as described in
`Documentation/SubmittingPatches
<https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/tree/Documentation/SubmittingPatches>`_
to aid the maintainers in identifying the right branch.

Requesting Fixes Cherry-Pick Afterwards
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

It's not uncommon for a patch to have been committed before it's identified as a
fix needing to be backported.

If the patch is already in Linus' tree, please follow `stable kernel rules
<https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/tree/Documentation/stable_kernel_rules.txt>`_.

Otherwise, send an email to intel-gfx@lists.freedesktop.org and
drm-intel-fixes@lists.freedesktop.org containing the subject of the patch, the
commit id, why you think it should be applied, and what branch you wish it to be
applied to.

Replying to the original patch is also fine, but please do remember to add Cc:
drm-intel-fixes@lists.freedesktop.org and the commit id.

Alternatively, if the cherry-pick has conflicts, please send a patch to
intel-gfx@lists.freedesktop.org and drm-intel-fixes@lists.freedesktop.org with
subject prefix "drm-intel-fixes PATCH" or "drm-intel-next-fixes PATCH" depending
on the branch. Please add 'git cherry-pick -x' style annotation above your
Signed-off-by: line in the commit message:

	(cherry picked from commit 0bff4858653312a10c83709e0009c3adb87e6f1e)

Resolving Conflicts when Rebuilding drm-intel-nightly
=====================================================

When you push patches with dim drm-intel-nightly always gets rebuild and this
can sometimes fail, for example like this: ::

        Updating rerere cache and nightly.conf... Done.
        Fetching drm-upstream... Done.
        Fetching origin... Done.
        Fetching sound-upstream... Done.
        Merging origin/drm-intel-fixes... Reset. Done.
        Merging drm-upstream/drm-fixes... Fast-forward. Done.
        Merging origin/drm-intel-next-fixes... Done.
        Merging origin/drm-intel-next-queued... ++<<<<<<< HEAD
        ++=======
        ++>>>>>>> origin/drm-intel-next-queued
        Fail: conflict merging origin/drm-intel-next-queued

Often it's very easy to resolve such conflicts, but maintainers can take over
when it's tricky or something fails in the below procedure.

1. First check that drm-intel-next-queued was indeed pushed correctly and that
   your local and remote branches match.

2. Then re-run the -nightly generation just to confirm: ::

        $ dim rebuild-nightly

   It's handy to keep the log output for context so that you know which branch
   caused the conflicts, and which branches are already included.

3. Switch to $DIM_PREFIX/drm-intel-nightly and analyze the conflict: ::

        $ git diff # shows three-way diff of conflict
        $ gitk --merge # lists all commits git believes to be relevant

   If the conflict is simple and created by one of the patches fix things up and
   compile/test the resulting kernel. In case of doubt just ping authors of
   other patches or maintainers on IRC.

4. When you're happy with the resolution commit it with ::

        $ git commit -a

   git will then store the conflict resolution internally (see git help rerere
   for how this is implemented). Then re-run -nigthly generation to confirm the
   resolution has been captured correctly by git (sometimes git rerere can't
   match up your resolution with the conflict for odd reasons) and to make sure
   there's no other conflict in later merges: ::

        $ dim rebuild-nightly

   This will also push the stored conflict resolution to the drm-intel-rerere
   branch and therefore publishes your resolution. Everything before this step
   has just local effects.

And if any step fails or the conflict is tricky just ping maintainers.

Merge Timeline
==============

This chart describes the merge timelines for various branches in terms of one
kernel release cycle. Worth noting is that we're working on two or three kernel
releases at the same time. Big features take a long time to hit a kernel
release. There are no fast paths.

.. include:: drm-intel-timeline.rst

For predictions on the future merge windows and releases, see
http://phb-crystal-ball.org/.

Committer Guidelines
====================

This section describes the guidelines for pushing patches. Strict rules covering
all cases are impossible to write and follow. We put a lot of trust in the sound
judgement of the people with commit access, and that only develops with
experience. These guidelines are primarily for the committers to aid in making
the right decisions, and for others to set their the expectations right.

The short list:

* Only push patches changing `drivers/gpu/drm/i915`.

* Only push patches to `drm-intel-next-queued` branch.

* Ensure certain details are covered, see separate list below.

* You have confidence in the patches you push, proportionate to the complexity
  and impact they have. The confidence must be explicitly documented with
  special tags (Reviewed-by, Acked-by, Tested-by, Bugzilla, etc.) in the commit
  message. This is also your insurance, and you want to have it when the commit
  comes back haunting you. The complexity and impact are properties of the patch
  that must be justified in the commit message.

* Last but not least, especially when getting started, you can't go wrong with
  asking or deferring to the maintainers. If you don't feel comfortable pushing
  a patch for any reason (technical concerns, unresolved or conflicting
  feedback, management or peer pressure, or anything really) it's best to defer
  to the maintainers. This is what the maintainers are there for.

* After pushing, please reply to the list that you've done so.

Detail Check List
-----------------

An inexhaustive list of details to check:

* The patch conforms to `Documentation/SubmittingPatches
  <https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/tree/Documentation/SubmittingPatches>`_.

* The commit message is sensible, and includes adequate details and references.

* Bug fixes are clearly indicated as such.

* IGT test cases, if applicable, must be complete and reviewed. Please see
  `details on testing requirements
  <http://blog.ffwll.ch/2013/11/testing-requirements-for-drmi915.html>`_.

* An open source userspace, reviewed and ready for merging by the upstream
  project, must be available for new kernel ABI. Please see `details on
  upstreaming requirements
  <http://blog.ffwll.ch/2015/05/gfx-kernel-upstreaming-requirements.html>`_.

* Relevant documentation must be updated as part of the patch or series.

* Patch series builds and is expected to boot every step of the way, i.e. is
  bisectable.

* Each patch is of a sensible size. A good rule of thumb metric is, would you
  (or the author) stand a chance to fairly quickly understand what goes wrong if
  the commit is reported to cause a regression?

* `checkpatch.pl` does not complain. (Some of the more subjective warnings may
  be ignored at the committer's discretion.)

* The patch does not introduce new `sparse` warnings.

* When pushing someone else's patch you must add your own signed-off per
  http://developercertificate.org/. dim apply-branch should do this
  automatically for you.

On Confidence, Complexity, and Transparency
-------------------------------------------

* Reviewed-by/Acked-by/Tested-by must include the name and email of a real
  person for transparency. Anyone can give these, and therefore you have to
  value them according to the merits of the person. Quality matters, not
  quantity. Be suspicious of rubber stamps.

* Reviewed-by/Acked-by/Tested-by can be asked for and given informally (on the
  list, IRC, in person, in a meeting) but must be added to the commit.

* Reviewed-by. All patches must be reviewed, no exceptions. Please see
  "Reviewer's statement of oversight" in `Documentation/SubmittingPatches
  <https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/tree/Documentation/SubmittingPatches>`_
  and `review training
  <http://blog.ffwll.ch/2014/08/review-training-slides.html>`_.

* Acked-by. Indicates acceptance. No reply is not a sign of acceptance, unless
  you've involved the person (added Cc:, explicitly included in discussion).

* Tested-by. Please solicit separate Tested-by especially from the bug
  reporters, or people with specific hardware that you or the author do not
  have.

* There must not be open issues or unresolved or conflicting feedback from
  anyone. Clear them up first. Defer to maintainers as needed.

* For patches that are simple, isolated, non-functional, not visible to
  userspace, and/or are in author or reviewer's domain of expertise, one
  reviewer is enough. Author with commit access can push after review, or
  reviewer with commit access can push after review.

* The more complicated the patch gets, the greater the impact, involves ABI,
  touches several areas or platforms, is outside of author's domain of
  expertise, the more eyeballs are needed. For example, several reviewers, or
  separate author, reviewer and committer. Make sure you have experienced
  reviewers. Involve the domain expert(s). Possibly involve people across
  team/group boundaries. Possibly involve the maintainers. Give people more time
  to voice their concerns.

* Most patches fall somewhere in between. You have to be the judge, and ensure
  you have involved enough people to feel comfortable if the justification for
  the commit is questioned afterwards.

* Make sure pre-merge testing is completed successfully.


Pre-Merge Testing
-----------------

Our CI infrastructure is being built up and currently requirements for pre-merge
testing are fairly simple:

* All patches must past IGT Basic Acceptance Tests (BAT) on all the CI machines
  without causing regressions.  The CI bots will send results to intel-gfx for
  any patches tracked by patchwork. Check CI failures and make sure any sporadic
  failures are a) pre-existing b) tracked in bugzilla. If there's anything
  dubious that you can't track down to pre-existing&tracked issues please don't
  push, but instead figure out what's going on.

Tooling
=======

drm-intel git repositories are managed with dim_:

.. _dim: dim.html

Copyright
=========

.. include:: COPYING
	:literal:
