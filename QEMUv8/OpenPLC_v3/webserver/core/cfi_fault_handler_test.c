/* ============================================================================
 * CFI Fault Handler Trigger & Timing Test
 *
 * Purpose: Repeatedly trigger cfi_fault_handler via LR tampering in
 *          CFI_EPILOGUE_TAG_PAC path, and measure its latency.
 *
 * Build:
 *   aarch64-linux-gnu-gcc -o cfi_fault_handler_test cfi_fault_handler_test.c \
 *       openplc_cfi.o openplc_got.o \
 *       -I. -L. -lcrypto -lteec -lpthread -DCFI_TIMING=1
 *
 * Run:
 *   ./cfi_fault_handler_test
 * ============================================================================ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "openplc_cfi.h"

#define NUM_FAULTS 100

/* ============================================================================
 * Assembly helper: do_fault_ret
 *
 * Embedded via __asm__ — NO compiler-generated prologue/epilogue.
 * Avoids the "naked" attribute which older GCC ignores on aarch64.
 *
 * On entry: x0 = fake_addr
 *           x30 = return address (victim_trigger_fault's bl target)
 *
 * ldp x29, x30, [sp], #16  : pop victim_trigger_fault's frame
 *                             (restores main's x29/x30)
 * mov x30, x0              : overwrite x30 with fake_addr
 * ret                      : jump to fake_addr -> SIGSEGV
 * ============================================================================ */
__asm__(
    ".text\n"
    ".align 2\n"
    ".global do_fault_ret\n"
    ".type do_fault_ret, %function\n"
    "do_fault_ret:\n"
    "    ldp x29, x30, [sp], #16\n"
    "    mov x30, x0\n"
    "    ret\n"
    ".size do_fault_ret, .-do_fault_ret\n"
);

extern void do_fault_ret(uint64_t fake_addr);

/* ============================================================================
 * Victim function: CFI-protected with PAC-BTI epilogue
 * ============================================================================ */
__attribute__((noinline))
static void victim_trigger_fault(void)
{
    uint32_t tag = 0xF0000001U;

    CFI_PROLOGUE_TAG(tag);

    /* simulate some work */
    usleep(10);

    /*
     * Call cfi_epilogue_pac with a WRONG addr.
     * Shadow stack has the correct return addr, but we pass fake.
     * cfi_epilogue_pac detects mismatch, sets:
     *   cfi_recovery_addr = correct return addr
     *   cfi_in_cfi_func = 1
     * then returns 1.
     */
    cfi_epilogue_pac(0xdeadbeefcafebabeULL, tag);

    /*
     * Call assembly helper to pop our frame, overwrite x30, and ret.
     * SIGSEGV will be caught by cfi_fault_handler.
     */
    do_fault_ret(0xdeadbeefcafebabeULL);
}

/* ============================================================================
 * Main
 * ============================================================================ */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("+=========================================================================+\n");
    printf("|           CFI Fault Handler Trigger & Timing Test                      |\n");
    printf("|  Repeatedly triggers cfi_fault_handler via LR tampering + ret fault     |\n");
    printf("+=========================================================================+\n\n");

    printf("[INIT] Initializing CFI...\n");
    if (cfi_init() != 0) {
        fprintf(stderr, "[ERROR] CFI init failed\n");
        return 1;
    }
    printf("[INIT] CFI initialized. cfi_fault_handler is active.\n\n");

    /* Warmup */
    printf("[WARMUP] Warming up shadow stack...\n");
    victim_trigger_fault();
    printf("[WARMUP] Warmup complete.\n\n");

    /* Main test loop */
    printf("[TEST] Triggering %d faults, each caught by cfi_fault_handler...\n",
           NUM_FAULTS);

    for (int i = 0; i < NUM_FAULTS; i++) {
        victim_trigger_fault();

        if ((i + 1) % 10 == 0) {
            printf("  ... %d/%d faults triggered and recovered\n", i + 1, NUM_FAULTS);
        }
    }

    printf("\n[TEST] All %d faults triggered successfully.\n", NUM_FAULTS);

    /* Print status & timing */
    printf("\n");
    cfi_print_status_banner();

#if CFI_TIMING
    printf("\n[TEST] Printing timing statistics...\n");
    cfi_print_timing_stats();
#endif

    cfi_fini();

    printf("\n[SUMMARY] Test complete. Expected: pac_recover_count = %d\n", NUM_FAULTS);
    return 0;
}
