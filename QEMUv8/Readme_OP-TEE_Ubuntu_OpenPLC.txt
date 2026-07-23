############ This file is to install OP-TEE with Ubuntu (for install OpenPLC) in REE in QEMUv8

# Download OP-TEE QEMUv8 from https://optee.readthedocs.io/en/latest/building/devices/qemu.html#qemu-v8
cd <optee-project>

# Create a working directory
mkdir -p ubuntu-rootfs-work
cd ubuntu-rootfs-work

# Download Ubuntu 22.04 arm64 base
wget http://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/ubuntu-base-22.04-base-arm64.tar.gz

# Create a blank disk image (8GB, adjustable as needed)
dd if=/dev/zero of=ubuntu-rootfs.ext4 bs=1M count=8192
mkfs.ext4 ubuntu-rootfs.ext4

# Mount and extract Ubuntu base
sudo mkdir -p /mnt/ubuntu-rootfs
sudo mount -o loop ubuntu-rootfs.ext4 /mnt/ubuntu-rootfs
sudo tar -xzf ubuntu-base-22.04-base-arm64.tar.gz -C /mnt/ubuntu-rootfs/

# Configure DNS and apt
sudo mount --bind /dev /mnt/ubuntu-rootfs/dev
sudo mount --bind /proc /mnt/ubuntu-rootfs/proc
sudo mount --bind /sys /mnt/ubuntu-rootfs/sys
sudo mount --bind /dev/pts /mnt/ubuntu-rootfs/dev/pts

# Copy DNS configuration
sudo cp /etc/resolv.conf /mnt/ubuntu-rootfs/etc/resolv.conf

# chroot in and install the necessary software
sudo chroot /mnt/ubuntu-rootfs /bin/bash

# Execute in chroot environment:
apt update
apt install -y systemd systemd-sysv sudo \
    net-tools iputils-ping isc-dhcp-client openssh-server \
    git python3 python3-pip python3-venv python3-dev \
    build-essential libmodbus-dev libcurl4-openssl-dev \
    sqlite3 libsqlite3-dev ca-certificates \
    wget curl vim nano

# Python packages required for installing OpenPLC
pip3 install cython flask flask-login pymodbus pyserial

# Set root password (such as root)
passwd root

# Configure Network (DHCP)
cat > /etc/netplan/01-netcfg.yaml << 'EOF'
network:
  version: 2
  ethernets:
    eth0:
      dhcp4: true
EOF

# Enable systemd-networkd
systemctl enable systemd-networkd
systemctl enable systemd-resolved

# Install OpenPLC
cd /root
git clone https://github.com/thiagoralves/OpenPLC_v3.git
cd OpenPLC_v3
./install.sh linux

exit

# umount
sudo umount /mnt/ubuntu-rootfs/dev/pts
sudo umount /mnt/ubuntu-rootfs/dev
sudo umount /mnt/ubuntu-rootfs/proc
sudo umount /mnt/ubuntu-rootfs/sys
sudo umount /mnt/ubuntu-rootfs

# Copy to project directory
cp ubuntu-rootfs.ext4 <optee-project>/out/bin/ubuntu-rootfs.ext4


###################### Modify in build/qemu_v8.mk
# Add definition at the beginning
UBUNTU_ROOTFS		?= $(BINARIES_PATH)/ubuntu-rootfs.ext4
USE_UBUNTU_ROOTFS	?= y

# Modify qemu_base_args
ifeq ($(PLAT_QEMU),virt)
QEMU_BASE_ARGS += -bios bl1.bin

ifeq ($(USE_UBUNTU_ROOTFS),y)
# Ubuntu rootfs via virtio-blk
QEMU_BASE_ARGS += -drive if=none,file=$(UBUNTU_ROOTFS),format=raw,id=hd0
QEMU_BASE_ARGS += -device virtio-blk-device,drive=hd0
QEMU_BASE_ARGS += -append 'console=ttyAMA0,115200 keep_bootcon root=/dev/vda rw $(QEMU_KERNEL_BOOTARGS)'
else
# Original Buildroot rootfs via initrd
QEMU_BASE_ARGS += -initrd rootfs.cpio.gz
QEMU_BASE_ARGS += -append 'console=ttyAMA0,38400 keep_bootcon root=/dev/vda2 $(QEMU_KERNEL_BOOTARGS)'
endif

QEMU_BASE_ARGS += -kernel Image
QEMU_BASE_ARGS += -machine virt,acpi=off,secure=on,mte=$(QEMU_MTE),gic-version=$(QEMU_GIC_VERSION),virtualization=$(QEMU_VIRT)
endif

