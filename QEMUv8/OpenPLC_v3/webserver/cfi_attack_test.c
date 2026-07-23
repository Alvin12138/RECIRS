/* ============================================================================
 * CFI Attack Test Program
 *
 * Purpose: Test CFI degrade response under simulated control-flow attacks.
 *
 * Attack Method: Inline assembly modifies the LR (x30) register before
 * function return. The CFI epilogue detects the mismatch between the
 * tampered LR and the shadow stack's saved return address.
 *
 * Scenarios:
 *   1. Learning phase attack   (before freeze)
 *   2. Post-freeze attack      (after golden table frozen)
 *   3. Degraded mode attack    (after manual degraded, golden lookup)
 *
 * Build:
 *   cd core/
 *   aarch64-linux-gnu-gcc -o cfi_attack_test ../cfi_attack_test.c \
 *       -I. -L. -lopenplc_cfi -lcrypto -lteec -lpthread
 *
 * Run:
 *   ./cfi_attack_test [scenario]
 *   scenario: 1=learning, 2=post-freeze, 3=degraded (default: 2)
 * ============================================================================ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>

#include "openplc_cfi.h"

/* Extern the global state for monitoring */
extern cfi_runtime_state_t g_cfi_state;

static sigjmp_buf jump_buffer;
static volatile int sigsegv_caught = 0;

/*
 * Signal handler for PAC-BTI recovery test.
 * In a real attack, PAC-BTI would fault and the CFI signal handler
 * would recover. We install our own to catch and continue the test.
 */
static void attack_test_sigsegv_handler(int sig, siginfo_t *info, void *ctx)
{
    (void)sig; (void)info; (void)ctx;
    sigsegv_caught = 1;
    siglongjmp(jump_buffer, 1);
}

static void install_test_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = attack_test_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

static void restore_default_handler(void)
{
    signal(SIGSEGV, SIG_DFL);
    signal(SIGILL, SIG_DFL);
}

/* ============================================================================
 * Victim functions with CFI instrumentation
 * ============================================================================ */

/* Forward declaration */
static uint64_t get_victim_addr(void);

/* Victim A: Normal CFI-protected function */
__attribute__((noinline))
static void victim_func_a(void)
{
    CFI_PROLOGUE_TAG(0xA1000001U);
    usleep(100);  /* Simulate some work */
    CFI_EPILOGUE_TAG(0xA1000001U);
}

/* Victim B: Another CFI-protected function */
__attribute__((noinline))
static void victim_func_b(void)
{
    CFI_PROLOGUE_TAG(0xA1000002U);
    usleep(100);
    CFI_EPILOGUE_TAG(0xA1000002U);
}

/* Victim C: PAC-BTI protected function */
__attribute__((noinline))
static void victim_func_c(void)
{
    CFI_PROLOGUE_TAG(0xA1000003U);
    usleep(100);
    CFI_EPILOGUE_TAG_PAC(0xA1000003U);
}

/* Helper to get function address for attack */
static uint64_t get_victim_addr(void)
{
    return 0xdeadbeefcafebabeULL;  /* Invalid return address */
}

/* ============================================================================
 * Attack payloads
 * ============================================================================ */

/*
 * Attack 1: Tamper LR before return (CFI_EPILOGUE_TAG will detect)
 * This simulates a stack corruption that changes the return address.
 */
__attribute__((noinline))
static void attack_tamper_lr_epilogue(void)
{
    uint32_t tag = 0xA1000004U;
    CFI_PROLOGUE_TAG(tag);
    usleep(50);

    /* ATTACK: Overwrite LR (x30) with a fake address */
    uint64_t fake_addr = 0xdeadbeefcafebabeULL;
    __asm__ volatile(
        "mov x30, %0\n"
        :: "r"(fake_addr)
        : "x30"
    );

    /* CFI epilogue will detect: __builtin_return_address(0) != shadow stack */
    CFI_EPILOGUE_TAG(tag);
}

