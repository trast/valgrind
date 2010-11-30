
/*-------------------------------------------------------------------------*/
/*--- Judgegrind: Valgrind tool designed for online judges   jg_main.c  ---*/
/*-------------------------------------------------------------------------*/

/*
  This file is part of Judgegrind, a Valgrind tool designed to
  perform "runtime" measurements for online judges.  It is linux
  specific.

  Copyright (C) 2010 Thomas Rast
  trast@inf.ethz.ch

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
  02111-1307, USA.

  The GNU General Public License is contained in the file COPYING.
*/

#include <sys/syscall.h>

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_machine.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_options.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_aspacemgr.h"

static unsigned long long reg_ins_count = 0;
static Bool filter_syscalls = False;
static int debug = 0;

static Bool jg_process_cmd_line_option(Char* arg)
{
    if (VG_BOOL_CLO(arg, "--filter-syscalls", filter_syscalls))
	return True;
    if (VG_INT_CLO(arg, "--debug", debug))
	return True;
    else
	return False;
}

static void jg_print_usage(void)
{  
    VG_(printf)(
		"    --filter-syscalls=no|yes  apply syscall whitelist filtering [no]\n"
		);
}

static void jg_print_debug_usage(void)
{  
    VG_(printf)(
		"    --debug=yes               debug: 1 stack, 2 mem checks, 3 instr [no]\n"
		);
}

static void jg_post_clo_init(void)
{
}

/* these must add up to the word size in bits */
#if VG_WORDSIZE == 8
#define PAGE_BITS 12
#define BIG_MAP_BITS 19
#define MAIN_MAP_BITS 15
#define INT_BITS 5
#define SUB_MAP_BITS 13
#else
#define PAGE_BITS 12
#define BIG_MAP_BITS 0
#define MAIN_MAP_BITS 8
#define INT_BITS 5
#define SUB_MAP_BITS 7
#endif

#define BIG_MAP_SIZE ((long long)1<<BIG_MAP_BITS)
#define MAIN_MAP_SIZE ((long long)1<<MAIN_MAP_BITS)
#define SUB_MAP_SIZE ((long long)1<<SUB_MAP_BITS)
#define PAGE_SIZE ((long long)1<<PAGE_BITS)

#define BIG_MAP_SHIFT (PAGE_BITS+SUB_MAP_BITS+INT_BITS+MAIN_MAP_BITS)
#define MAIN_MAP_SHIFT (PAGE_BITS+INT_BITS+SUB_MAP_BITS)
#define SUB_MAP_SHIFT (PAGE_BITS+INT_BITS)
#define BITS_SHIFT PAGE_BITS

#define BIG_MAP_MASK ((long long)-1<<BIG_MAP_SHIFT)
#define MAIN_MAP_MASK (((long long)-1<<MAIN_MAP_SHIFT) & ~BIG_MAP_MASK)
#define SUB_MAP_MASK (((long long)-1<<SUB_MAP_SHIFT) & ~MAIN_MAP_MASK & ~BIG_MAP_MASK)
#define PAGE_MASK 0xfff
#define BITS_MASK ((long long)-1 & ~PAGE_MASK & ~MAIN_MAP_MASK & ~SUB_MAP_MASK & ~BIG_MAP_MASK)

#define IS_BIG_ADDR(x) ((x) & BIG_MAP_MASK)
#define BIG_MAP_ENT(x) (((x) & BIG_MAP_MASK) >> BIG_MAP_SHIFT)
#define MAIN_MAP_ENT(x) (((x) & MAIN_MAP_MASK) >> MAIN_MAP_SHIFT)
#define SUB_MAP_ENT(x) (((x) & SUB_MAP_MASK) >> SUB_MAP_SHIFT)
#define BITS_ENT(x) (((x) & BITS_MASK) >> PAGE_BITS)

