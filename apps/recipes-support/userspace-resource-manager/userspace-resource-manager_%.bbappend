# SPDX-License-Identifier: MIT
# URM moves classified processes out of their logind session cgroup. Keep the
# entire Weston launch chain and Wi-Fi client in their session so the standard
# active-seat NetworkManager/Polkit policy can authorize them. Excluding only the
# client/shell is insufficient: a classified startup script can pass its cgroup
# to children before URM restores its parent. Linux comm is 15 bytes long.
do_install:append() {
    if [ "${QCOM_APP}" = "app006-wifi-test" ]; then
        printf '\n%s\n' weston-start.sh weston weston-desktop- wifi-test >> ${D}${sysconfdir}/urm/classifier/classifier-blocklist.txt
    fi
}