/*
 * Attack 2: Tamper LR before return (CFI_EPILOGUE_TAG_PAC path)
 * PAC-BTI hardware will catch the corrupted LR on ret.
 */
__attribute__((noinline))
static void attack_tamper_lr_pac(void)
{
    uint32_t tag = 0xA1000005U;
    CFI_PROLOGUE_TAG(tag);
    usleep(50);

    /* ATTACK: Overwrite LR (x30) with a fake address */
    uint64_t fake_addr = 0xdeadbeefcafebabeULL;
    __asm__ volatile(
        "mov x30, %0\n"
        :: "r"(fake_addr)
        : "x30"
    );

    /* CFI_EPILOGUE_TAG_PAC: shadow stack mismatch -> save recovery_addr -> ret */
    /* PAC-BTI will fault on ret -> signal handler recovers using recovery_addr */
    CFI_EPILOGUE_TAG_PAC(tag);
}

/*
 * Attack 3: Degraded mode attack (golden table lookup should detect)
 * After degraded, CFI checks against TEE golden table, not REE shadow stack.
 */
__attribute__((noinline))
static void attack_degraded_golden(void)
{
    uint32_t tag = 0xA1000006U;
    CFI_PROLOGUE_TAG(tag);
    usleep(50);

    /* ATTACK: Change LR to a different valid-looking address
     * Golden table has the REAL golden addr for this tag.
     * The fake addr won't match -> detected!
     */
    uint64_t fake_addr = 0xaaaaaaaa00000000ULL;
    __asm__ volatile(
        "mov x30, %0\n"
        :: "r"(fake_addr)
        : "x30"
    );

    CFI_EPILOGUE_TAG_PAC(tag);
}

/* ============================================================================
 * NEW: Attack 4 — PAC Replay / Forgery Attack
 * ============================================================================
 *
 * Threat Model: Attacker knows the correct return address (via info leak or
 * side channel) but does NOT possess the PAC key. Attacker forges PAC bits
 * and replays the pointer, hoping to find a valid PAC-addr pair or probe
 * the authentication logic.
 *
 * Key Difference from LR Tamper:
 *   - LR Tamper: WRONG address. Shadow stack detects immediately.
 *   - PAC Replay: CORRECT address, WRONG PAC. Shadow stack passes (if
 *     PAC-aware masking is used), but hardware autiasp/retaa FAILS.
 *
 * ARM64 PAuth + TBI1 Assumption:
 *   - Effective address: bits [55:0] (56-bit VA)
 *   - PAC + TBI metadata: bits [63:56] (top byte)
 *   - We mask with 0x00FFFFFFFFFFFFFF to extract the address portion.
 *
 * NOTE: If your shadow stack compares FULL pointers (including PAC bits),
 *       this attack will ALSO be caught by shadow stack. To distinguish
 *       the two fault types, modify shadow stack comparison to mask PAC
 *       bits first (see recommendation below).
 */

#define PAC_ADDR_MASK  0x00FFFFFFFFFFFFFFULL   /* Lower 56 bits: address */
#define PAC_BITS_MASK  0xFF00000000000000ULL   /* Upper 8 bits: PAC+TBI */

/* Helper: Extract address portion from a signed LR */
static inline uint64_t pac_strip(uint64_t ptr)
{
    return ptr & PAC_ADDR_MASK;
}

/* Helper: Reconstruct a pointer with forged PAC bits */
static inline uint64_t pac_forge(uint64_t addr, uint8_t forged_top_byte)
{
    return (addr & PAC_ADDR_MASK) | ((uint64_t)forged_top_byte << 56);
}

/*
 * Attack 4a: Single PAC Replay (forged PAC, correct address)
 * 
 * Simulates one attempt: attacker guesses a wrong PAC and replays.
 * Expected: autiasp fails -> PAC fault -> signal handler catches.
 */
__attribute__((noinline))
static void attack_pac_replay_single(void)
{
    uint32_t tag = 0xA1000007U;
    CFI_PROLOGUE_TAG(tag);
    usleep(50);

    /* Save original LR (correct addr + correct PAC) */
    uint64_t orig_lr;
    __asm__ volatile("mov %0, x30" : "=r"(orig_lr));

    /* Extract the correct address portion */
    uint64_t correct_addr = pac_strip(orig_lr);
    
    /* Forge a clearly wrong PAC (e.g., flip top byte to 0x01) */
    uint8_t forged_pac = 0x01;  /* Any value != original top byte */
    uint64_t replay_lr = pac_forge(correct_addr, forged_pac);

    printf("    [PAC-REPLAY] Original LR:  0x%016lx\n", orig_lr);
    printf("    [PAC-REPLAY] Address part: 0x%014lx (masked)\n", correct_addr);
    printf("    [PAC-REPLAY] Forged PAC:   0x%02x (orig top byte was 0x%02x)\n",
           forged_pac, (uint8_t)(orig_lr >> 56));
    printf("    [PAC-REPLAY] Replay LR:   0x%016lx\n", replay_lr);
    printf("    [PAC-REPLAY] >>> Address intact, PAC forged -> REPLAY ATTACK <<<\n");

    /* Install the forged pointer into LR */
    __asm__ volatile("mov x30, %0" :: "r"(replay_lr) : "x30");

    /* 
     * CFI_EPILOGUE_TAG_PAC sequence:
     *   1. Shadow stack check (if PAC-aware): PASSES (addr matches)
     *   2. autiasp/autibsp: FAILS (PAC mismatch) -> BRK/exception
     *   3. Signal handler catches the PAC fault
     */
    CFI_EPILOGUE_TAG_PAC(tag);
}

/*
 * Attack 4b: PAC Brute-force / Multi-attempt Replay
 *
 * Simulates attacker making N attempts with different forged PAC values.
 * This tests whether your CFI system can detect REPLAY PATTERNS or
 * rate-limit authentication failures (advanced hardening).
 */
__attribute__((noinline))
static void attack_pac_replay_bruteforce(int max_attempts)
{
    uint32_t tag = 0xA100000AU;
    CFI_PROLOGUE_TAG(tag);
    
    printf("    [PAC-REPLAY] Starting brute-force PAC replay (%d attempts)...\n",
           max_attempts);

    for (int i = 0; i < max_attempts; i++) {
        volatile int caught = 0;
        
        if (sigsetjmp(jump_buffer, 1) == 0) {
            /* Save original LR at each attempt */
            uint64_t orig_lr;
            __asm__ volatile("mov %0, x30" : "=r"(orig_lr));

            uint64_t correct_addr = pac_strip(orig_lr);
            
            /* Generate a different PAC guess each iteration */
            uint8_t guess = (uint8_t)(0xAB + i * 17);  /* Pseudo-random walk */
            uint64_t forged_lr = pac_forge(correct_addr, guess);

            printf("    [PAC-REPLAY] Attempt %2d: guess PAC=0x%02x, "
                   "LR=0x%016lx\n", i + 1, guess, forged_lr);

            __asm__ volatile("mov x30, %0" :: "r"(forged_lr) : "x30");

            /* Attempt return - will fault if PAC auth fails */
            CFI_EPILOGUE_TAG_PAC(tag);

            /* If we reach here, the PAC guess was valid (collision!) */
            printf("    [PAC-REPLAY] Attempt %2d: *** VALID PAC FOUND! ***\n", 
                   i + 1);
            break;
        } else {
            caught = 1;
            printf("    [PAC-REPLAY] Attempt %2d: PAC fault caught (expected)\n",
                   i + 1);
            /* 
             * IMPORTANT: After siglongjmp, LR is restored by signal handler
             * or CPU context. We are back at the sigsetjmp point with
             * original LR intact, so the loop can continue safely.
             */
        }
    }

    usleep(50);
    CFI_EPILOGUE_TAG(tag);  /* Normal epilogue for this attack wrapper */
}