/* do not use with side effects for 'x' */
#define SET_BIT(x,i) ((x) = ((x) | (1 << (i))))
#define CLR_BIT(x,i) ((x) = ((x) & ~(1 << (i))))
#define GET_BIT(x,i) ((x) & (1 << (i)))

static int *main_map[MAIN_MAP_SIZE] = {0};
static int ***big_map = 0;

void die_with_bad_access (Addr a);
void die_with_bad_access (Addr a)
{
    VG_(printf)("invalid mem access: %p\n", (void *) a);
    VG_(tool_panic)("user tried invalid address");
}

Addr stack_low = 0;
Addr stack_high = 0;
Addr brk_low = 0;
Addr brk_high = 0;

/* sue me */
extern void VG_(stack_limits) (Addr SP, Addr *start, Addr *end);

static int jg_can_access_mem (Addr base)
{
    int **mm;

    /* be fast in the common cases */
    if (stack_low <= base && stack_high >= base)
	return 1;
    if (brk_low <= base && brk_high >= base)
	return 1;

    if (IS_BIG_ADDR(base)) {
	if (!big_map)
	    return 0;
	mm = big_map[BIG_MAP_ENT(base)];
    } else {
	mm = main_map;
    }

    int *main_ent = mm[MAIN_MAP_ENT(base)];
    if (!main_ent)
	return 0;
    if (GET_BIT(main_ent[SUB_MAP_ENT(base)], BITS_ENT(base)))
	return 1;
    return 0;
}

static VG_REGPARM(0)
void jg_check_mem_access (HWord _base)
{
    Addr base = (Addr) _base;
    if (debug > 1) {
	VG_(printf)("checking:              %016p            = %016llx %016llx %016llx\n",
		    (void *) base,
		    MAIN_MAP_ENT(base), SUB_MAP_ENT(base), BITS_ENT(base));
    }
    if (!jg_can_access_mem(base))
	die_with_bad_access(base);
}

static void jg_check_mem_is_addressable (CorePart part, ThreadId tid, Char* s,
					 Addr base, SizeT size)
{
    jg_check_mem_access(base);
}

static void jg_set_mem_one (Addr base, Bool accessible)
{
    int **mm;

    if (IS_BIG_ADDR(base)) {
	if (!big_map)
	    big_map = VG_(malloc)("jg.big_map.1", BIG_MAP_SIZE*sizeof(int**));
	mm = big_map[BIG_MAP_ENT(base)];
    } else {
	mm = main_map;
    }

    int *main_ent = mm[MAIN_MAP_ENT(base)];
    if (!main_ent) {
	int i;
	main_ent = mm[MAIN_MAP_ENT(base)]
	    = VG_(malloc)("jg.sub_map.1", SUB_MAP_SIZE*sizeof(int));
	for (i = 0; i < SUB_MAP_SIZE; i++)
	    main_ent[i] = 0;
    }
    if (accessible)
	SET_BIT(main_ent[SUB_MAP_ENT(base)], BITS_ENT(base));
    else
	CLR_BIT(main_ent[SUB_MAP_ENT(base)], BITS_ENT(base));
}

void VG_(discard_translations) ( Addr64 guest_start, ULong range,
                                 HChar* who );

static void jg_die_mem (Addr base, SizeT len)
{
    if (debug > 1) {
	VG_(printf)("marking as inaccessible: %016p + %8lu = %016llx %016llx %016llx\n",
		    (void *) base, (unsigned long) len,
		    MAIN_MAP_ENT(base), SUB_MAP_ENT(base), BITS_ENT(base));
    }
    Addr a = base & ~PAGE_MASK;
    Addr end = (base+len) & ~PAGE_MASK;
    while (a < end) {
	jg_set_mem_one(a, 0);
	a += PAGE_SIZE;
    }
    VG_(discard_translations)(0, 0xffffffffffff, "jg_die_mem");
}

