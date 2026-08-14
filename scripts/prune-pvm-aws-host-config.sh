#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

config=${1:-.config}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
config_tool="$script_dir/config"

if test ! -r "$config"; then
	echo "error: cannot read kernel config: $config" >&2
	exit 2
fi

disable_matching()
{
	pattern=$1
	sed -n "s/^CONFIG_\\($pattern\\)=[ym]$/\\1/p" "$config" |
	while IFS= read -r symbol; do
		"$config_tool" --file "$config" --disable "$symbol"
	done
}

# Kconfig silently restores a symbol set to n when any enabled symbol selects
# it.  Ubuntu runner flavors enable different optional selectors, so derive
# all direct reverse selectors from this source tree instead of maintaining a
# runner-specific list.
disable_selectors_of()
{
	target=$1
	find "$source_root" -type f -name 'Kconfig*' \
		-exec awk -v target="$target" '
			FNR == 1 { symbol = "" }
			/^[[:space:]]*(config|menuconfig)[[:space:]]+[A-Z0-9_]+/ {
				symbol = $2
			}
			$1 == "select" && $2 == target && symbol != "" {
				print symbol
			}
		' {} + |
		sort -u |
	while IFS= read -r symbol; do
		"$config_tool" --file "$config" --disable "$symbol"
	done
}

# Ubuntu runner flavors enable different physical NIC vendors.  Nitro needs
# only ENA; disable every inherited vendor menu before restoring Amazon.
disable_matching 'NET_VENDOR_[A-Z0-9_]*'
"$config_tool" --file "$config" --enable NET_VENDOR_AMAZON
"$config_tool" --file "$config" --module ENA_ETHERNET
"$config_tool" --file "$config" --disable JME
"$config_tool" --file "$config" --disable ETHOC

# Keep only ext4 plus the ISO/VFAT media formats and overlayfs requested for
# this host.  These symbols can survive a fragment merge through Ubuntu's
# inherited module selections even when broad filesystem families are off.
"$config_tool" --file "$config" --disable ECRYPT_FS
"$config_tool" --file "$config" --disable HFSPLUS_FS
"$config_tool" --file "$config" --disable DLM
"$config_tool" --file "$config" --disable NLS_CODEPAGE_936
"$config_tool" --file "$config" --disable NLS_CODEPAGE_950
"$config_tool" --file "$config" --disable NLS_ISO8859_15

# Remove selectors which otherwise force debugfs back on after the fragment
# has disabled it.
disable_selectors_of DEBUG_FS
"$config_tool" --file "$config" --disable ZSMALLOC_STAT
"$config_tool" --file "$config" --disable DAMON
"$config_tool" --file "$config" --disable ACPI_EC_DEBUGFS
"$config_tool" --file "$config" --disable NOTIFIER_ERROR_INJECTION
"$config_tool" --file "$config" --disable DEBUG_FS

# Disable inherited consumers which select the crypto core and algorithms.
# None are needed for an ext4-on-EBS Nitro host or for PVM/KVM itself.
"$config_tool" --file "$config" --disable ZSWAP
"$config_tool" --file "$config" --disable FS_ENCRYPTION
"$config_tool" --file "$config" --disable FS_VERITY
"$config_tool" --file "$config" --disable XFRM_ALGO
"$config_tool" --file "$config" --disable XFRM_AH
"$config_tool" --file "$config" --disable XFRM_ESP
"$config_tool" --file "$config" --disable XFRM_IPCOMP
"$config_tool" --file "$config" --disable NET_KEY
"$config_tool" --file "$config" --disable INET_AH
"$config_tool" --file "$config" --disable INET_ESP
"$config_tool" --file "$config" --disable INET_IPCOMP
"$config_tool" --file "$config" --disable IPV6_AH
"$config_tool" --file "$config" --disable IPV6_ESP
"$config_tool" --file "$config" --disable IPV6_IPCOMP
"$config_tool" --file "$config" --disable INET6_IPCOMP
"$config_tool" --file "$config" --disable CEPH_LIB
"$config_tool" --file "$config" --disable MD_RAID456
"$config_tool" --file "$config" --disable DM_RAID
"$config_tool" --file "$config" --disable BCACHE
"$config_tool" --file "$config" --disable DM_VERITY
"$config_tool" --file "$config" --disable DM_INTEGRITY
"$config_tool" --file "$config" --disable MACSEC
"$config_tool" --file "$config" --disable SUNRPC
"$config_tool" --file "$config" --disable RPCSEC_GSS_KRB5
"$config_tool" --file "$config" --disable TCG_TPM
"$config_tool" --file "$config" --disable TRUSTED_KEYS
"$config_tool" --file "$config" --disable ENCRYPTED_KEYS
"$config_tool" --file "$config" --disable KEYS

# Disable inherited optional crypto algorithms and userspace APIs.  Kconfig's
# subsequent olddefconfig pass will restore only primitives selected by a
# retained core feature.
disable_selectors_of CRYPTO
disable_matching 'CRYPTO_[A-Z0-9_]*'
"$config_tool" --file "$config" --disable CRYPTO
"$config_tool" --file "$config" --disable CRYPTO_HW

# Normalize symbols whose type varies between Ubuntu's config and upstream.
"$config_tool" --file "$config" --enable NETFILTER_NETLINK
"$config_tool" --file "$config" --disable ANDROID_BINDER_IPC
"$config_tool" --file "$config" --disable ANDROID_BINDERFS
"$config_tool" --file "$config" --disable MULTIPLEXER
