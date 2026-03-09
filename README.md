# Huawei MateBook (AMD) Audio Fix Patch

This repository provides a kernel patch to resolve the 3.5mm headphone jack audio issue on Huawei MateBook laptops (specifically `BOM-WXX9` and similar models) running Linux with the ES8336 codec on AMD platforms.

## The Problem
The default Linux kernel `acp3x-es83xx` driver does not correctly identify the Active Low signal required to enable the headphone amplifier on certain Huawei models. As a result, plugging in headphones does not route the audio correctly.

## Detailed Build Guide

### 1. Preparation
You will need the kernel sources (version 6.19 or later is recommended). Install dependencies (for Fedora):
```bash
sudo dnf install ncurses-devel flex bison openssl-devel elfutils-libelf-devel dwarves
```
### 1. Download Kernel Source

Download the kernel source (e.g., version 6.19) from [kernel.org](https://www.kernel.org):
   ```bash
   wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.19.tar.xz
   tar -xf linux-6.19.tar.xz
   cd linux-6.19
   ```
### 2. Apply the Patch

The patch modifies the acp3x-es83xx driver to support the ES83XX_HP_LOW quirk and adds your device to the DMI match table.
```bash
cp /path/to/matebook-sound-fix.patch .
patch -p1 < matebook-sound-fix.patch
```
### 3. Configure the Kernel

To avoid compilation errors and speed up the process, use your current system config and disable unnecessary debug modules.
```bash
cp /boot/config-$(uname -r) .config
make olddefconfig
scripts/config --disable CONFIG_TRUSTED_KEYS
scripts/config --disable CONFIG_DEBUG_INFO
scripts/config --disable CONFIG_DEBUG_INFO_BTF
```
### 4. Build and Install

_Note: This process is CPU-intensive. Adjust -j according to your thread count._
```bash
make -j$(nproc)
sudo make modules_install
sudo make install
sudo grub2-mkconfig -o /boot/grub2/grub.cfg
```
### 5. Final Setup

After rebooting into your new kernel:

1. Open alsamixer.

2. Press F6 to select your sound card.

3. Ensure "Headphone" and "Headphone Power" are unmuted (marked as [OO]).

### Credits

This patch was developed using code and logic from the following projects:

* **[Huawei-Matebook-14-Audio](https://github.com/Xellanox/Huawei-Matebook-14-Audio)** by **Xellanox**: Special thanks for the original implementation of the Active Low logic and GPIO configuration for the ES8336 codec. This project served as the primary foundation for this patch.
* **Linux Kernel Development Team**: For providing the base `acp3x-es83xx` driver.

If this fix helped you, please consider heading over to [Xellanox's repository](https://github.com/Xellanox/Huawei-Matebook-14-Audio) and giving it a star!

### License

This project is licensed under the GPL-2.0 License.
