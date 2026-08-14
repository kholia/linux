// SPDX-License-Identifier: GPL-2.0
/*
 *  prepare to run common code
 *
 *  Copyright (C) 2000 Andrea Arcangeli <andrea@suse.de> SuSE
 */

/* cpu_feature_enabled() cannot be used this early */
#define USE_EARLY_PGTABLE_L5

#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/percpu.h>
#include <linux/start_kernel.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/cc_platform.h>
#include <linux/elf.h>
#include <linux/pgtable.h>

#include <asm/asm.h>
#include <asm/page_64.h>
#include <asm/processor.h>
#include <asm/proto.h>
#include <asm/smp.h>
#include <asm/setup.h>
#include <asm/desc.h>
#include <asm/tlbflush.h>
#include <asm/sections.h>
#include <asm/kdebug.h>
#include <asm/e820/api.h>
#include <asm/bios_ebda.h>
#include <asm/bootparam_utils.h>
#include <asm/microcode.h>
#include <asm/kasan.h>
#include <asm/fixmap.h>
#include <asm/realmode.h>
#include <asm/extable.h>
#include <asm/trapnr.h>
#include <asm/sev.h>
#include <asm/tdx.h>
#include <asm/init.h>
#include <asm/pvm_para.h>

/*
 * Manage page tables very early on.
 */
extern pmd_t early_dynamic_pgts[EARLY_DYNAMIC_PAGE_TABLES][PTRS_PER_PMD];
unsigned int __initdata next_early_pgt;
SYM_PIC_ALIAS(next_early_pgt);
pmdval_t early_pmd_flags = __PAGE_KERNEL_LARGE & ~(_PAGE_GLOBAL | _PAGE_NX);

unsigned int __pgtable_l5_enabled __ro_after_init;
SYM_PIC_ALIAS(__pgtable_l5_enabled);
unsigned int pgdir_shift __ro_after_init = 39;
EXPORT_SYMBOL(pgdir_shift);
SYM_PIC_ALIAS(pgdir_shift);
unsigned int ptrs_per_p4d __ro_after_init = 1;
EXPORT_SYMBOL(ptrs_per_p4d);
SYM_PIC_ALIAS(ptrs_per_p4d);

unsigned long page_offset_base __ro_after_init = __PAGE_OFFSET_BASE_L4;
EXPORT_SYMBOL(page_offset_base);
unsigned long vmalloc_base __ro_after_init = __VMALLOC_BASE_L4;
EXPORT_SYMBOL(vmalloc_base);
unsigned long vmemmap_base __ro_after_init = __VMEMMAP_BASE_L4;
EXPORT_SYMBOL(vmemmap_base);

#ifdef CONFIG_X86_PIE
unsigned long kernel_map_base __ro_after_init = __START_KERNEL_map;
EXPORT_SYMBOL(kernel_map_base);
SYM_PIC_ALIAS(kernel_map_base);
#endif

#ifdef CONFIG_PVM_GUEST
unsigned long cpu_entry_area_base __ro_after_init = RAW_CPU_ENTRY_AREA_BASE;
EXPORT_SYMBOL(cpu_entry_area_base);
unsigned long vmemory_end __ro_after_init = RAW_VMEMORY_END;
EXPORT_SYMBOL(vmemory_end);
#endif
/* Wipe all early page tables except for the kernel symbol map */
static void __init reset_early_page_tables(void)
{
	int k_index = pgd_index((unsigned long)_text);

	memset(early_top_pgt, 0, sizeof(pgd_t) * k_index);
	next_early_pgt = 0;
	write_cr3(__sme_pa_nodebug(early_top_pgt));
}

/* Create a new PMD entry */
bool __init __early_make_pgtable(unsigned long address, pmdval_t pmd)
{
	unsigned long physaddr = address - __PAGE_OFFSET;
	pgdval_t pgd, *pgd_p;
	p4dval_t p4d, *p4d_p;
	pudval_t pud, *pud_p;
	pmdval_t *pmd_p;

	/* Invalid address or early pgt is done ?  */
	if (physaddr >= MAXMEM || read_cr3_pa() != __pa_nodebug(early_top_pgt))
		return false;

again:
	pgd_p = &early_top_pgt[pgd_index(address)].pgd;
	pgd = *pgd_p;

	/*
	 * The use of KERNEL_MAP_BASE rather than __PAGE_OFFSET here is
	 * critical -- __PAGE_OFFSET would point us back into the dynamic
	 * range and we might end up looping forever.  A PIE kernel's image
	 * mapping is relocated, so __START_KERNEL_map is not valid here.
	 */
	if (!pgtable_l5_enabled())
		p4d_p = pgd_p;
	else if (pgd)
		p4d_p = (p4dval_t *)((pgd & PTE_PFN_MASK) + KERNEL_MAP_BASE - phys_base);
	else {
		if (next_early_pgt >= EARLY_DYNAMIC_PAGE_TABLES) {
			reset_early_page_tables();
			goto again;
		}

		p4d_p = (p4dval_t *)early_dynamic_pgts[next_early_pgt++];
		memset(p4d_p, 0, sizeof(*p4d_p) * PTRS_PER_P4D);
		*pgd_p = (pgdval_t)p4d_p - KERNEL_MAP_BASE + phys_base + _KERNPG_TABLE;
	}
	p4d_p += p4d_index(address);
	p4d = *p4d_p;

	if (p4d)
		pud_p = (pudval_t *)((p4d & PTE_PFN_MASK) + KERNEL_MAP_BASE - phys_base);
	else {
		if (next_early_pgt >= EARLY_DYNAMIC_PAGE_TABLES) {
			reset_early_page_tables();
			goto again;
		}

		pud_p = (pudval_t *)early_dynamic_pgts[next_early_pgt++];
		memset(pud_p, 0, sizeof(*pud_p) * PTRS_PER_PUD);
		*p4d_p = (p4dval_t)pud_p - KERNEL_MAP_BASE + phys_base + _KERNPG_TABLE;
	}
	pud_p += pud_index(address);
	pud = *pud_p;

	if (pud)
		pmd_p = (pmdval_t *)((pud & PTE_PFN_MASK) + KERNEL_MAP_BASE - phys_base);
	else {
		if (next_early_pgt >= EARLY_DYNAMIC_PAGE_TABLES) {
			reset_early_page_tables();
			goto again;
		}

		pmd_p = (pmdval_t *)early_dynamic_pgts[next_early_pgt++];
		memset(pmd_p, 0, sizeof(*pmd_p) * PTRS_PER_PMD);
		*pud_p = (pudval_t)pmd_p - KERNEL_MAP_BASE + phys_base + _KERNPG_TABLE;
	}
	pmd_p[pmd_index(address)] = pmd;

	return true;
}

static bool __init early_make_pgtable(unsigned long address)
{
	unsigned long physaddr = address - __PAGE_OFFSET;
	pmdval_t pmd;

	pmd = (physaddr & PMD_MASK) + early_pmd_flags;

	return __early_make_pgtable(address, pmd);
}

void __init do_early_exception(struct pt_regs *regs, int trapnr)
{
	if (trapnr == X86_TRAP_PF &&
	    early_make_pgtable(native_read_cr2()))
		return;

	if (IS_ENABLED(CONFIG_AMD_MEM_ENCRYPT) &&
	    trapnr == X86_TRAP_VC && handle_vc_boot_ghcb(regs))
		return;

	if (trapnr == X86_TRAP_VE && tdx_early_handle_ve(regs))
		return;

	early_fixup_exception(regs, trapnr);
}

/* Don't add a printk in there. printk relies on the PDA which is not initialized 
   yet. */
void __init clear_bss(void)
{
	memset(__bss_start, 0,
	       (unsigned long) __bss_stop - (unsigned long) __bss_start);
	memset(__brk_base, 0,
	       (unsigned long) __brk_limit - (unsigned long) __brk_base);
}

