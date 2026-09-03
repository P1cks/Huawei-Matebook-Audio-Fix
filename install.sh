#!/usr/bin/env bash
# Install the Huawei MateBook audio fix via DKMS.
# Usage: sudo ./install.sh [--force]
set -euo pipefail

PACKAGE="huawei-matebook-audio-fix"
VERSION="1.0"
MODULE="snd-acp-legacy-mach"

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; NC=''
fi
info()  { echo -e "${GREEN}[ OK ]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[FAIL]${NC} $*" >&2; }

[ "$EUID" -ne 0 ] && { error "Run as root: sudo ./install.sh"; exit 1; }

FORCE=0
for arg in "$@"; do
    case "$arg" in
        -f|--force) FORCE=1 ;;
        -h|--help) echo "Usage: sudo ./install.sh [--force]"; exit 0 ;;
        *) warn "Unknown option: $arg" ;;
    esac
done

VENDOR=$(cat /sys/class/dmi/id/board_vendor 2>/dev/null || echo unknown)
MODEL=$(cat /sys/class/dmi/id/board_name 2>/dev/null || echo unknown)
info "Detected system: $VENDOR $MODEL"

case "$MODEL" in
    BOM-WXX9)
        info "Model fully supported (headphone jack fix applies)." ;;
    KLVL-WXX9|KLVL-WXXW|HVY-WXX9)
        warn "Driver supports this model, but the jack fix targets BOM-WXX9." ;;
    *)
        [ "$FORCE" -eq 1 ] && warn "Unknown model, continuing due to --force." ||
            { error "Unsupported model. Use --force to install anyway."; exit 1; } ;;
esac

if ! command -v dkms >/dev/null 2>&1; then
    warn "DKMS not found, installing..."
    if   command -v dnf     >/dev/null; then dnf install -y dkms
    elif command -v apt-get >/dev/null; then apt-get update && apt-get install -y dkms
    elif command -v pacman  >/dev/null; then pacman -S --noconfirm dkms
    else error "Install dkms manually."; exit 1
    fi
fi

if [ ! -d "/lib/modules/$(uname -r)/build" ]; then
    warn "Kernel headers not found, installing..."
    if   command -v dnf     >/dev/null; then dnf install -y "kernel-devel-$(uname -r)"
    elif command -v apt-get >/dev/null; then apt-get install -y "linux-headers-$(uname -r)"
    elif command -v pacman  >/dev/null; then pacman -S --noconfirm linux-headers
    else error "Install kernel headers manually."; exit 1
    fi
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$SCRIPT_DIR/dkms/dkms.conf" ] || { error "dkms/ folder not found."; exit 1; }

if dkms status "$PACKAGE/$VERSION" 2>/dev/null | grep -q installed; then
    info "Removing previous DKMS installation..."
    dkms remove "$PACKAGE/$VERSION" --all >/dev/null
fi

info "Adding DKMS module..."
dkms add "$SCRIPT_DIR/dkms"

info "Building module..."
dkms build "$PACKAGE/$VERSION"

info "Installing module..."
dkms install "$PACKAGE/$VERSION"
depmod -a

info "Trying to activate without reboot..."
if lsmod | grep -q "^snd_acp_legacy_mach "; then
    if rmmod snd_acp_legacy_mach 2>/dev/null; then
        modprobe "$MODULE"
        info "Module reloaded with the fix."
    else
        warn "Stock module is in use. Reboot required."
    fi
else
    modprobe "$MODULE" || warn "modprobe failed, check dmesg."
fi

echo
if lsmod | grep -q "^snd_acp_legacy_mach "; then
    info "Module active. Plug in your headphones and test."
    info "Verify: sudo dmesg | grep -i 'active low'"
else
    warn "Reboot to activate: sudo reboot"
fi
info "Done. DKMS will rebuild the module on kernel updates."