static void jg_new_mem (Addr base, SizeT len)
{
    if (debug > 1) {
	VG_(printf)("marking as accessible: %016p + %8lu = %016llx %016llx %016llx\n",
		    (void *) base, (unsigned long) len,
		    MAIN_MAP_ENT(base), SUB_MAP_ENT(base), BITS_ENT(base));
    }
    tl_assert(((BIG_MAP_ENT(base) << BIG_MAP_SHIFT) |
	       (MAIN_MAP_ENT(base) << MAIN_MAP_SHIFT) |
	       (SUB_MAP_ENT(base) << SUB_MAP_SHIFT) |
	       (BITS_ENT(base) << BITS_SHIFT) |
	       (base & PAGE_MASK))
	      == base);
    tl_assert(((BIG_MAP_ENT(base) << BIG_MAP_SHIFT) ^
	       (MAIN_MAP_ENT(base) << MAIN_MAP_SHIFT) ^
	       (SUB_MAP_ENT(base) << SUB_MAP_SHIFT) ^
	       (BITS_ENT(base) << BITS_SHIFT) ^
	       (base & PAGE_MASK))
	      == base);
    Addr a = base & ~PAGE_MASK;
    Addr end = (base+len) & ~PAGE_MASK;
    while (a < end) {
	jg_set_mem_one(a, 1);
	a += PAGE_SIZE;
    }
}

static void jg_new_mem_w_flags(Addr base, SizeT len,
			       Bool rr, Bool ww, Bool xx)
{
    jg_new_mem(base, len);
}

static void jg_new_mem_w_flags_di(Addr base, SizeT len,
				  Bool rr, Bool ww, Bool xx, ULong di_handle)
{
    jg_new_mem(base, len);
}

static void jg_copy_mem (Addr src, Addr dst, SizeT len)
{
    jg_new_mem(dst, len);
}

static void jg_new_mem_w_tid (Addr base, SizeT len, ThreadId tid)
{
    jg_new_mem(base, len);
}

static void jg_new_mem_brk (Addr base, SizeT len, ThreadId tid)
{
    if (!brk_low || brk_low > base)
	brk_low = base;
    if (brk_high < base+len)
	brk_high = base+len;

    if (debug > 0)
	VG_(printf)("brk extent: %p, %p\n", brk_low, brk_high);

    jg_new_mem(base, len);
}

static void jg_determine_stack (Addr base, SizeT len)
{
    /*
     * HACK: aspacemgr puts the stack as a SkAnonC segment and then
     * directly below it a SkResvn segment where it extends the stack
     * if needed.  Find the extents so we can optimize out stack
     * write queries.
     */

    const NSegment *seg = VG_(am_find_nsegment)(base);
    tl_assert(seg != NULL);
    tl_assert(seg->kind == SkAnonC);
    stack_high = seg->end;
    if (!stack_high)
	stack_high--;
    while (seg->kind == SkAnonC) {
	base -= PAGE_SIZE;
	seg = VG_(am_find_nsegment)(base);
	tl_assert(seg != NULL);
    }
    tl_assert(seg->kind == SkResvn);
    stack_low = seg->start + PAGE_SIZE;
    tl_assert(stack_low < stack_high);

    if (debug > 0)
	VG_(printf)("stack extent: %p, %p\n", stack_low, stack_high);

    jg_new_mem(stack_low, stack_high-stack_low);

    /* do not call us again! */
    VG_(track_new_mem_stack)(NULL);
}

static VG_REGPARM(0)
void log_instr(HWord regop)
{
    reg_ins_count += regop;
}

/* assign value to tmp */
static inline
void assign (IRSB* sbOut, IRTemp tmp, IRExpr* expr) {
    addStmtToIRSB(sbOut, IRStmt_WrTmp(tmp,expr));
}

/* build various kinds of expressions */
#define triop(_op, _arg1, _arg2, _arg3) \
                                 IRExpr_Triop((_op),(_arg1),(_arg2),(_arg3))
