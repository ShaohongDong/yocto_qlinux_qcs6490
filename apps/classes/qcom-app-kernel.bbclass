# SPDX-License-Identifier: MIT

QCOM_APP_KERNEL_ACTIVE ?= "0"
QCOM_APP_CONFIG_FRAGMENTS ?= ""

# The helper registers selected application inputs as parse-cache dependencies.
python __anonymous() {
    from qcom_apps.bitbake import populate_kernel_recipe
    populate_kernel_recipe(d)
}

python do_qcom_app_stage_dts() {
    from qcom_apps.bitbake import stage_device_tree
    stage_device_tree(d)
}

addtask qcom_app_stage_dts after do_unpack before do_patch
do_qcom_app_stage_dts[vardeps] += "QCOM_APP QCOM_APP_INPUT_DIGEST QCOM_APP_KERNEL_ACTIVE"

qcom_app_merge_kernel_config() {
    if [ "${QCOM_APP_KERNEL_ACTIVE}" != "1" ] || [ -z "${QCOM_APP_CONFIG_FRAGMENTS}" ]; then
        return
    fi
    ${S}/scripts/kconfig/merge_config.sh -m -O ${B} \
        ${B}/.config ${QCOM_APP_CONFIG_FRAGMENTS}
    ${KERNEL_CONFIG_COMMAND}
}

qcom_app_verify_kernel_config() {
    if [ "${QCOM_APP_KERNEL_ACTIVE}" != "1" ] || [ -z "${QCOM_APP_CONFIG_FRAGMENTS}" ]; then
        return
    fi
    for fragment in ${QCOM_APP_CONFIG_FRAGMENTS}; do
        while IFS= read -r expected; do
            case "$expected" in
                CONFIG_*=*|'# CONFIG_'*' is not set')
                    if ! grep -Fqx "$expected" ${B}/.config; then
                        bbfatal "application kernel config was not honored: $expected"
                    fi
                    ;;
            esac
        done < "$fragment"
    done
}
