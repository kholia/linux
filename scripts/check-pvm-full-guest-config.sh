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

require_n()
{
	for symbol in "$@"; do
		if grep -Eq "^CONFIG_${symbol}=[ym]$" "$config"; then
			echo "error: CONFIG_${symbol} must be disabled" >&2
			errors=$((errors + 1))
		fi
	done
}

# PVM boot and built-in root devices.
require_y \
	64BIT HYPERVISOR_GUEST PARAVIRT KVM_GUEST X86_PIE PVM_GUEST PVH \
	ARCH_HAS_EXECMEM_ROX STRICT_KERNEL_RWX DEBUG_WX \
	PCI PCI_MSI VIRTIO VIRTIO_PCI VIRTIO_PCI_LEGACY VIRTIO_MMIO \
	VIRTIO_MMIO_CMDLINE_DEVICES VIRTIO_BLK VIRTIO_NET VIRTIO_CONSOLE \
	DEVTMPFS DEVTMPFS_MOUNT EXT4_FS ISO9660_FS

# General userspace, browser sandboxing, and the OpenShell container baseline.
require_y \
	SYSVIPC POSIX_MQUEUE FUTEX EPOLL SIGNALFD TIMERFD EVENTFD FHANDLE \
	MEMFD_CREATE FILE_LOCKING AIO IO_URING SHMEM TMPFS OVERLAY_FS \
	NAMESPACES UTS_NS TIME_NS IPC_NS USER_NS PID_NS NET_NS \
	CGROUPS MEMCG CGROUP_SCHED CFS_BANDWIDTH CGROUP_PIDS CPUSETS \
	CGROUP_DEVICE UNIX VETH BRIDGE BRIDGE_NETFILTER TUN \
	NETFILTER_ADVANCED NF_CONNTRACK NF_TABLES IP_NF_IPTABLES \
	IP_VS NET_SCH_HTB SECURITY_LANDLOCK SECCOMP_FILTER \
	DRM INPUT HID SOUND USB_SUPPORT

require_n MODULES BLK_DEV_INITRD PVM_GUEST_MINIMAL KASAN

if grep -q '=m$' "$config"; then
	echo "error: modular options remain while CONFIG_MODULES is disabled:" >&2
	grep '=m$' "$config" >&2
	errors=$((errors + 1))
fi

if test "$errors" -ne 0; then
	echo "full PVM guest config validation failed with $errors error(s)" >&2
	exit 1
fi

echo "full PVM guest config validation passed: broad userspace support built in; modules and initrd disabled"