#define binop(_op, _arg1, _arg2) IRExpr_Binop((_op),(_arg1),(_arg2))
#define unop(_op, _arg)          IRExpr_Unop((_op),(_arg))
#define mkU8(_n)                 IRExpr_Const(IRConst_U8(_n))
#define mkU16(_n)                IRExpr_Const(IRConst_U16(_n))
#define mkU32(_n)                IRExpr_Const(IRConst_U32(_n))
#define mkU64(_n)                IRExpr_Const(IRConst_U64(_n))
#define mkV128(_n)               IRExpr_Const(IRConst_V128(_n))
#define mkexpr(_tmp)             IRExpr_RdTmp((_tmp))

typedef  IRExpr  IRAtom;

static IRAtom* assignNew (IRSB* sbOut, IRType ty, IRExpr* e)
{
    IRTemp   t;
    t = newIRTemp(sbOut->tyenv, ty);
    assign(sbOut, t, e);
    return mkexpr(t);
}

#if VG_WORDSIZE == 8
#define FIELD_Uw U64
#define Ity_Iw Ity_I64
#define Iop_CmpLTwU Iop_CmpLT64U
#define mkUw mkU64
#define Iop_1Utow Iop_1Uto64
#define Iop_Orw Iop_Or64
#define Iop_wto1 Iop_64to1
#else
#define FIELD_Uw U32
#define Ity_Iw Ity_I32
#define Iop_CmpLTwU Iop_CmpLT32U
#define mkUw mkU32
#define Iop_1Utow Iop_1Uto32
#define Iop_Orw Iop_Or32
#define Iop_wto1 Iop_32to1
#endif

static void
instrument_store(IRSB* sbOut, IRExpr* addr)
{
    IRAtom *t1, *t2, *ta;
    IRDirty*   di;

    t1 = assignNew(sbOut, Ity_I1,
		   binop(Iop_CmpLTwU, addr, mkUw((ULong) stack_low)));
    t1 = assignNew(sbOut, Ity_Iw, unop(Iop_1Utow, t1));
    t2 = assignNew(sbOut, Ity_I1,
		   binop(Iop_CmpLTwU, mkUw((ULong) stack_high), addr));
    t2 = assignNew(sbOut, Ity_Iw, unop(Iop_1Utow, t2));
    ta = assignNew(sbOut, Ity_Iw, binop(Iop_Orw, t1, t2));
    ta = assignNew(sbOut, Ity_I1, unop(Iop_wto1, ta));
    di = unsafeIRDirty_0_N(0, "jg_check_mem_access",
			   VG_(fnptr_to_fnentry)(&jg_check_mem_access),
			   mkIRExprVec_1(addr));
    di->guard = ta;
    di->mFx = Ifx_Read;
    di->mAddr = addr;
    di->mSize = 1;
    addStmtToIRSB(sbOut, IRStmt_Dirty(di));
}

static void
maybe_instrument_store(IRSB* sbOut, IRExpr* addr)
{
    IRConst *c = (IRConst*)(addr);
    if (c->tag == Ico_U64) {
	if (jg_can_access_mem((Addr) c->Ico.FIELD_Uw))
	    /* known good address */;
	else
	    /* still need to check, it might be ok by then */
	    instrument_store(sbOut, addr);
    } else {
	instrument_store(sbOut, addr);
    }
}

