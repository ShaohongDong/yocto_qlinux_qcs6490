PLATFORM = "talos"
PBT_BUILD_DATE = "260616"

require common.inc

SRC_URI[camxlib.sha256sum] = "3bba0daa67089e529b807961101e920d12109dfbc585901f8db979716094ec5a"
SRC_URI[camx.sha256sum] = "bab2573930951360e58eca159eec8c5777043eb6c1c57725dffae499a75475df"
SRC_URI[chicdk.sha256sum] = "9b08fe81bc9177826fe5d9aff185de16bdb216cab20304bfea60a8c8a5c2eb48"
SRC_URI[camxcommon.sha256sum] = "9e8a7b595242916962bb018f2dffc3136c399f1bf80b0aecd91f4ac97f6293f9"

DEPENDS += "${@bb.utils.contains('DISTRO_FEATURES', 'opencl', 'virtual/libopencl1', '', d)}"
DEPENDS += "${@bb.utils.contains('DISTRO_FEATURES', 'opengl', 'virtual/egl virtual/libgles2', '', d)}"

do_install:append() {
    if ${@bb.utils.contains('DISTRO_FEATURES', 'opengl opencl', 'false', 'true', d)}; then
        rm -f ${D}${libdir}/camx/${PLATFORM}/camera/components/libiwarp*
    fi
}
