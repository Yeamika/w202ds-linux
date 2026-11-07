# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build:
#   make -C $O ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=$PWD modules
obj-m += w202_dp.o
w202_dp-y := w202_dp_main.o w202_dp_dpu1.o w202_dp_typec.o w202_dp_redriver.o
ccflags-y += -Werror
