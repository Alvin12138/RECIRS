export TA_DEV_KIT_DIR ?= $(OPTEE_DIR)/optee_os/out/arm/export-ta_arm64
export TEEC_EXPORT ?= $(OPTEE_DIR)/optee_client/out/export/usr
export HOST_CROSS_COMPILE ?= aarch64-linux-gnu-
export TA_CROSS_COMPILE ?= aarch64-linux-gnu-
export CROSS_COMPILE ?= $(HOST_CROSS_COMPILE)

.PHONY: all clean

all:
	$(MAKE) -C ta all
	$(MAKE) -C host all

clean:
	$(MAKE) -C ta clean
	$(MAKE) -C host clean