# Modify run-only target
.PHONY: run-only
run-only:
ifeq ($(USE_UBUNTU_ROOTFS),y)
	@test -f $(UBUNTU_ROOTFS) || (echo "Error: $(UBUNTU_ROOTFS) not found. Run 'make ubuntu-rootfs' first." && false)
else
	ln -sf $(ROOT)/out-br/images/rootfs.cpio.gz $(BINARIES_PATH)/
endif
	$(call check-terminal)
	$(call run-help)
	$(call launch-terminal,$(QEMU_NW_PORT),"Normal World")
	$(call launch-terminal,$(QEMU_SW_PORT),"Secure World")
	$(call wait-for-ports,$(QEMU_NW_PORT),$(QEMU_SW_PORT))
	cd $(BINARIES_PATH) && $(QEMU_BIN) $(QEMU_RUN_ARGS)

##################### Add in <optee-project>/build/kconfigs/qemu.conf
# Block device
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_PCI=y

# File system
CONFIG_EXT4_FS=y
CONFIG_EXT4_FS_POSIX_ACL=y
CONFIG_EXT4_FS_SECURITY=y

# Network
CONFIG_VIRTIO_NET=y

# 9P fs (Optional, for host sharing)
CONFIG_9P_FS=y
CONFIG_9P_FS_POSIX_ACL=y

# RNG
CONFIG_HW_RANDOM_VIRTIO=y

##################### Modify in build/kconfigs/u-boot_qemu_v8.conf
# Change the original: 
CONFIG_SYS_TEXT_BASE=0x60000000
CONFIG_BOOTCOMMAND="setenv kernel_addr_r 0x42200000 && setenv ramdisk_addr_r 0x46000000 && load hostfs - ${kernel_addr_r} uImage && load hostfs - ${ramdisk_addr_r} rootfs.cpio.uboot &&  setenv bootargs console=ttyAMA0,115200 earlyprintk=serial,ttyAMA0,115200 root=/dev/ram && bootm ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr}"
CONFIG_SEMIHOSTING=y
# into
CONFIG_SYS_TEXT_BASE=0x60000000
CONFIG_BOOTCOMMAND="setenv kernel_addr_r 0x42200000 && load hostfs - ${kernel_addr_r} uImage && setenv bootargs console=ttyAMA0,115200 earlyprintk=serial,ttyAMA0,115200 root=/dev/vda rw && bootm ${kernel_addr_r} - ${fdt_addr}"
CONFIG_SEMIHOSTING=y 


# Then recompile Linux under build
cd <optee-project>/build

make linux-clean
make u-boot-clean

# No need buildroot, but other components still need to be compiled
make u-boot arm-tf optee-os linux qemu

# Ensure that the Ubuntu image is in the correct location
cp ../ubuntu-rootfs-work/ubuntu-rootfs.ext4 ../out/bin/ubuntu-rootfs.ext4

# Ensure that the Ubuntu image exists
ls -la ../out/bin/ubuntu-rootfs.ext4

# Clean up old buildroot rootfs links (to avoid interference)
rm -f ../out/bin/rootfs.cpio.gz

# run
make run-only

##################### normal world error report:
[ TIME ] Timed out waiting for device /dev/ttyAMA0.
[DEPEND] Dependency failed for Serial Getty on ttyAMA0.
# Based on https://dev.t-firefly.com/thread-4903-1-1.html:
# mount ubuntu
sudo mount -o loop ~/Downloads/optee/out/bin/ubuntu-rootfs.ext4 /mnt
cp /mnt/lib/systemd/system/serial-getty@.service /mnt/lib/systemd/system/serial-getty@ttyAMA0.service
rm /mnt/etc/systemd/system/getty.target.wants/serial-getty@ttyAMA0.service
ln -s /mnt/lib/systemd/system/serial-getty@ttyAMA0.service /etc/systemd/system/getty.target.wants/
# Modify: In /mnt/lib/systemd/system/serial-getty@ttyAMA0.service change "%i.device" into "%i"
# Before and After all need to modify, then re: make run-only

##################### normal world network connection cannot be connected
In QEMU_BASE_ARGS change into e1000, i.e., -netdev user,id=vmnic -device e1000,netdev=vmnic

# Network card status display
ip link show
# If eth0 state is Down, change it to UP
sudo ip link set eth0 up

# IP address display
ip addr show
ping 8.8.8.8
# If there is no IPv4 address, use DHCP to automatically generate it
sudo dhclient eth0

