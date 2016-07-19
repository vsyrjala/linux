=========
 drm-misc
=========

-------------------------------------------------------------
drm-misc patch and upstream merge flow and timeline explained
-------------------------------------------------------------

Branches
========

Right now there's only `drm-misc`, plus maybe temporary topic branches. There's
also a `drm-fixes` branch, which can be useful when Dave Airlie is on vacations.

Merge Criteria
==============

Right now the only hard merge criteria are:

* Patch is properly reviewed or at least ack, i.e. don't just push your own
  stuff directly.

* drm-misc is for simple driver patches, odd-ball small stuff that might easily
  slip through the cracks and the more mundane DRM subsystem wide changes. Big
  stuff (new drivers, big driver updates, big new DRM core features) should go
  in through separate pull requests. Those topic branches can still be
  maintained with the dim_ tooling, but that's of course entirely optional.

* All the x86 and arm DRM drivers need to still compile. To simplify this we
  track defconfigs for both platforms in the `drm-intel-rerere` branch.

* The goal is to also pre-check everything with CI. Unfortunately neither the
  arm side (using kernelci.org and generic i-g-t tests) nor the Intel side
  (using Intel CI infrastructure and the full i-g-t suite) isn't yet fully ready
  for production.

* No rebasing out mistakes, because this is a shared tree.

Tooling
=======

drm-intel git repositories are managed with dim_:

.. _dim: dim.html