static
IRSB* jg_instrument (VgCallbackClosure* closure,
		     IRSB* sbIn,
		     VexGuestLayout* layout,
		     VexGuestExtents* vge,
		     VexArchInfo* archinfo_host,
		     IRType gWordTy, IRType hWordTy)
{
    IRDirty*   di;
    IRSB* sbOut;
    IRStmt*    st;
    int i;
    unsigned long long reg_instr = 0;

    if (gWordTy != hWordTy)
	VG_(tool_panic)("host/guest word size mismatch");

    sbOut = deepCopyIRSBExceptStmts(sbIn);

    i = 0;
    while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
	addStmtToIRSB(sbOut, sbIn->stmts[i]);
	i++;
    }

    tl_assert(sbIn->stmts_used > 0);
    tl_assert(i < sbIn->stmts_used);
    st = sbIn->stmts[i];
    tl_assert(Ist_IMark == st->tag);

    for (/*use current i*/; i < sbIn->stmts_used; i++) {

	st = sbIn->stmts[i];
	tl_assert(isFlatIRStmt(st));

	switch (st->tag) {
	case Ist_Exit:
	    if (st->Ist.Exit.jk == Ijk_ClientReq)
		VG_(tool_panic)("client requests not allowed (conditional)");
	    if (reg_instr) {
		di = unsafeIRDirty_0_N(0, "log_instr", VG_(fnptr_to_fnentry)(&log_instr),
				       mkIRExprVec_1(mkIRExpr_HWord(reg_instr)));
		addStmtToIRSB(sbOut, IRStmt_Dirty(di));
		reg_instr = 0;
	    }
	    break;
	case Ist_Store:
	    maybe_instrument_store(sbOut, st->Ist.Store.addr);
	    break;
	case Ist_WrTmp:
	    switch (st->Ist.WrTmp.data->tag) {
	    case Iex_Get:
	    case Iex_GetI:
		reg_instr++;
		break;
	    default:
		break;
	    }
	    break;
	case Ist_Put:
	case Ist_PutI:
	    reg_instr++;
	    break;
	case Ist_CAS:
	    maybe_instrument_store(sbOut, st->Ist.CAS.details->addr);
	    break;
	case Ist_LLSC:
	    if (st->Ist.LLSC.storedata) /* we don't care about LL */
		maybe_instrument_store(sbOut, st->Ist.LLSC.addr);
	    break;
	default:
	    /* nada */
	    break;
	}

	addStmtToIRSB(sbOut, st);
    }

    if (sbOut->jumpkind == Ijk_ClientReq)
	VG_(tool_panic)("client requests not allowed (final)");

    if (reg_instr) {
	di = unsafeIRDirty_0_N(0, "log_instr", VG_(fnptr_to_fnentry)(&log_instr),
			       mkIRExprVec_1(mkIRExpr_HWord(reg_instr)));
	addStmtToIRSB(sbOut, IRStmt_Dirty(di));
    }

    if (debug > 2) {
	for (i = 0; i < sbOut->stmts_used; i++) {
	    ppIRStmt(sbOut->stmts[i]);
	    VG_(printf)("\n");
	}
    }

    return sbOut;
}

/*
 * Some statistics in R said that to estimate user+sys time on my i7
 * 2.67GHz, we should use reg_ins_count*1.222e-10 (and no other
 * factors play a significant role).
 *
 * Since that is still about as arbitrary as it gets, we simplify the
 * score to a rough guess at the execution time in milliseconds, using
 * a weight of 1e-10s = 1e-7ms per register access.
 */
const unsigned long long reg_ins_div = 10000000;

static void jg_fini(Int exitcode)
{
    VG_(umsg)("score: %llu\n", reg_ins_count/reg_ins_div);
}

