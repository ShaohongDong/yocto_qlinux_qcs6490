SUMMARY = "Selected QCOM application and its kernel modules"
LICENSE = "MIT"

inherit packagegroup

# packagegroup_dependencies also registers the selected manifest's parse inputs.
python __anonymous() {
    from qcom_apps.bitbake import packagegroup_dependencies
    dependencies = packagegroup_dependencies(d)
    if not dependencies:
        d.setVar("COMPATIBLE_MACHINE", "^$")
    else:
        d.setVar("COMPATIBLE_MACHINE", ".*")
        d.setVar("RDEPENDS:%s" % d.getVar("PN"), dependencies)
}
