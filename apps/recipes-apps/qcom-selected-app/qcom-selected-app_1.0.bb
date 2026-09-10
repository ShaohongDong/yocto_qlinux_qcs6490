SUMMARY = "Selected QCOM development application"
LICENSE = "MIT"

inherit qcom-app-manifest systemd
inherit_defer ${@qcom_app_build_class(d)}

PV = "1.0+app${QCOM_APP_INPUT_DIGEST}"
PACKAGE_ARCH = "${MACHINE_ARCH}"
