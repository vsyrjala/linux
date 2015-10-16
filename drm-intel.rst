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

Maintained by Daniel Vetter and co. Consists mostly of `drivers/gpu/drm/i915`.

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
features have been merged there.

Pull requests to Dave are sent as needed, with no particular schedule.

drm-intel-fixes (aka "-fixes")
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This branch contains fixes to Linus' tree after drm-next has been merged during
the merge window. The fixes are merged through drm-fixes. Valid from -rc1 to the
kernel release.

Usually Linus releases each -rc on a Sunday, and drm-intel-fixes gets rebased on
that the following Monday. The pull request to Dave for new fixes is typically
sent on the following Thursday. This is repeated until final release of the
kernel.

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

Merge Timeline
==============

This chart describes the merge timelines for various branches in terms of one
kernel release cycle. Worth noting is that we're working on two or three kernel
releases at the same time. Big features take a long time to hit a kernel
release. There are no fast paths.

.. Note: This requires JavaScript and will access http://wavedrom.com to render.
.. include:: drm-intel-timeline.rst

For predictions on the future merge windows and releases, see
http://phb-crystal-ball.org/.


Copyright
=========

.. include:: COPYING
	:literal:
