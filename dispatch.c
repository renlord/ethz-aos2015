#include <kernel.h>
#include <exec.h>
#include <barrelfish_kpi/cpu.h>
#include <paging_kernel_arch.h>
#include <dispatch.h>
#include <wakeup.h>
#include <barrelfish_kpi/syscalls.h>
#include <barrelfish_kpi/lmp.h>
#include <barrelfish_kpi/dispatcher_shared_target.h>
#include <barrelfish_kpi/cpu_arch.h>
#include <barrelfish_kpi/registers_arch.h>
#include <cp15.h>

static inline __attribute__((noreturn))
void do_resume(uint32_t *regs)
{
    STATIC_ASSERT(CPSR_REG ==  0, "");
    STATIC_ASSERT(R0_REG   ==  1, "");
    STATIC_ASSERT(PC_REG   == 16, "");

    // Flush cashes and tlb
    cp15_invalidate_tlb_fn();
    cp15_invalidate_i_and_d_caches();

    __asm volatile(
        // lr = r14, used as tmp register.
        // Load cpsr into lr and move regs to next entry (postindex op)
        // LDR = read word from memory
        //        target register
        //        /   use register containing "regs" as base register
        //       /   /           post index: only base register is used for
        //      /   /           /     addressing and the offset added afterwards
        "ldr    lr, [%[regs]], #4                       \n\t"
        // set spsr_fc to value of lr == regs.cpsr
        // restore cpsr
        //        bits indicating spsr
        //       /         read from register lr
        //      /         /
        "msr    spsr_fc, lr                             \n\t"
        // Restore register r0 to r15,"^" means: cpsr := spsr
        // This is deprecated as LR and PC are both included in this command
        // see ARMv7 TRM A8.6.53
        //               will increment the base pointer
        //              /
        /* "mov lr, %[regs]                                \n\t" */
        /* "ldmia  lr!, {r0-r12}^                          \n\t" */
        /* // Restore stack pointer */
        /* "ldmia  lr!, {r13}                              \n\t" */
        /* // Restore LR and PC */
        /* "ldmia  lr!, {r14-r15}                          \n\t" */
        "ldmia  %[regs], {r0-r15}^                          \n\t"
        // Make sure pipeline is clear
        "nop                          \n\t"
        "nop                          \n\t"
        "nop                          \n\t"
        "nop                          \n\t"
        "nop                          \n\t"
        "nop                          \n\t"
        :: [regs] "r" (regs) : "lr");

    panic("do_resume returned.");
}

/// Ensure context is for user-mode with interrupts enabled.
static inline void
ensure_user_mode_policy(arch_registers_state_t *state)
{
    uintptr_t cpsr_if_mode = CPSR_F_MASK | ARM_MODE_USR;

    if ((state->named.cpsr & (CPSR_IF_MASK | ARM_MODE_MASK)) != cpsr_if_mode) {
        assert(0 == (state->named.cpsr & ARM_MODE_PRIV));
        state->named.cpsr &= CPSR_IF_MASK | ARM_MODE_MASK;
        state->named.cpsr |= cpsr_if_mode;
    }
}

struct dcb *dcb_current;
/**
 * \brief Resume the given user-space snapshot.
 *
 * This function resumes user-space execution by restoring the CPU
 * registers with the ones given in the array, pointed to by 'state'.
 */
uint32_t ctr=0;
void __attribute__ ((noreturn))
resume(arch_registers_state_t *state)
{
    ctr++;
    state->named.rtls = arch_get_thread_register();
    ensure_user_mode_policy(state);

    /*
      This function succeeds the first time executed, i.e.
      when init is started for the first time.
      If we hold the execution here after the first execption, we are still good
    */
    //    while(ctr>1);
    do_resume(state->regs);
}

void wait_for_interrupt(void)
{
    // REVIEW: Timer interrupt could be masked here.

    // Switch to system mode with interrupts enabled. -- OLD
    // Switch to priviledged mode with interrupts enabled.
    __asm volatile(
        //"mov    r0, #" XTR(ARM_MODE_SYS) "              \n\t"
        "mov    r0, #" XTR(ARM_MODE_PRIV) "              \n\t"
        "msr    cpsr_c, r0                              \n\t"
        "0:                                             \n\t"
        "wfi                  \n\t" // works on ARMv7-A
        "b      0b                                      \n\t" ::: "r0");

    panic("wfi returned");
}

/**
 * \brief Switch context to 'dcb'.
 *
 * This is a wrapper function to call the real, hardware-dependent
 * context-switch function to switch to the dispatcher, pointed to by
 * 'dcb'. It also sets 'dcb_current'.
 *
 * \param dcb        Pointer to dispatcher to which to switch context.
 */
static inline void context_switch(struct dcb *dcb)
{
    assert(dcb != NULL);
    assert(dcb->vspace != 0);

    paging_context_switch(dcb->vspace);
}

void __attribute__ ((noreturn)) dispatch(struct dcb *dcb)
{
    // XXX FIXME: Why is this null pointer check on the fast path ?
    // If we have nothing to do we should call something other than dispatch
    if (dcb == NULL) {
        dcb_current = NULL;
        wait_for_interrupt();
    }

    // Don't context switch if we are current already
    if (dcb_current != dcb) {
        context_switch(dcb);
        dcb_current = dcb;
    }

    assert(dcb != NULL);

    dispatcher_handle_t handle = dcb->disp;
    struct dispatcher_shared_generic *disp =
        get_dispatcher_shared_generic(handle);
    arch_registers_state_t *save_area =
        dispatcher_get_save_area(handle);

    assert(disp != NULL);

    debug(SUBSYS_DISPATCH, "resume %.*s at 0x%" PRIx64 "\n", DISP_NAME_LEN,
          disp->name, (uint64_t)registers_get_ip(save_area));
    resume(save_area);
}
