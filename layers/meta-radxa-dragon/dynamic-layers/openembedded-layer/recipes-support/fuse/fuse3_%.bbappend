# SPDX-License-Identifier: MIT
# do_install_ptest_base's cleandirs setup can create ${libdir}/${BPN} before
# fakeroot starts.  The ptest class only fixes ownership below PTEST_PATH,
# leaving this parent directory owned by the build user and breaking RPM
# generation when that UID is absent from the target passwd database.
do_install_ptest_base:append() {
    if [ -d "${D}${libdir}/${BPN}" ]; then
        chown root:root "${D}${libdir}/${BPN}"
    fi
}

fix_fuse3_ptest_parent_ownership() {
    # Images without ptest do not install this directory.
    if [ -d "${PKGD}${libdir}/${BPN}" ]; then
        chown root:root "${PKGD}${libdir}/${BPN}"
    fi
}

PACKAGE_PREPROCESS_FUNCS:append = " fix_fuse3_ptest_parent_ownership"
