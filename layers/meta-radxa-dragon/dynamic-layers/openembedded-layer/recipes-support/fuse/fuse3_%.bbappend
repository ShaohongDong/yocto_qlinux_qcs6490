# do_install_ptest_base's cleandirs setup can create ${libdir}/${BPN} before
# fakeroot starts.  The ptest class only fixes ownership below PTEST_PATH,
# leaving this parent directory owned by the build user and breaking RPM
# generation when that UID is absent from the target passwd database.
do_install_ptest_base:append() {
    chown root:root ${D}${libdir}/${BPN}
}

fix_fuse3_ptest_parent_ownership() {
    chown root:root ${PKGD}${libdir}/${BPN}
}

PACKAGE_PREPROCESS_FUNCS:append = " fix_fuse3_ptest_parent_ownership"
