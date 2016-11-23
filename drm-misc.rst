=========
 drm-misc
=========

-------------------------------------------------------------
drm-misc patch and upstream merge flow and timeline explained
-------------------------------------------------------------

This document describes the flow and timeline of misc drm and gpu patches to
various upstream trees. For a detailed list of what's all maintained in drm-misc
grep for "drm-misc" in MAINTAINERS.

Rule No. 1
==========

This document is an eternal draft and simply tries to explain the reality of how
drm-misc is maintained. If you observe a difference between these rules and
reality, it is your assumed responsibility to update the rules.

The workflow is heavily based upon the one used to maintain the Intel drm
driver, see `drm-intel <drm-intel.html>`_:

Branches
========

All branches are maintained in `git://anongit.freedesktop.org/drm-misc`.

drm-misc-next
~~~~~~~~~~~~~

This is the main feature branch where most of the patches land. This branch is
always open to "hide" the merge window from developers. To avoid upsetting
linux-next and causing mayhem in the merge window in general no pull requests
are sent to upstream 1-2 weeks before the merge window opens. Outside of that
feature freeze period pull requests are sent to upstream roughly every week, to
avoid too much coordination pains.

If you're unsure apply your patch here, it can always be cherry-picked to one of
the -fixes patches later on. But in contrast to the drm-intel flow
cherry-picking is not the default.

drm-misc-next-fixes
~~~~~~~~~~~~~~~~~~~

This is for bugfixes to drm-misc-next after feature freeze, but before -rc1 is
tagged.

drm-misc-fixes
~~~~~~~~~~~~~~

This is for bugfixes which target the current -rc cycle.

drm-tip
~~~~~~~

This is the overall integration tree for drm, and lives in
`git://anongit.freedesktop.org/drm-tip`. Every time one of the above branches is
update drm-tip gets rebuild. If there's a conflict see section on `resolving
conflicts when rebuilding drm-tip
<drm-intel.html#resolving-conflicts-when-rebuilding-drm-tip>`_.

Merge Criteria
==============

Right now the only hard merge criteria are:

* Patch is properly reviewed or at least ack, i.e. don't just push your own
  stuff directly.

* drm-misc is for drm core (non-driver) patches, subsystem-wide refactorings,
  and small trivial patches all over (including drivers). For a detailed list of
  what's all maintained in drm-misc grep for "drm-misc" in MAINTAINERS.

* Larger features can be merged through drm-misc too, but in some cases
  (especially when there are cross-subsystem conflicts) it might make sense to
  merge patches through a dedicated topic tree. The dim_ tooling has full
  support for them, if needed.

* Any non-linear actions (backmerges, merging topic branches and sending out
  pull requests) are only done by the official drm-misc maintainers (currently
  Daniel, Jani and Sean, see MAINTAINERS), and not by committers.

* All the x86, arm and arm64 DRM drivers need to still compile. To simplify this
  we track defconfigs for all three platforms in the `drm-intel-rerere` branch.

* The goal is to also pre-check everything with CI. Unfortunately neither the
  arm side (using kernelci.org and generic i-g-t tests) nor the Intel side
  (using Intel CI infrastructure and the full i-g-t suite) isn't yet fully ready
  for production.

* No rebasing out mistakes, because this is a shared tree.

* See also the extensive `committer guidelines for drm-intel
  <drm-intel.html#committer-guidelines>`_.

Tooling
=======

drm-misc git repositories are managed with dim_:

.. _dim: dim.html