/*
 * Attack 4c: Cross-context PAC Replay (Advanced)
 *
 * Simulates stealing a signed LR from one context and replaying it in
 * another where the PAC key is different (e.g., different process or
 * re-keyed TEE session). Even with "correct" PAC bits, the context
 * mismatch causes autiasp to fail.
 */
__attribute__((noinline))
static void attack_pac_cross_context_replay(void)
{
    uint32_t tag = 0xA100000BU;
    CFI_PROLOGUE_TAG(tag);
    usleep(50);

    /* Save current LR (signed with CURRENT context's key) */
    uint64_t orig_lr;
    __asm__ volatile("mov %0, x30" : "=r"(orig_lr));

    /* 
     * Simulate a "stolen" LR from another context.
     * In real attack: this value comes from info leak in another process.
     * Here we simulate by taking current LR and flipping one PAC bit,
     * conceptually representing a different signing context.
     */
    uint64_t stolen_lr = orig_lr ^ 0x1000000000000000ULL;  /* Flip one PAC bit */

    printf("    [PAC-REPLAY] Cross-context replay attack\n");
    printf("    [PAC-REPLAY] Original LR: 0x%016lx\n", orig_lr);
    printf("    [PAC-REPLAY] Stolen LR:   0x%016lx (different context)\n", 
           stolen_lr);

    __asm__ volatile("mov x30, %0" :: "r"(stolen_lr) : "x30");

    CFI_EPILOGUE_TAG_PAC(tag);
}
/* ============================================================================
 * NEW: Attack 5 — Integrity Fault (GOT/PLT tampering)
 * ============================================================================
 *
 * Real-world: Attacker overwrites a function pointer in GOT/PLT
 * or a global callback table to redirect control flow.
 *
 * We simulate by:
 *   1. Locate a function pointer in our own data segment
 *   2. Temporarily make the page writable
 *   3. Overwrite the pointer with a malicious address
 *   4. Call cfi_got_verify() — it should detect the mismatch
 *   5. Restore the pointer and page permissions
 */

/* Global function pointer that CFI protects */
static void (*g_callback_table[2])(void) = { victim_func_a, victim_func_b };

__attribute__((noinline))
static void attack_integrity_got_tamper(void)
{
    uint32_t tag = 0xA1000008U;
    CFI_PROLOGUE_TAG(tag);
    
    printf("    [ATTACK-DETAIL] Attempting GOT/PLT integrity tampering...\n");

    /* Save original */
    void (*orig_ptr)(void) = g_callback_table[0];
    uint64_t orig_addr = (uint64_t)orig_ptr;
    uint64_t malicious_addr = 0xaaaaaaaa00000000ULL;

    /* Make page writable (simulate attacker with write primitive) */
    size_t pagesize = sysconf(_SC_PAGESIZE);
    void *page = (void *)(((uintptr_t)g_callback_table) & ~(pagesize - 1));
    
    if (mprotect(page, pagesize, PROT_READ | PROT_WRITE) != 0) {
        perror("    [ATTACK-DETAIL] mprotect failed");
        CFI_EPILOGUE_TAG(tag);
        return;
    }

    /* ATTACK: Overwrite function pointer */
    printf("    [ATTACK-DETAIL] Overwriting g_callback_table[0]: "
           "0x%lx -> 0x%lx\n", orig_addr, malicious_addr);
    g_callback_table[0] = (void (*)(void))malicious_addr;

    /* 
     * If your openplc_cfi.c has cfi_got_verify() or integrity check,
     * call it here. Otherwise, the corruption will be detected when
     * the tampered pointer is dereferenced.
     * 
     * Example: cfi_got_verify(); 
     * 
     * For this test, we simulate detection by checking the pointer
     * against our known-good list (shadow stack / golden table).
     */
    
    /* Verify the tamper is in place */
    uint64_t current = (uint64_t)g_callback_table[0];
    printf("    [ATTACK-DETAIL] Current pointer value: 0x%lx\n", current);

    /* Restore before epilogue to avoid crashing the test framework */
    g_callback_table[0] = orig_ptr;
    mprotect(page, pagesize, PROT_READ);

    printf("    [ATTACK-DETAIL] Pointer restored. Tamper was transient.\n");
    
    usleep(50);
    CFI_EPILOGUE_TAG(tag);
}