static void jg_pre_syscall (ThreadId tid, UInt syscallno,
			    UWord* args, UInt nArgs)
{
    int i;

    if (!filter_syscalls)
	return;

    switch(syscallno) {
    /* used by submissions in normal operation */
    case SYS_brk:
    case SYS_exit_group:
    case SYS_fstat:
    case SYS_mmap:
    case SYS_mprotect:
    case SYS_mremap:
    case SYS_munmap:
    case SYS_open: /* note, no close or dup */
    case SYS_read:
    case SYS_write:
    case SYS_access:
    case SYS_uname:
#if __WORDSIZE != 64
    case SYS_fstat64:
    case SYS_mmap2:
#endif
    case SYS_fadvise64:
    /* process startup uses these for some reason */
#if __WORDSIZE == 64
    case SYS_arch_prctl:
#else
    case SYS_set_thread_area:
    case SYS_get_thread_area:
#endif
    /* used in multithreaded programs */
    case SYS_futex:
    case SYS_set_tid_address:
    case SYS_set_robust_list:
    /* used during assert() backtrace printing */
    case SYS_writev:
    case SYS_readv:
    case SYS_kill:
    case SYS_tkill:
    case SYS_tgkill:
    case SYS_gettid:
    case SYS_rt_sigaction:
    case SYS_rt_sigprocmask:
    /* who knows, but harmless */
    case SYS_time:
    case SYS_getrlimit:
#if __WORDSIZE != 64
    case SYS_ugetrlimit:
#endif
	break; /* happy */
    default:
	VG_(printf) ("System call %d intercepted, args:\n", (int) syscallno);
	for (i = 0; i < nArgs; i++)
	    VG_(printf) ("  0x%010llx = %lld\n", (long long) args[i], (long long) args[i]);
	VG_(tool_panic) ("forbidden system call intercepted.\n");
	break;
    }
}

static void jg_post_syscall (ThreadId tid, UInt syscallno,
			     UWord* args, UInt nArgs,
			     SysRes res)
{
    /*
     * We don't need a post-syscall wrapper, but valgrind insists.
     */
}

static void jg_pre_clo_init(void)
{
    VG_(details_name)            ("Judgegrind");
    VG_(details_version)         (NULL);
    VG_(details_description)     ("Count instructions executed");
    VG_(details_copyright_author)(
				  "Copyright (C) 2010 Thomas Rast, based on Lackey and others.");
    VG_(details_bug_reports_to)  ("trast@inf.ethz.ch");

    VG_(basic_tool_funcs)        (jg_post_clo_init,
				  jg_instrument,
				  jg_fini);
    VG_(needs_syscall_wrapper)	(jg_pre_syscall,
			         jg_post_syscall);
    VG_(needs_command_line_options)(jg_process_cmd_line_option,
				    jg_print_usage,
				    jg_print_debug_usage);

    /*
     * We only need to know what memory regions the client might be
     * allowed to write, to rule out writes to valgrind's own memory.
     */
    VG_(track_new_mem_startup)     (jg_new_mem_w_flags_di);
    VG_(track_new_mem_stack_signal)(jg_new_mem_w_tid);
    VG_(track_new_mem_brk)         (jg_new_mem_brk);
    VG_(track_new_mem_mmap)        (jg_new_mem_w_flags_di);
    VG_(track_change_mem_mprotect) (jg_new_mem_w_flags);
    VG_(track_copy_mem_remap)      (jg_copy_mem);

    VG_(track_die_mem_stack_signal)(jg_die_mem);
    VG_(track_die_mem_brk)         (jg_die_mem);
    VG_(track_die_mem_munmap)      (jg_die_mem);

    /*
     * This isn't actually tracking, just bootstrapping, see the
     * actual function
     */
    VG_(track_new_mem_stack)       (jg_determine_stack);

    VG_(track_pre_mem_write)       (jg_check_mem_is_addressable);

    if (debug > 0) {
	VG_(printf)("BIG_MAP_MASK:    %016llx\n", BIG_MAP_MASK);
	VG_(printf)("MAIN_MAP_MASK:   %016llx\n", MAIN_MAP_MASK);
	VG_(printf)("SUB_MAP_MASK:    %016llx\n", SUB_MAP_MASK);
	VG_(printf)("BITS_MASK:       %016llx\n", BITS_MASK);
	VG_(printf)("PAGE_MASK:       %016llx\n", PAGE_MASK);
    }
}

VG_DETERMINE_INTERFACE_VERSION(jg_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                          ---*/
/*--------------------------------------------------------------------*/
