meta-lts-mixins - wrynose/linux-firmware
================================

"Mixin" layer for adding latest Linux firmware into the Yocto Project LTS.

At the time Wrynose was released in May 2026 it included Linux firmware 20260410,
and officially Wrynose supports only that. This thin special-purpose mixin layer
is meant to provide a current Linux firmware for Wrynose by backporting the
linux-firmware recipe from the master branch of openembedded-core.

Dependencies
------------

This layer depends on:

- URI: git://github.com/openembedded/openembedded-core.git
  layers: meta
    branch: wrynose

Contributing
------------

The patches can be backported from openembedded-core with:

 git -C ../openembedded-core format-patch --stdout -1 origin/master meta/recipes-kernel/linux-firmware | \
  git am --signoff -p4 --directory=recipes-kernel/linux-firmware

  The yocto-patches mailinglist (yocto-patches@lists.yoctoproject.org) is used
  for questions, comments and patch review. It is subscriber only, so please
  register before posting.

  Send pull requests to yocto-patches@lists.yoctoproject.org with
  '[meta-lts-mixins][wrynose/linux-firmware]' in the subject.

  When sending single patches, please use something like:
  git send-email -M -1 --to=yocto-patches@lists.yoctoproject.org --subject-prefix='meta-lts-mixins][wrynose/linux-firmware][PATCH'

Maintenance
-----------

Layer maintainers:
* Jose Quaresma <jose.quaresma@oss.qualcomm.com>
* Viswanath Kraleti <viswanath.kraleti@oss.qualcomm.com>
