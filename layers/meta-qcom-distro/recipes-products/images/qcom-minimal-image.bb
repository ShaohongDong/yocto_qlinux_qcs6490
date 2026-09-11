SUMMARY = "Minimal image"

LICENSE = "BSD-3-Clause-Clear"

IMAGE_FEATURES += "splash tools-debug allow-root-login post-install-logging"

inherit core-image features_check extrausers image-adbd

# let's make sure we have a good image..
REQUIRED_DISTRO_FEATURES = "pam systemd"

CORE_IMAGE_BASE_INSTALL += " \
    kernel-modules \
    packagegroup-qcom-utilities-bluetooth-utils \
    packagegroup-qcom-utilities-filesystem-utils \
    resize-rootfs \
    zram \
"

# Default login: root / root
EXTRA_USERS_PARAMS = "usermod -p '\$6\$tXF5X5uFywp.Hu3M\$rTiGtPmTMba9io9PZEfQggLzszRPO7IatCYcrrFS.grLBMrBXGD9NlswdbEID2/46xs.IIXmESc3F28oo0KvG.' root;"

# Adding kernel-devsrc to provide kernel development support on SDK
TOOLCHAIN_TARGET_TASK += "kernel-devsrc"

# Add RT tools only if the RT kernel is selected
CORE_IMAGE_EXTRA_INSTALL += "${@bb.utils.contains_any('PREFERRED_PROVIDER_virtual/kernel', \
                            'linux-qcom-rt linux-qcom-next-rt', 'rt-tests', '', d)}"

# UKI workaround for targets using multi-dtb (dtb provided by the firmware)
## To be removed once https://lists.openembedded.org/g/openembedded-core/message/231436 is accepted
python __anonymous() {
    qcom_dtb_default = d.getVar("QCOM_DTB_DEFAULT")
    # OTA selects a single UKI DTB independently and needs the complete list
    # for raw-kernel deployment and fixed-boot compatibility fingerprints.
    if qcom_dtb_default == "multi-dtb" and d.getVar("QCOM_OTA_ENABLE") != "1":
        d.setVar("KERNEL_DEVICETREE", "")
}

BAD_RECOMMENDATIONS += "systemd-networkd"
