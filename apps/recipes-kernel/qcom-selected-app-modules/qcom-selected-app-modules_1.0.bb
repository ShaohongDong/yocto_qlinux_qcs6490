SUMMARY = "External modules for the selected QCOM application"
LICENSE = "MIT"

inherit module

# Keep all external modules declared by one application in its selected-app
# package.  The image depends on this stable package name, while module.bbclass
# still supplies the kernel build dependencies and post-install depmod hooks.
KERNEL_SPLIT_MODULES = "0"
FILES:${PN} += "${nonarch_base_libdir}/modules"

# populate_module_recipe also registers the selected manifest's parse inputs.
python __anonymous() {
    from qcom_apps.bitbake import populate_module_recipe
    populate_module_recipe(d)
}

PV = "1.0+app${QCOM_APP_INPUT_DIGEST}"

qcom_prepare_kernel_scripts() {
    # STAGING_KERNEL_BUILDDIR is shared mutable state.  A kernel rebuild can
    # refresh it after make-mod-scripts has already been stamped as complete,
    # so prepare the host-side module tools immediately before using them.
    unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS
    for target in prepare scripts_basic scripts; do
        oe_runmake -C ${STAGING_KERNEL_DIR} \
            O=${STAGING_KERNEL_BUILDDIR} \
            CC="${KERNEL_CC}" LD="${KERNEL_LD}" AR="${KERNEL_AR}" \
            OBJCOPY="${KERNEL_OBJCOPY}" STRIP="${KERNEL_STRIP}" \
            HOSTCC="${BUILD_CC}" HOSTCFLAGS="${BUILD_CFLAGS}" \
            HOSTLDFLAGS="${BUILD_LDFLAGS}" HOSTCPP="${BUILD_CPP}" \
            $target
    done
}

do_compile() {
    unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS
    qcom_prepare_kernel_scripts
    for module_dir in ${QCOM_APP_MODULE_DIRS}; do
        oe_runmake -C ${STAGING_KERNEL_DIR} \
            M=${S}/$module_dir \
            KERNEL_VERSION=${KERNEL_VERSION} \
            CC="${KERNEL_CC}" LD="${KERNEL_LD}" AR="${KERNEL_AR}" \
            OBJCOPY="${KERNEL_OBJCOPY}" STRIP="${KERNEL_STRIP}" \
            O=${STAGING_KERNEL_BUILDDIR} modules
    done
}

do_install() {
    unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS
    for module_dir in ${QCOM_APP_MODULE_DIRS}; do
        oe_runmake -C ${STAGING_KERNEL_DIR} \
            M=${S}/$module_dir \
            DEPMOD=echo \
            MODLIB=${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION} \
            KERNEL_VERSION=${KERNEL_VERSION} \
            CC="${KERNEL_CC}" LD="${KERNEL_LD}" OBJCOPY="${KERNEL_OBJCOPY}" \
            STRIP="${KERNEL_STRIP}" O=${STAGING_KERNEL_BUILDDIR} modules_install
    done
}

do_fetch[vardeps] += "QCOM_APP QCOM_APP_INPUT_DIGEST"
do_compile[vardeps] += "QCOM_APP QCOM_APP_INPUT_DIGEST QCOM_APP_MODULE_DIRS"