static unsigned long get_cmd_line_ptr(void)
{
	unsigned long cmd_line_ptr = boot_params.hdr.cmd_line_ptr;

	cmd_line_ptr |= (u64)boot_params.ext_cmd_line_ptr << 32;

	return cmd_line_ptr;
}

static void __init copy_bootdata(char *real_mode_data)
{
	char * command_line;
	unsigned long cmd_line_ptr;

	/*
	 * If SME is active, this will create decrypted mappings of the
	 * boot data in advance of the copy operations.
	 */
	sme_map_bootdata(real_mode_data);

	memcpy(&boot_params, real_mode_data, sizeof(boot_params));
	sanitize_boot_params(&boot_params);
	cmd_line_ptr = get_cmd_line_ptr();
	if (cmd_line_ptr) {
		command_line = __va(cmd_line_ptr);
		memcpy(boot_command_line, command_line, COMMAND_LINE_SIZE);
	}

	/*
	 * The old boot data is no longer needed and won't be reserved,
	 * freeing up that memory for use by the system. If SME is active,
	 * we need to remove the mappings that were created so that the
	 * memory doesn't remain mapped as decrypted.
	 */
	sme_unmap_bootdata(real_mode_data);
}

asmlinkage __visible void __init __noreturn x86_64_start_kernel(char * real_mode_data)
{
	int k_index = pgd_index((unsigned long)_text);

	/*
	 * Build-time sanity checks on the kernel image and module
	 * area mappings. (these are purely build-time and produce no code)
	 */
	BUILD_BUG_ON(RAW_MODULES_VADDR < __START_KERNEL_map);
	BUILD_BUG_ON(RAW_MODULES_VADDR - __START_KERNEL_map < KERNEL_IMAGE_SIZE);
	BUILD_BUG_ON(MODULES_LEN + KERNEL_IMAGE_SIZE > 2*PUD_SIZE);
	BUILD_BUG_ON((__START_KERNEL_map & ~PMD_MASK) != 0);
	BUILD_BUG_ON((RAW_MODULES_VADDR & ~PMD_MASK) != 0);
	BUILD_BUG_ON(!(RAW_MODULES_VADDR > __START_KERNEL));
	MAYBE_BUILD_BUG_ON(!(((RAW_MODULES_END - 1) & PGDIR_MASK) ==
				(__START_KERNEL & PGDIR_MASK)));

	cr4_init_shadow();

	/* Kill off the identity-map trampoline */
	reset_early_page_tables();

	if (pgtable_l5_enabled()) {
		page_offset_base	= __PAGE_OFFSET_BASE_L5;
		vmalloc_base		= __VMALLOC_BASE_L5;
		vmemmap_base		= __VMEMMAP_BASE_L5;
	}

	clear_bss();

	/*
	 * This needs to happen *before* kasan_early_init() because latter maps stuff
	 * into that page.
	 */
	clear_page(init_top_pgt);

	/*
	 * SME support may update early_pmd_flags to include the memory
	 * encryption mask, so it needs to be called before anything
	 * that may generate a page fault.
	 */
	sme_early_init();

	kasan_early_init();

	/*
	 * Flush global TLB entries which could be left over from the trampoline page
	 * table.
	 *
	 * This needs to happen *after* kasan_early_init() as KASAN-enabled .configs
	 * instrument native_write_cr4() so KASAN must be initialized for that
	 * instrumentation to work.
	 */
	__native_tlb_flush_global(this_cpu_read(cpu_tlbstate.cr4));

	idt_setup_early_handler();

	pvm_early_setup();

	/* Needed before cc_platform_has() can be used for TDX */
	tdx_early_init();

	copy_bootdata(__va(real_mode_data));

	/*
	 * Load microcode early on BSP.
	 */
	load_ucode_bsp();

	/* set init_top_pgt kernel high mapping*/
	init_top_pgt[k_index] = early_top_pgt[k_index];

	x86_64_start_reservations(real_mode_data);
}

