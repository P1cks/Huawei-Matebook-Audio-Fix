# Huawei MateBook Audio Fix (AMD / ES83xx)

[![Verify Patch](https://github.com/P1cks/Huawei-Matebook-Audio-Fix/actions/workflows/verify-patch.yml/badge.svg)](https://github.com/P1cks/Huawei-Matebook-Audio-Fix/actions/workflows/verify-patch.yml)

A DKMS-based fix for the 3.5 mm headphone jack on AMD-based Huawei MateBook laptops with the ES8336/ES8316 codec.

## The Problem

On several Huawei MateBook models the headphone amplifier enable GPIO must be driven **active low**. The in-tree `acp3x-es83xx` machine driver configures it as active high, so after plugging in headphones no audio is routed to the jack.

This project adds an `ES83XX_HP_LOW` quirk for the affected models and ships it as a DKMS module, so the fix:

- installs with one command,
- does not require rebuilding the whole kernel,
- is automatically rebuilt on every kernel update.

## Supported Models

| Model | DMI board name | Jack fix |
|-------|----------------|----------|
| Huawei MateBook 14 (AMD) | BOM-WXX9 | ✅ yes |
| Other Huawei MateBook (AMD) | KLVL-WXX9, KLVL-WXXW, HVY-WXX9 | driver supported, quirk not applied |

Check your model:

```bash
cat /sys/class/dmi/id/board_vendor
cat /sys/class/dmi/id/board_name
```

## Requirements

- Linux with DKMS support (Fedora, Ubuntu/Debian, Arch, ...)
- Kernel headers for the running kernel
- Secure Boot **disabled** (the module is signed with a local DKMS key only)

## Installation

```bash
git clone https://github.com/P1cks/Huawei-Matebook-Audio-Fix.git
cd Huawei-Matebook-Audio-Fix
sudo ./install.sh
```

The script will:

1. check your DMI model (use --force to override),
2. install dkms and kernel headers if missing (dnf / apt / pacman),
3. build and install the snd-acp-legacy-mach module,
4. try to activate it without a reboot.

Reboot if the script asks for it:

sudo reboot

### Manual DKMS installation

```bash
sudo dkms add ./dkms
sudo dkms build huawei-matebook-audio-fix/1.0
sudo dkms install huawei-matebook-audio-fix/1.0
sudo reboot
```

## Verification

After boot, make sure the patched module is in use and the quirk is active:

```bash
modinfo -F filename snd-acp-legacy-mach
sudo dmesg | grep -i 'active'
```

The modinfo path must contain extra/ or updates/, and dmesg must show "headphone gpio 1 active low".

Plug in headphones — audio must switch from speakers to the jack.

## Uninstallation

```bash
sudo ./uninstall.sh
sudo reboot
```

## Troubleshooting

### Module is not loaded after reboot

```bash
sudo dkms status
sudo dmesg | tail -n 20
sudo modprobe snd-acp-legacy-mach
```

If modprobe fails, force a rebuild for the current kernel:

```bash
sudo dkms build huawei-matebook-audio-fix/1.0 -k $(uname -r) --force
sudo dkms install huawei-matebook-audio-fix/1.0 -k $(uname -r) --force
```

### No sound or crosstalk between channels

Some ES83xx codec mixer controls may need tuning for your board. Open alsamixer, adjust the headphone/codec controls, then save the state:

```bash
sudo alsactl store
```

### Secure Boot

With Secure Boot enabled the kernel refuses unsigned modules. Either disable Secure Boot in BIOS, or enroll the DKMS key:

```bash
sudo mokutil --import /var/lib/dkms/mok.pub
```

## How It Works

- patch/ contains a minimal upstream-style patch: it adds the ES83XX_HP_LOW flag and sets the headphone enable GPIO active_low for BOM-WXX9.
- dkms/ contains the patched driver sources plus the module wrapper (acp-legacy-mach.c) required to build it out-of-tree as snd-acp-legacy-mach.
- DKMS installs the module with higher priority than the in-tree one and rebuilds it automatically on kernel upgrades.

## Repository Layout

- install.sh / uninstall.sh — one-command install & rollback
- patch/ — upstream-style kernel patch
- dkms/ — out-of-tree module sources + dkms.conf
- scripts/ — helper tools (checkpatch.pl)
- .github/workflows/ — CI: checkpatch + kernel build matrix

## Upstream Status

The patch is intended for submission to the Linux kernel (ASoC / AMD ACP maintainers). Once accepted upstream, this repository will no longer be needed.

## License

[GPL-2.0](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) — same as the Linux kernel. See [LICENSE](LICENSE).