##################### make optee_client: Error message that requires pkgconfig and uuid, install optee_cient on Ubuntu
# Download the source code of til Linux
cd ~/Downloads
wget https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.39/util-linux-2.39.tar.xz
tar xf util-linux-2.39.tar.xz
cd util-linux-2.39

# Cross compilation (only compile libuuid)
./configure --host=aarch64-linux-gnu \
            --prefix=/usr/local/aarch64 \
            --disable-all-programs \
            --enable-libuuid

make
sudo make install

# create aarch64-linux-gnu-pkg-config script
sudo tee /usr/local/bin/aarch64-linux-gnu-pkg-config << 'EOF'
#!/bin/bash
export PKG_CONFIG_PATH=/usr/local/aarch64/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
export PKG_CONFIG_LIBDIR=/usr/local/aarch64/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig
/usr/bin/pkg-config "$@"
EOF

sudo chmod +x /usr/local/bin/aarch64-linux-gnu-pkg-config

cd ../optee/optee_client

sudo mount -o loop ~/Downloads/optee/out/bin/ubuntu-rootfs.ext4 /mnt/ubuntu-rootfs
sudo make install DESTDIR=/mnt/ubuntu-rootfs CROSS_COMPILE=aarch64-linux-gnu-      LDFLAGS="-L/usr/local/aarch64/lib"      CFLAGS="-I/usr/local/aarch64/include" 

###################### make optee_examples hello world and copy to ubuntu
cd ../optee_examples/hello_world/host
make     CROSS_COMPILE=aarch64-linux-gnu-     TEEC_EXPORT=~/Downloads/optee/optee_client/out/export/usr     --no-builtin-variables

cd ../ta
make     CROSS_COMPILE=aarch64-linux-gnu-     PLATFORM=vexpress-qemu_virt     TA_DEV_KIT_DIR=~/Downloads/optee/optee_os/out/arm/export-ta_arm64

cd ..
sudo cp host/optee_example_hello_world /mnt/ubuntu-rootfs/usr/bin/
sudo chmod +x /mnt/ubuntu-rootfs/usr/bin/optee_example_hello_world
sudo mkdir -p /mnt/ubuntu-rootfs/lib/optee_armtz/
sudo cp ta/8aaaf200-2450-11e4-abe2-0002a5d5c51b.ta /mnt/ubuntu-rootfs/lib/optee_armtz/
sudo cp ta/include/hello_world_ta.h /mnt/ubuntu-rootfs/usr/include/

sudo chmod 644 /mnt/ubuntu-rootfs/lib/optee_armtz/8aaaf200-2450-11e4-abe2-0002a5d5c51b.ta
sudo umount /mnt/ubuntu-rootfs


####### likewise, make optee_examples/cfi_shadow_stack_multithreads/ta/cfi_ta.c 
####### generated 12345678-1234-1234-1234-56789abcdef0.ta copy to ubuntu /lib/optee_armtz
# copy from normal world
cp /mnt/host/optee_examples/cfi_shadow_stack_multithreads_tag_degrade_forward/ta/12345678-1234-1234-1234-56789abcdef0.ta /lib/optee_armtz/
cp /mnt/host/optee_examples/cfi_shadow_stack_multithreads_tag_degrade_forward/ta/include/cfi_ta.h /usr/include/
chmod 644 /lib/optee_armtz/12345678-1234-1234-1234-56789abcdef0.ta

###################### Host-Guest folder sharing

make QEMU_VIRTFS_ENABLE=y QEMU_USERNET_ENABLE=y #QEMU_VIRTFS_HOST_DIR=<share> run-only
make QEMU_VIRTFS_ENABLE=y QEMU_USERNET_ENABLE=y run-only
# In qemu normal world:
mkdir -p /mnt/host
mount -t 9p -o trans=virtio host /mnt/host

####################### OpenPLC Interactive Server: error binding socket => Address already in use
sudo ss -tulnp | grep :43628
sudo kill -9 <pid>

####################### tee-supplicant reboot
# stop supplicant, and use debug mode to restart
sudo pkill tee-supplicant
sudo tee-supplicant -d


####################### logs generate
stdbuf -o0 -e0 ./core/openplc 2>&1 | tee timing_log.txt
stdbuf -o0 -e0 ./core/openplc 2>&1 | tee timing_log_operator.txt
stdbuf -o0 -e0 ./core/cfi_attack_test 3 2>&1 | tee timing_log_atk_3.txt

####################### normal world new windows
tmux
# new window:
Ctrl + b c
# switch window:
Ctrl + b n 
# shot down window:
Ctrl + b y
