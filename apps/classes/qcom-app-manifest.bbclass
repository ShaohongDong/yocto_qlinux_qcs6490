# SPDX-License-Identifier: MIT

def qcom_app_build_class(d):
    from qcom_apps.bitbake import build_class
    return build_class(d)

# The helper registers app.yaml and its allowlisted inputs as parse-cache
# dependencies before deriving recipe metadata.
python __anonymous() {
    from qcom_apps.bitbake import populate_app_recipe
    populate_app_recipe(d)
}

python qcom_app_install_services() {
    from qcom_apps.bitbake import install_services
    install_services(d)
}

python qcom_app_validate_install() {
    from qcom_apps.bitbake import validate_install
    validate_install(d)
}

do_install[postfuncs] += "qcom_app_install_services qcom_app_validate_install"
do_fetch[vardeps] += "QCOM_APP QCOM_APP_INPUT_DIGEST"
do_configure[vardeps] += "QCOM_APP QCOM_APP_INPUT_DIGEST"
