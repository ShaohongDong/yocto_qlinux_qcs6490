DEPENDS:remove:qcom = "optee-os-tadevkit"
DEPENDS:append:qcom = " optee-os-tadevkit-qcom"

MACHINE_OPTEE_REQUIRE ?= ""
MACHINE_OPTEE_REQUIRE:qcom = "optee-qcom.inc"

require ${MACHINE_OPTEE_REQUIRE}