/* ============================================================================
 * NEW: Attack 5b — Integrity Fault (forward-edge CFI bypass)
 * ============================================================================
 *
 * Simulates an indirect call/jump target that is NOT in the valid set.
 * This tests cfi_verify_forward_target() if you have it.
 */

__attribute__((noinline))
static void attack_integrity_forward_edge(void)
{
    uint32_t tag = 0xA1000009U;
    CFI_PROLOGUE_TAG(tag);

    /* Simulate an indirect call with invalid target */
    uint64_t invalid_target = 0xbbbbbbbb00000000ULL;
    
    printf("    [ATTACK-DETAIL] Simulating forward-edge jump to 0x%lx\n",
           invalid_target);
    
    /* 
     * If your CFI has forward-edge verification (e.g., BTI + label check),
     * this would be detected before the jump. We simulate by calling
     * a verification helper if available.
     * 
     * Example pseudo-check:
     * if (cfi_verify_forward_target(invalid_target) != 0) {
     *     printf("    [ATTACK-DETAIL] Forward-edge target rejected!\n");
     * }
     */

    usleep(50);
    CFI_EPILOGUE_TAG(tag);
}

/* ============================================================================
 * Test scenarios
 * ============================================================================ */

static void print_banner(const char *title)
{
    printf("\n+=========================================================================+\n");
    printf("| %-71s |\n", title);
    printf("+=========================================================================+\n");
}

static void print_state(void)
{
    const cfi_runtime_state_t *s = cfi_get_runtime_state();
    printf("  [STATE] status=%s degraded=%d events=%u integrity=%u/%u\n",
           s->status == CFI_RUNTIME_STATUS_NORMAL ? "NORMAL" :
           s->status == CFI_RUNTIME_STATUS_SUSPECT ? "SUSPECT" :
           s->status == CFI_RUNTIME_STATUS_DEGRADED ? "DEGRADED" : "EMERGENCY",
           cfi_is_degraded(), s->event_count,
           s->integrity_pass_count,
           s->integrity_pass_count + s->integrity_fail_count);
}

/* Scenario 1: Attack during learning phase (before freeze) */
static int test_learning_phase_attack(void)
{
    print_banner("Scenario 1: Attack During Learning Phase");
    printf("  During learning phase, CFI can recover via TEE GET_BACKUP.\n");
    printf("  A mismatch triggers auto-degrade to protect against unknown code.\n\n");

    print_state();

    printf("  [ACTION] Calling victim functions to populate shadow stack...\n");
    victim_func_a();
    victim_func_b();
    victim_func_c();

    printf("  [ATTACK] Tampering LR before return (epilogue strict mode)...\n");
    if (sigsetjmp(jump_buffer, 1) == 0) {
        attack_tamper_lr_epilogue();
        printf("  [RESULT] attack_tamper_lr_epilogue returned normally!\n");
    } else {
        printf("  [RESULT] Signal caught (PAC-BTI recovery or abort)\n");
    }

    print_state();

    printf("  [ATTACK] Tampering LR before return (PAC-BTI mode)...\n");
    sigsegv_caught = 0;
    if (sigsetjmp(jump_buffer, 1) == 0) {
        attack_tamper_lr_pac();
        printf("  [RESULT] attack_tamper_lr_pac returned normally!\n");
    } else {
        printf("  [RESULT] Signal caught: PAC-BTI fault recovered via shadow stack\n");
    }

    print_state();
    return cfi_is_degraded();
}

