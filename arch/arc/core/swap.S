/*
 * Copyright (c) 2014-2015 Wind River Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 * @brief Thread context switching
 *
 * This module implements the routines necessary for thread context switching
 * on ARCv2 CPUs.
 *
 * See isr_wrapper.S for details.
 */

#define _ASMLANGUAGE

#include <nano_private.h>
#include <offsets.h>
#include <toolchain.h>
#include <arch/cpu.h>
#include <v2/irq.h>
#include "swap_macros.h"

GTEXT(_Swap)

GDATA(_nanokernel)

/**
 *
 * @brief Initiate a cooperative context switch
 *
 * The _Swap() routine is invoked by various nanokernel services to effect
 * a cooperative context switch.  Prior to invoking _Swap(), the caller
 * disables interrupts via nanoCpuIntLock() and the return 'key' is passed as a
 * parameter to _Swap(). The key is in fact the value stored in the register
 * operand of a CLRI instruction.
 *
 * It stores the intlock key parameter into current->intlock_key.

 * Given that _Swap() is called to effect a cooperative context switch,
 * the caller-saved integer registers are saved on the stack by the function
 * call preamble to _Swap(). This creates a custom stack frame that will be
 * popped when returning from _Swap(), but is not suitable for handling a return
 * from an exception. Thus, the fact that the thread is pending because of a
 * cooperative call to _Swap() has to be recorded via the _CAUSE_COOP code in
 * the relinquish_cause of the thread's tTCS. The _IrqExit()/_FirqExit() code
 * will take care of doing the right thing to restore the thread status.
 *
 * When _Swap() is invoked, we know the decision to perform a context switch or
 * not has already been taken and a context switch must happen.
 *
 * @return may contain a return value setup by a call to fiberRtnValueSet()
 *
 * C function prototype:
 *
 * unsigned int _Swap (unsigned int key);
 *
 */

SECTION_FUNC(TEXT, _Swap)

	/* interrupts are locked, interrupt key is in r0 */

	mov r1, _nanokernel
	ld_s r2, [r1, __tNANO_current_OFFSET]

	/* save intlock key */
	st_s r0, [r2, __tTCS_intlock_key_OFFSET]
	st _CAUSE_COOP, [r2, __tTCS_relinquish_cause_OFFSET]

	/*
	 * Save status32 and blink on the stack before the callee-saved registers.
	 * This is the same layout as the start of an IRQ stack frame.
	 */
	lr r3, [_ARC_V2_STATUS32]
	push_s r3
#ifdef CONFIG_ARC_STACK_CHECKING
	/* disable stack checking during swap */
	bclr r3, r3, _ARC_V2_STATUS32_SC_BIT
	kflag r3
#endif
	push_s blink

	_save_callee_saved_regs

	ld_s r2, [r1, __tNANO_fiber_OFFSET]
	breq r2, 0, _swap_to_the_task

.balign 4
_swap_to_a_fiber:

	ld_s r3, [r2, __tTCS_link_OFFSET]
	b.d _finish_swapping_to_thread    /* always execute delay slot */
	st_s r3, [r1, __tNANO_fiber_OFFSET] /* delay slot */

.balign 4
_swap_to_the_task:

	ld_s r2, [r1, __tNANO_task_OFFSET]

	/* fall through */

.balign 4
_finish_swapping_to_thread:


	/* entering here, r2 contains the new current thread */
#if 0
/* don't save flags in tNANO: slower, error-prone, and might not even give
 * a speed boost where it's supposed to */
	ld_s r3, [r2, __tTCS_flags_OFFSET]
	st_s r3, [r1, __tNANO_flags_OFFSET]
#endif
#ifdef CONFIG_ARC_STACK_CHECKING
	/* Use stack top and down registers from restored context */
	add r3, r2, __tTCS_NOFLOAT_SIZEOF
	sr r3, [_ARC_V2_KSTACK_TOP]
	ld_s r3, [r2, __tTCS_stack_top_OFFSET]
	sr r3, [_ARC_V2_KSTACK_BASE]
#endif
	/* XXX - can be moved to delay slot of _CAUSE_RIRQ ? */
	st_s r2, [r1, __tNANO_current_OFFSET]

	_load_callee_saved_regs

	ld_s r3, [r2, __tTCS_relinquish_cause_OFFSET]

	breq r3, _CAUSE_RIRQ, _swap_return_from_rirq
	nop
	breq r3, _CAUSE_FIRQ, _swap_return_from_firq
	nop

	/* fall through to _swap_return_from_coop */

.balign 4
_swap_return_from_coop:

	ld_s r1, [r2, __tTCS_intlock_key_OFFSET]
	st  0, [r2, __tTCS_intlock_key_OFFSET]
	ld_s r0, [r2, __tTCS_return_value_OFFSET]

	lr ilink, [_ARC_V2_STATUS32]
	bbit1 ilink, _ARC_V2_STATUS32_AE_BIT, _return_from_exc

	pop_s blink /* pc into blink */
	pop_s r3    /* status32 into r3 */
	kflag r3    /* write status32 */

	j_s.d [blink] /* always execute delay slot */
	seti r1       /* delay slot */


.balign 4
_swap_return_from_rirq:
_swap_return_from_firq:

	lr r3, [_ARC_V2_STATUS32]
	bbit1 r3, _ARC_V2_STATUS32_AE_BIT, _return_from_exc_irq

	/* pretend interrupt happened to use rtie instruction */
	lr r3, [_ARC_V2_AUX_IRQ_ACT]
	brne r3,0,_swap_already_in_irq

	or r3,r3,(1<<(CONFIG_NUM_IRQ_PRIO_LEVELS-1)) /* use lowest */
	sr r3, [_ARC_V2_AUX_IRQ_ACT]

_swap_already_in_irq:
	rtie

.balign 4
_return_from_exc_irq:
	_pop_irq_stack_frame
	sub_s sp, sp, 8

_return_from_exc:

	/* put the return address to eret */
	ld ilink, [sp] /* pc into ilink */
	sr ilink, [_ARC_V2_ERET]

	/* put status32 into estatus */
	ld ilink, [sp, 4] /* status32 into ilink */
	sr ilink, [_ARC_V2_ERSTATUS]
	add_s sp, sp, 8
	rtie
