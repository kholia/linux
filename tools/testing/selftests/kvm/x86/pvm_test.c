// SPDX-License-Identifier: GPL-2.0-only
/*
 * PVM ABI and execution tests.
 *
 * Run this test with kvm-pvm as the active KVM x86 vendor module.  The test
 * skips when the active module does not implement the PVM virtual MSRs.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <asm/kvm_para.h>
#include <asm/pvm_para.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"

#define PVM_RANGE(pml4_start, pml4_end, pml5_start, pml5_end) \
	(((0xfe00ULL | (pml4_start)) << 0) | \
	 ((0xfe00ULL | (pml4_end)) << 16) | \
	 ((0xfe00ULL | (pml5_start)) << 32) | \
	 ((0xfe00ULL | (pml5_end)) << 48))

static struct pvm_vcpu_struct pvcs __aligned(PAGE_SIZE);

static void pvm_synthetic_cpuid(u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
	asm volatile(".byte 0x0f,0x01,0x3c,0x25,0x50,0x56,0x4d,0xff,0x0f,0xa2"
		     : "+a" (*eax), "=b" (*ebx), "+c" (*ecx), "=d" (*edx)
		     : : "memory");
}

static void guest_code(void)
{
	union {
		u32 regs[3];
		char text[12];
	} signature;
	u64 range;
	u32 eax = KVM_CPUID_SIGNATURE;
	u32 ecx = 0;

	pvm_synthetic_cpuid(&eax, &signature.regs[0], &ecx,
			    &signature.regs[2]);
	signature.regs[1] = ecx;

	GUEST_ASSERT(!memcmp(signature.text, KVM_SIGNATURE,
			     sizeof(signature.text)));
	GUEST_ASSERT_EQ(rdmsr_safe(MSR_PVM_LINEAR_ADDRESS_RANGE, &range), 0);
	GUEST_ASSERT_NE(range, 0);

	/* Exercise a regular virtual MSR read/write from PVM supervisor mode. */
	GUEST_ASSERT_EQ(wrmsr_safe(MSR_PVM_RETU_RIP, 0x400000), 0);
	GUEST_ASSERT_EQ(rdmsr_safe(MSR_PVM_RETU_RIP, &range), 0);
	GUEST_ASSERT_EQ(range, 0x400000);

	GUEST_DONE();
}

static void run_guest(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	for (;;) {
		vcpu_run(vcpu);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_DONE:
			return;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			return;
		default:
			TEST_FAIL("Unexpected ucall %lu", uc.cmd);
		}
	}
}

static void test_pvcs_msr(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	gpa_t pvcs_gpa = addr_gva2gpa(vm, (uintptr_t)&pvcs);

	TEST_ASSERT_EQ(pvcs_gpa & (PAGE_SIZE - 1), 0);
	TEST_ASSERT_EQ(_vcpu_set_msr(vcpu, MSR_PVM_VCPU_STRUCT,
				     pvcs_gpa + 1), 0);
	vcpu_set_msr(vcpu, MSR_PVM_VCPU_STRUCT, pvcs_gpa);
	vcpu_set_msr(vcpu, MSR_PVM_VCPU_STRUCT, 0);
}

static void test_entry_msrs(struct kvm_vcpu *vcpu)
{
	vcpu_set_msr(vcpu, MSR_PVM_EVENT_ENTRY, 0x500000);
	vcpu_set_msr(vcpu, MSR_PVM_RETU_RIP, 0x400000);

	/* Non-canonical and wrapping entry ranges must be rejected. */
	TEST_ASSERT_EQ(_vcpu_set_msr(vcpu, MSR_PVM_EVENT_ENTRY,
				     0x0100000000000000ULL), 0);
	TEST_ASSERT_EQ(_vcpu_set_msr(vcpu, MSR_PVM_EVENT_ENTRY,
				     UINT64_MAX - 255), 0);
	TEST_ASSERT_EQ(_vcpu_set_msr(vcpu, MSR_PVM_RETU_RIP,
				     0x0100000000000000ULL), 0);
	TEST_ASSERT_EQ(_vcpu_set_msr(vcpu, MSR_PVM_RETU_RIP,
				     UINT64_MAX - 1), 0);
}

static void test_linear_range_msr(struct kvm_vcpu *vcpu)
{
	u64 default_range = vcpu_get_msr(vcpu, MSR_PVM_LINEAR_ADDRESS_RANGE);

	TEST_ASSERT(default_range, "PVM default linear-address range is zero");
	vcpu_set_msr(vcpu, MSR_PVM_LINEAR_ADDRESS_RANGE, default_range);

	/* Zero restores the host-provided default range. */
	TEST_ASSERT_EQ(_vcpu_set_msr(vcpu, MSR_PVM_LINEAR_ADDRESS_RANGE, 0), 1);
	TEST_ASSERT_EQ(vcpu_get_msr(vcpu, MSR_PVM_LINEAR_ADDRESS_RANGE),
		       default_range);

	/* Reserved bits, reversed ranges, and empty non-sentinel ranges. */
	TEST_ASSERT_EQ(_vcpu_set_msr(vcpu, MSR_PVM_LINEAR_ADDRESS_RANGE, 1), 0);
	TEST_ASSERT_EQ(_vcpu_set_msr(vcpu, MSR_PVM_LINEAR_ADDRESS_RANGE,
				     PVM_RANGE(0x101, 0x100, 0x1ff, 0x1ff)), 0);
	TEST_ASSERT_EQ(_vcpu_set_msr(vcpu, MSR_PVM_LINEAR_ADDRESS_RANGE,
				     PVM_RANGE(0x100, 0x100, 0x1ff, 0x1ff)), 0);
}

static void check_pvcs_layout(void)
{
	_Static_assert(sizeof(struct pvm_vcpu_struct) == 128, "PVCS ABI size");
	_Static_assert(offsetof(struct pvm_vcpu_struct, event_flags) == 0,
		       "PVCS event_flags offset");
	_Static_assert(offsetof(struct pvm_vcpu_struct, user_cs) == 64,
		       "PVCS user_cs offset");
	_Static_assert(offsetof(struct pvm_vcpu_struct, event_vector) == 70,
		       "PVCS event_vector offset");
	_Static_assert(offsetof(struct pvm_vcpu_struct, user_gsbase) == 72,
		       "PVCS user_gsbase offset");
	_Static_assert(offsetof(struct pvm_vcpu_struct, rip) == 88,
		       "PVCS rip offset");
	_Static_assert(offsetof(struct pvm_vcpu_struct, r11) == 104,
		       "PVCS r11 offset");
}

int main(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	check_pvcs_layout();
	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	/* Unknown MSRs stop KVM_SET_MSRS before processing the first entry. */
	TEST_REQUIRE(_vcpu_set_msr(vcpu, MSR_PVM_VCPU_STRUCT, 0) == 1);

	run_guest(vcpu);
	test_pvcs_msr(vm, vcpu);
	test_linear_range_msr(vcpu);
	test_entry_msrs(vcpu);

	kvm_vm_free(vm);
	return 0;
}