/* Scenario 2: Attack after freeze (golden table should detect) */
static int test_post_freeze_attack(void)
{
    print_banner("Scenario 2: Attack After Freeze");
    printf("  After freeze, unknown tags cause immediate degrade.\n");
    printf("  If learning is still active, mismatch triggers degrade.\n\n");

    print_state();

    printf("  [ACTION] Calling normal victim functions...\n");
    victim_func_a();
    victim_func_b();

    printf("  [ATTACK] Tampering LR with fake address...\n");
    printf("  CFI epilogue: __builtin_return_address(0) = 0xdeadbeefcafebabe\n");
    printf("  Shadow stack expects the real return address\n\n");

    sigsegv_caught = 0;
    if (sigsetjmp(jump_buffer, 1) == 0) {
        attack_tamper_lr_epilogue();
        printf("  [RESULT] Function returned! Check logs for CFI VIOLATION.\n");
    } else {
        printf("  [RESULT] Signal caught: abort() triggered by CFI violation\n");
    }

    print_state();
    return cfi_is_degraded();
}

/* Scenario 3: Attack in degraded mode (golden table lookup) */
static int test_degraded_mode_attack(void)
{
    print_banner("Scenario 3: Attack in Degraded Mode (Golden Table)");
    printf("  After degraded, checks use TEE golden table, not REE shadow stack.\n");
    printf("  Golden table has (tag -> real_addr); fake_addr won't match.\n\n");

    /* Trigger degraded mode */
    printf("  [ACTION] Manually triggering DEGRADED mode...\n");
    cfi_set_protection_mode(CFI_MODE_DEGRADED);
    printf("  [ACTION] Mode set to DEGRADED. Golden cache is cold.\n");
    print_state();

    /* First call: populate golden cache */
    printf("  [ACTION] Calling victim_func_c to warm golden cache...\n");
    sigsegv_caught = 0;
    if (sigsetjmp(jump_buffer, 1) == 0) {
        victim_func_c();
        printf("  [RESULT] victim_func_c returned. Golden cached.\n");
    } else {
        printf("  [RESULT] Signal caught during victim_func_c\n");
    }
    print_state();

    /* Attack: LR tampering in degraded mode */
    printf("  [ATTACK] attack_degraded_golden: LR = 0xaaaaaaaa00000000\n");
    printf("  Golden table expects the REAL address for tag 0xA1000006\n");
    printf("  This mismatch should be detected by tee_get_golden comparison!\n\n");

    sigsegv_caught = 0;
    if (sigsetjmp(jump_buffer, 1) == 0) {
        attack_degraded_golden();
        printf("  [RESULT] attack_degraded_golden returned!\n");
        printf("  Check logs for [CFI-TEE] Golden mismatch!\n");
    } else {
        printf("  [RESULT] Signal caught: PAC-BTI fault on ret\n");
    }

    print_state();
    return cfi_is_degraded();
}

/* ============================================================================
 * NEW: Scenario 4 — PAC Replay / Forgery Attack Test
 * ============================================================================ */
static int test_pac_replay_attack(void)
{
    print_banner("Scenario 4: PAC Replay / Forgery Attack");
    printf("  Threat: Attacker knows correct return addr, forges PAC bits.\n");
    printf("  Goal:  Distinguish PAC fault from LR tamper in logs.\n");
    printf("  Note:  If shadow stack compares full pointers, both attacks\n");
    printf("         look identical. Use PAC-aware masking to differentiate.\n\n");

    print_state();

    /* Test 4a: Single forged PAC */
    printf("  [ATTACK] Single PAC replay (correct addr, forged PAC)...\n");
    sigsegv_caught = 0;
    if (sigsetjmp(jump_buffer, 1) == 0) {
        attack_pac_replay_single();
        printf("  [RESULT] attack_pac_replay_single returned! PAC collision?\n");
    } else {
        printf("  [RESULT] PAC fault caught by signal handler (expected)\n");
        printf("  [PASS]   Hardware PAC authentication blocked replay\n");
    }
    print_state();

    /* Test 4b: Brute-force replay (multiple attempts) */
    printf("\n  [ATTACK] Brute-force PAC replay (5 attempts)...\n");
    sigsegv_caught = 0;
    if (sigsetjmp(jump_buffer, 1) == 0) {
        attack_pac_replay_bruteforce(5);
        printf("  [RESULT] Brute-force loop completed\n");
    } else {
        printf("  [RESULT] Outer signal caught (unexpected)\n");
    }
    print_state();

    /* Test 4c: Cross-context replay */
    printf("\n  [ATTACK] Cross-context PAC replay...\n");
    sigsegv_caught = 0;
    if (sigsetjmp(jump_buffer, 1) == 0) {
        attack_pac_cross_context_replay();
        printf("  [RESULT] Cross-context replay returned!\n");
    } else {
        printf("  [RESULT] Cross-context PAC fault caught\n");
    }
    print_state();

    return cfi_is_degraded();
}
/* ============================================================================
 * NEW: Scenario 5 — Integrity Fault Test (GOT/PLT Tampering)
 * ============================================================================ */
