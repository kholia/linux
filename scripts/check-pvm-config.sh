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
		# Kconfig omits symbols whose dependencies are disabled.  An absent
		# symbol is therefore just as disabled as an explicit "not set".
		if grep -Eq "^CONFIG_${symbol}=[ym]$" "$config"; then
			echo "error: CONFIG_${symbol} must be disabled" >&2
			errors=$((errors + 1))
		fi
	done
}

# Boot, OCI runtime, Kubernetes networking, and sandbox requirements.
require_y \
	64BIT BLOCK BLK_DEV VIRTIO_BLK EXT4_FS EXT4_USE_FOR_EXT2 ISO9660_FS \
	FUTEX EPOLL SIGNALFD TIMERFD EVENTFD FHANDLE MEMFD_CREATE \
	FILE_LOCKING SYSVIPC SYSVIPC_SYSCTL SHMEM TMPFS TMPFS_POSIX_ACL \
	NET_NS NAMESPACES VETH BRIDGE BRIDGE_NETFILTER \
	BRIDGE_IGMP_SNOOPING NETFILTER NETFILTER_ADVANCED \
	NETFILTER_INGRESS NETFILTER_NETLINK NETFILTER_NETLINK_QUEUE \
	NETFILTER_NETLINK_LOG NF_CONNTRACK NF_CT_NETLINK \
	NF_CONNTRACK_EVENTS NF_CONNTRACK_TIMEOUT NF_CONNTRACK_TIMESTAMP \
	NETFILTER_XTABLES NETFILTER_XT_MATCH_CONNTRACK \
	NETFILTER_XT_MATCH_COMMENT NETFILTER_XT_MATCH_MULTIPORT \
	NETFILTER_XT_MATCH_MARK NETFILTER_XT_MATCH_STATISTIC \
	NETFILTER_XT_MATCH_ADDRTYPE NETFILTER_XT_MATCH_RECENT \
	NETFILTER_XT_MATCH_LIMIT NETFILTER_XT_TARGET_LOG \
	NETFILTER_XT_TARGET_MARK NETFILTER_XT_TARGET_CONNMARK \
	NETFILTER_XT_MATCH_CONNMARK NF_NAT NF_NAT_MASQUERADE \
	IP_NF_IPTABLES IP_NF_IPTABLES_LEGACY IP_NF_FILTER IP_NF_NAT \
	IP_NF_MANGLE IP_NF_TARGET_MASQUERADE IP_NF_TARGET_REJECT \
	NF_TABLES NF_TABLES_INET NFT_CT NFT_NAT NFT_MASQ NFT_REJECT \
	NFT_COMPAT NFT_NUMGEN NFT_FIB_IPV4 NFT_FIB_IPV6 NFT_LIMIT \
	NFT_LOG NFT_REDIR NFT_TPROXY IP_ADVANCED_ROUTER \
	IP_MULTIPLE_TABLES IP_ROUTE_MULTIPATH NET_IP_TUNNEL IP_VS \
	IP_VS_PROTO_TCP IP_VS_PROTO_UDP IP_VS_RR IP_VS_WRR IP_VS_SH \
	IP_VS_NFCT NET_SCH_HTB NET_CLS_CGROUP CGROUP_NET_PRIO \
	CGROUP_NET_CLASSID DUMMY TUN CGROUPS CGROUP_DEVICE \
	CGROUP_CPUACCT CGROUP_PIDS MEMCG POSIX_MQUEUE \
	POSIX_MQUEUE_SYSCTL SECURITY_LANDLOCK SECCOMP_FILTER \
	HYPERVISOR_GUEST PARAVIRT KVM_GUEST X86_PIE PVM_GUEST \
	PVM_GUEST_MINIMAL PVH PCI PCI_MSI \
	VIRTIO VIRTIO_PCI VIRTIO_PCI_LEGACY \
	VIRTIO_MMIO VIRTIO_MMIO_CMDLINE_DEVICES \
	STRICT_KERNEL_RWX VMAP_STACK RANDOMIZE_BASE RANDOMIZE_MEMORY \
	STACKPROTECTOR_STRONG INIT_STACK_ALL_ZERO \
	INIT_ON_ALLOC_DEFAULT_ON INIT_ON_FREE_DEFAULT_ON \
	FORTIFY_SOURCE HARDENED_USERCOPY PAGE_TABLE_CHECK \
	PAGE_TABLE_CHECK_ENFORCED MITIGATION_PAGE_TABLE_ISOLATION

# NF_NAT_MASQUERADE_IPV4 disappeared; NF_NAT_MASQUERADE is its current,
# protocol-independent replacement and is checked above.

require_n \
	MODULES BLK_DEV_INITRD IKHEADERS BPF_SYSCALL BPF_JIT \
	CGROUP_BPF NET_CLS_BPF NET_ACT_BPF NETFILTER_XT_MATCH_BPF \
	IO_URING AIO CRYPTO KEYS USERFAULTFD KEXEC SUSPEND PM ACPI \
	CPU_FREQ CPU_IDLE SCHED_MC_PRIO X86_INTEL_PSTATE X86_AMD_PSTATE \
	MICROCODE PERF_EVENTS CPU_SUP_CENTAUR CPU_SUP_ZHAOXIN CPU_SUP_HYGON HYPERV XEN \
	VMWARE_GUEST ACRN_GUEST JAILHOUSE_GUEST FW_LOADER \
	IA32_EMULATION MODIFY_LDT_SYSCALL KPROBES FTRACE MAGIC_SYSRQ \
	KUNIT INPUT HID VT VGA_CONSOLE FB FRAMEBUFFER_CONSOLE \
	BACKLIGHT_CLASS_DEVICE LOGO WIRELESS WLAN MEDIA_SUPPORT SOUND DRM \
	USB_SUPPORT PCCARD PARPORT

if grep -q '=m$' "$config"; then
	echo "error: modular options remain in a built-in-only kernel:" >&2
	grep '=m$' "$config" >&2
	errors=$((errors + 1))
fi

# NET and SECCOMP_FILTER select the classic filter engine.  Ensure the
# unavoidable core is present but every userspace/eBPF loading path is gone.
if ! grep -qx 'CONFIG_BPF=y' "$config"; then
	echo "error: networking/seccomp classic filter core is unexpectedly absent" >&2
	errors=$((errors + 1))
fi

# Small CRYPTO_LIB primitives are selected by IPv6, the RNG, and the classic
# filter core.  The configurable crypto API, algorithms, AF_ALG, and hardware
# crypto drivers must all remain absent.
if grep '^CONFIG_CRYPTO_' "$config" | grep -v '^CONFIG_CRYPTO_LIB_' | grep -q '=y$'; then
	echo "error: configurable kernel crypto options are enabled:" >&2
	grep '^CONFIG_CRYPTO_' "$config" | grep -v '^CONFIG_CRYPTO_LIB_' | \
		grep '=y$' >&2
	errors=$((errors + 1))
fi

if test "$errors" -ne 0; then
	echo "PVM config validation failed with $errors error(s)" >&2
	exit 1
fi

echo "PVM config validation passed: required features built in; loaders disabled"