void __init __noreturn x86_64_start_reservations(char *real_mode_data)
{
	/* version is always not zero if it is copied */
	if (!boot_params.hdr.version)
		copy_bootdata(__va(real_mode_data));

	x86_early_init_platform_quirks();

	switch (boot_params.hdr.hardware_subarch) {
	case X86_SUBARCH_INTEL_MID:
		x86_intel_mid_early_setup();
		break;
	default:
		break;
	}

	start_kernel();
}

void early_setup_idt(void)
{
	void *handler = NULL;

	if (IS_ENABLED(CONFIG_AMD_MEM_ENCRYPT)) {
		setup_ghcb();
		handler = vc_boot_ghcb;
	}

	__pi_startup_64_load_idt(handler);
}

#ifdef CONFIG_X86_PIE
void __init startup_64_apply_relocations(struct boot_params *bp)
{
	extern const Elf64_Rela __rela_start[], __rela_end[];
	extern const u64 __relr_start[], __relr_end[];
	u64 va_offset = (u64)rip_rel_ptr(_text) - __START_KERNEL;
	u64 va_shift = bp->kaslr_va_shift;
	u64 *place = NULL;

	if (!va_shift)
		return;

	for (const Elf64_Rela *r = __rela_start; r < __rela_end; r++) {
		if (ELF64_R_TYPE(r->r_info) != R_X86_64_RELATIVE)
			continue;

		place = (u64 *)(r->r_offset + va_offset);
		*place += va_shift;
	}

	for (const u64 *rel = __relr_start; rel < __relr_end; rel++) {
		if ((*rel & 1) == 0) {
			place = (u64 *)(*rel + va_offset);
			*place++ += va_shift;
			continue;
		}

		for (u64 *p = place, r = *rel >> 1; r; p++, r >>= 1)
			if (r & 1)
				*p += va_shift;
		place += 63;
	}
}
#endif

#ifdef CONFIG_PVM_GUEST
extern unsigned long pvm_range_start;
extern unsigned long pvm_range_end;

static bool __init pvm_pgtable_l5_enabled(void)
{
	return native_read_cr4() & X86_CR4_LA57;
}

static void __init detect_pvm_range(void)
{
	unsigned long msr_val;
	unsigned long index_start, index_end;

	msr_val = __rdmsr(MSR_PVM_LINEAR_ADDRESS_RANGE);

	if (pvm_pgtable_l5_enabled()) {
		index_start = (msr_val >> 32) & 0x1ff;
		index_end = (msr_val >> 48) & 0x1ff;
		pvm_range_start = (0xfe00 | index_start) * PGDIR_SIZE;
		pvm_range_end = (0xfe00 | index_end) * PGDIR_SIZE;
	} else {
		index_start = msr_val & 0x1ff;
		index_end = (msr_val >> 16) & 0x1ff;
		pvm_range_start = (0x1fffe00 | index_start) * P4D_SIZE;
		pvm_range_end = (0x1fffe00 | index_end) * P4D_SIZE;

		/*
		 * If the host is in 5-level paging mode and the guest is in
		 * 4-level paging mode, clear the L5 range for migration.
		 */
		if (((msr_val >> 32) & 0x1ff) != 0x1ff)
			msr_val |= (0x1ffUL << 32) | (0x1ffUL << 48);
	}
	native_wrmsrq(MSR_PVM_LINEAR_ADDRESS_RANGE, msr_val);

	/*
	 * early page fault would map page into directing mapping area,
	 * so we should modify 'page_offset_base' here early.
	 */
	page_offset_base = pvm_range_start;
}

void __init pvm_relocate_kernel(struct boot_params *bp)
{
	if (pvm_range_end || !pvm_detect())
		return;

	detect_pvm_range();
	bp->kaslr_va_shift = pvm_range_end - (2UL << 30) - __START_KERNEL_map;
}
#endif
