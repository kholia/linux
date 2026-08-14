#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

config=${1:-.config}

if test ! -r "$config"; then
	echo "error: cannot read kernel config: $config" >&2
	exit 2
fi

errors=0

require_y()
{
	for symbol in "$@"; do
		if ! grep -qx "CONFIG_${symbol}=y" "$config"; then
			echo "error: CONFIG_${symbol} must be built in (=y)" >&2
			errors=$((errors + 1))
		fi
	done
}

require_m()
{
	for symbol in "$@"; do
		if ! grep -qx "CONFIG_${symbol}=m" "$config"; then
			echo "error: CONFIG_${symbol} must be modular (=m)" >&2
			errors=$((errors + 1))
		fi
	done
}

require_n()
{
	for symbol in "$@"; do
		if grep -Eq "^CONFIG_${symbol}=[ym]$" "$config"; then
			echo "error: CONFIG_${symbol} must be disabled" >&2
			errors=$((errors + 1))
		fi
	done
}

require_y \
	64BIT BLK_DEV_INITRD ACPI EFI EFI_STUB PCI PCI_MSI \
	NVME_CORE BLK_DEV_NVME EXT4_FS \
	TTY VT VT_CONSOLE VGA_CONSOLE FRAMEBUFFER_CONSOLE FB_EFI \
	SERIAL_8250 SERIAL_8250_CONSOLE SERIAL_EARLYCON \
	NET_VENDOR_INTEL NET_VENDOR_AMAZON \
	VIRTUALIZATION KVM KVM_PVM IA32_EMULATION

require_m \
	IGC ENA_ETHERNET DRM DRM_I915 \
	ISO9660_FS FAT_FS VFAT_FS OVERLAY_FS

require_n \
	KVM_INTEL KVM_AMD KVM_SMM KVM_HYPERV KVM_XEN KVM_WERROR \
	RANDOMIZE_BASE

if ! grep -qx 'CONFIG_PHYSICAL_ALIGN=0x1000000' "$config"; then
	echo "error: CONFIG_PHYSICAL_ALIGN must remain 0x1000000" >&2
	errors=$((errors + 1))
fi

if ! grep -qx '# CONFIG_LOCALVERSION_AUTO is not set' "$config"; then
	echo "error: CONFIG_LOCALVERSION_AUTO must be disabled" >&2
	errors=$((errors + 1))
fi

if test "$errors" -ne 0; then
	echo "PVM host config validation failed with $errors error(s)" >&2
	exit 1
fi

echo "PVM host config validation passed: broad NUC and AWS host support retained"