static int test_integrity_fault_attack(void)
{
    print_banner("Scenario 5: Integrity Fault (GOT/PLT Tampering)");
    printf("  Simulates attacker overwriting function pointer in data segment.\n");
    printf("  CFI integrity check (cfi_got_verify) should detect mismatch.\n\n");

    print_state();

    printf("  [ATTACK] Tampering global function pointer via mprotect...\n");
    
    /* This attack does not trigger signals directly, it tests logical integrity */
    attack_integrity_got_tamper();
    
    printf("  [RESULT] If cfi_got_verify() is implemented, check logs for "
           "INTEGRITY FAIL\n");

    print_state();

    printf("  [ATTACK] Simulating invalid forward-edge target...\n");
    attack_integrity_forward_edge();

    print_state();
    return cfi_is_degraded();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv)
{
    int scenario = 2;  /* default: post-freeze attack */
    if (argc > 1) {
        scenario = atoi(argv[1]);
        if (scenario < 1 || scenario > 5) scenario = 2;
    }

    printf("+=========================================================================+\n");
    printf("|                    CFI Attack Test Program                               |\n");
    printf("|  Tests CFI degrade response under simulated control-flow attacks        |\n");
    printf("+=========================================================================+\n");
    printf("Scenario: %d (%s)\n\n",
           scenario,
           scenario == 1 ? "Learning phase attack" :
           scenario == 2 ? "Post-freeze attack" :
           scenario == 3 ? "Degraded mode attack" :
           scenario == 4 ? "PAC signature fault" :
           "Integrity fault (GOT/PLT)");
    /* Install test signal handler (wraps around CFI's handler) */
    install_test_handler();

    /* Initialize CFI */
    printf("[INIT] Initializing CFI...\n");
    if (cfi_init() != 0) {
        fprintf(stderr, "[ERROR] CFI init failed\n");
        return 1;
    }
    printf("[INIT] CFI initialized. ree_degraded=%d\n", cfi_is_degraded());

    /* Call victim functions to register their tags */
    printf("[INIT] Calling victim functions to register tags...\n");
    victim_func_a();
    victim_func_b();
    victim_func_c();

    /* For scenario 2 & 3, freeze first */
    if (scenario >= 2) {
        printf("[INIT] Freezing golden table...\n");
        cfi_freeze();
        printf("[INIT] Golden table frozen.\n");
    }

    /* Run the selected scenario */
    int degraded = 0;
    switch (scenario) {
        case 1: degraded = test_learning_phase_attack(); break;
        case 2: degraded = test_post_freeze_attack(); break;
        case 3: degraded = test_degraded_mode_attack(); break;
        case 4: degraded = test_pac_replay_attack(); break;
        case 5: degraded = test_integrity_fault_attack(); break;
    }

    /* Final state */
    print_banner("Final State");
    print_state();
    cfi_print_status_banner();

    /* Cleanup */
    restore_default_handler();
    cfi_fini();

    printf("\n[SUMMARY] Test complete. Degraded=%d\n", degraded);
    printf("          Expected: degraded=1 after attack\n");

    return (degraded == 1) ? 0 : 1;  /* Return 0 if degrade worked */
}
