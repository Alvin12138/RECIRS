#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <tee_client_api.h>
#include <cfi_ta.h>
#include "openplc_cfi.h"
#include "openplc_got.h"

/* Socket headers for Runtime Stage Layer 3 (Operator Command Server) */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * Timing Measurement Framework (clock_gettime CLOCK_MONOTONIC)
 * ============================================================================
 * Usage:
 *   CFI_T_START();  // At the beginning of the function
 *   ... // Tested code
 *   CFI_T_END("stat_name");  // Record the corresponding statistical term at the function exit
 *
 * Aggregate statistical items by name and automatically calculate count/total/avg/max/min
 * Generate unique variable names through __LINE__ and support nested measurements
 * ============================================================================ */
#if CFI_TIMING

#include <time.h>

typedef struct {
    const char *name;       /* Statistical item name */
    uint64_t total_ns;      /* Total time consumption (nanoseconds) */
    uint64_t count;         /* call count */
    uint64_t max_ns;        /* Maximum single time consumption */
    uint64_t min_ns;        /* Minimum single time consumption */
} cfi_timing_entry_t;

#define CFI_TIMING_MAX_ENTRIES 32
static cfi_timing_entry_t cfi_timing_entries[CFI_TIMING_MAX_ENTRIES];
static int cfi_timing_entry_count = 0;
static pthread_mutex_t cfi_timing_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Local nested timing stack for threads - supports multiple nested measurements within the same function */
#define CFI_TIMING_MAX_NEST 8
static __thread uint64_t cfi_timing_stack[CFI_TIMING_MAX_NEST];
static __thread int cfi_timing_sp = 0;

/* ========== Runtime Stage Forward Declarations ========== */
/* These are defined at the end of the file in the Runtime Stage section */
extern cfi_runtime_state_t g_cfi_state;
extern cfi_protection_mode_t current_mode;
extern volatile int cfi_safety_stop_requested;
extern volatile int operator_server_running;
#define CFI_ATTACK_THRESHOLD 3   /* Each level can tolerate up to 3 attacks before automatically downgrading */

#define GOLDEN_CACHE_SIZE 128
typedef struct {
    uint32_t tag;
    uint64_t golden_addr;
    int valid;
} golden_cache_entry_t;

static __thread golden_cache_entry_t golden_cache[GOLDEN_CACHE_SIZE];
static __thread int golden_cache_count = 0;

/** Get the current monotonic clock time (nanoseconds) */
static inline uint64_t cfi_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/** Find or create statistical entries */
static cfi_timing_entry_t* cfi_timing_find_or_create(const char *name) {
    for (int i = 0; i < cfi_timing_entry_count; i++) {
        if (strcmp(cfi_timing_entries[i].name, name) == 0) {
            return &cfi_timing_entries[i];
        }
    }
    if (cfi_timing_entry_count >= CFI_TIMING_MAX_ENTRIES) {
        return NULL;
    }
    cfi_timing_entry_t *entry = &cfi_timing_entries[cfi_timing_entry_count++];
    entry->name = name;
    entry->total_ns = 0;
    entry->count = 0;
    entry->max_ns = 0;
    entry->min_ns = UINT64_MAX;
    return entry;
}

/** Record a measurement result once */
static void cfi_timing_record(const char *name, uint64_t elapsed_ns) {
    pthread_mutex_lock(&cfi_timing_mutex);
    cfi_timing_entry_t *entry = cfi_timing_find_or_create(name);
    if (entry) {
        entry->total_ns += elapsed_ns;
        entry->count++;
        if (elapsed_ns > entry->max_ns) entry->max_ns = elapsed_ns;
        if (elapsed_ns < entry->min_ns) entry->min_ns = elapsed_ns;
    }
    pthread_mutex_unlock(&cfi_timing_mutex);
}

/** Start timing: Push the current time into the local nested stack of the thread */
#define CFI_T_START() do { \
    if (cfi_timing_sp < CFI_TIMING_MAX_NEST) \
        cfi_timing_stack[cfi_timing_sp++] = cfi_get_time_ns(); \
} while(0)

/** End timing: Pop up the time from the top of the stack, calculate the difference and record it */
#define CFI_T_END(name) do { \
    if (cfi_timing_sp > 0) { \
        uint64_t _cfi_elapsed = cfi_get_time_ns() - cfi_timing_stack[--cfi_timing_sp]; \
        cfi_timing_record(name, _cfi_elapsed); \
    } \
} while(0)

#else /* !CFI_TIMING */

#define CFI_T_START() ((void)0)
#define CFI_T_END(name) ((void)0)

#endif /* CFI_TIMING */

/* ============================================================================
 * OpenPLC CFI - Hybrid Shadow Stack (REE fast path + TEE golden backup)
 * + Forward-Edge CFI with Runtime Learning (auto-register on first use)
 *
 * Forward-edge design philosophy (symmetric to backward-edge):
 * - Backward: push_backup learns return addr on first call, check verifies it
 * - Forward: verify_target learns indirect target on first call, checks thereafter
 * - NO pre-compiled address tables needed
 * - NO double compilation needed
 * ============================================================================ */

static TEEC_Context ctx;
TEEC_Session sess;
static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int initialized = 0;
static pthread_key_t cfi_tls_key;

/* Global downgrade flag */
static volatile int ree_degraded = 0;
static pthread_mutex_t degrade_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Forward edge: REE target cache (each thread is independent to avoid lock contention) */
#define TARGET_CACHE_SIZE 128
typedef struct {
    uint64_t addr;
    uint32_t label;
    int valid;
} target_cache_entry_t;

typedef struct {
    target_cache_entry_t entries[TARGET_CACHE_SIZE];
    int count;
} target_cache_t;

/* REE Shadow Stack Entry */
typedef struct {
    uint64_t ret_addr;
    uint32_t tag;
    uint32_t seq;
} ree_entry_t;

#define REE_STACK_DEPTH 1024
typedef struct {
    ree_entry_t stack[REE_STACK_DEPTH];
    int sp;
    int tid;
    int registered;
} ree_shadow_stack_t;

/* ========== Thread local data ========== */
typedef struct {
    ree_shadow_stack_t *rss;
    target_cache_t target_cache;
} cfi_thread_data_t;

/* Backward edge tag cache */
#define TAG_CACHE_SIZE 64
typedef struct {
    uint32_t tags[TAG_CACHE_SIZE];
    int count;
} tag_cache_t;

/* ========== GOT Protecting data ========== */
static got_entry_t *got_entries = NULL;
static int got_entry_count = 0;
static int got_initialized = 0;

/* ========== Approach 3: Learning + Freeze ========== */
static volatile int learning_mode_active = 1;  /* At startup, it defaults to the learning phase */
static pthread_mutex_t learning_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t freeze_breach_count = 0;

/* ========== PAC-BTI Fault recovery status ========== */
static __thread uint64_t cfi_recovery_addr = 0;   /* Expected return address (for signal processors) */
static __thread uint32_t cfi_recovery_tag = 0;    /* Expected tag (for signal processors) */
static __thread int cfi_in_cfi_func = 0;          /* Is it included in the CFI protection function */

/* ========== Strict mode recovery status (CFI_EPILOGUE_TAG) ========== */
static __thread uint64_t cfi_strict_recovery_addr = 0; /* Correct return address for shadow stack storage */
static struct sigaction cfi_old_sigsegv;          /* old SIGSEGV handler */
static struct sigaction cfi_old_sigill;           /* old SIGILL handler */
static int cfi_signals_registered = 0;            /* Has the signal processor been registered */

/* Forward declarations */
static void cfi_register_signal_handlers(void);
static void cfi_unregister_signal_handlers(void);


/* ========== Backward edge tag cache ========== */
static __thread tag_cache_t local_tag_cache = { .count = 0 };
/**
 * cfi_get_strict_recovery_addr - Obtain the recovery address in strict mode
 * Used for restoring LR after detecting mismatch in CFI_EPILOGUE-TAG macro
 */
uint64_t cfi_get_strict_recovery_addr(void) {
    return cfi_strict_recovery_addr;
}

/* destruct */
static void cfi_thread_destructor(void *arg) {
    cfi_thread_data_t *data = (cfi_thread_data_t *)arg;
    if (data && data->rss) {
        if (data->rss->registered) {
            TEEC_Operation op = {0};
            uint32_t err;
            op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE);
            op.params[0].value.a = (uint32_t)data->rss->tid;
            TEEC_InvokeCommand(&sess, TA_CFI_CMD_UNREGISTER, &op, &err);
        }
        free(data->rss);
        free(data);
    }
}

/* ========== Initialize/Close ========== */
int cfi_init(void) {
    CFI_T_START();
    pthread_mutex_lock(&init_mutex);
    if (initialized) {
        pthread_mutex_unlock(&init_mutex);
        CFI_T_END("cfi_init_already");
        return 0;
    }

    TEEC_Result res;
    TEEC_UUID uuid = TA_CFI_UUID;
    uint32_t err;

    res = TEEC_InitializeContext(NULL, &ctx);
    if (res != TEEC_SUCCESS) {
        fprintf(stderr, "CFI: TEEC_InitializeContext failed: 0x%x\n", res);
        pthread_mutex_unlock(&init_mutex);
        CFI_T_END("cfi_init_error");
        return -1;
    }

    printf("[CFI-CLIENT] TEE context initialized OK\n");
    res = TEEC_OpenSession(&ctx, &sess, &uuid, TEEC_LOGIN_PUBLIC, NULL, NULL, &err);
    if (res != TEEC_SUCCESS) {
        fprintf(stderr, "CFI: TEEC_OpenSession failed: 0x%x origin 0x%x\n", res, err);
        TEEC_FinalizeContext(&ctx);
        pthread_mutex_unlock(&init_mutex);
        CFI_T_END("cfi_init_error");
        return -1;
    }

    pthread_key_create(&cfi_tls_key, cfi_thread_destructor);

    /* Register PAC-BTI Fault Signal Processor */
    cfi_register_signal_handlers();

    ree_degraded = 0;
    initialized = 1;
    g_cfi_state.attack_threshold = CFI_ATTACK_THRESHOLD;
    pthread_mutex_unlock(&init_mutex);
    printf("[CFI-CLIENT] CFI initialized (Backward-edge + Forward-edge + PAC-BTI recovery)\n");
    CFI_T_END("cfi_init");
    return 0;
}

void cfi_fini(void) {
    CFI_T_START();
    pthread_mutex_lock(&init_mutex);
    if (!initialized) {
        pthread_mutex_unlock(&init_mutex);
        CFI_T_END("cfi_fini_error");
        return;
    }

    /* Register PAC-BTI Fault Signal Processor */
    cfi_unregister_signal_handlers();

    TEEC_CloseSession(&sess);
    TEEC_FinalizeContext(&ctx);
    pthread_key_delete(cfi_tls_key);
    initialized = 0;
    pthread_mutex_unlock(&init_mutex);
    CFI_T_END("cfi_fini");
    printf("[CFI-CLIENT] CFI finalized\n");
}



/* Forward statement: Automatic downgrade chain requires */
static void cfi_record_attack(cfi_event_type_t type, const char *context);
static void cfi_clear_all_caches(void);

static pthread_mutex_t g_mode_mutex = PTHREAD_MUTEX_INITIALIZER;
/* ========== Downgrade management ========== */
int cfi_is_degraded(void) {
    return ree_degraded;
}

static void trigger_degrade(cfi_event_type_t type, const char *reason) {
    CFI_T_START();
    pthread_mutex_lock(&degrade_mutex);
    if (!ree_degraded) {
        ree_degraded = 1;
        g_cfi_state.status = CFI_RUNTIME_STATUS_DEGRADED;
        g_cfi_state.degrade_count++;
        cfi_log_event(CFI_EVENT_DEGRADE_TRIGGERED, 0, 0, 0,
                      reason ? reason : "TEE integrity mismatch or freeze breach");
        fprintf(stderr, "\n[CFI-CLIENT] *** DEGRADE TRIGGERED ***\n");
        fprintf(stderr, "[CFI-CLIENT] REE state compromised!\n");
        fprintf(stderr, "[CFI-CLIENT] All subsequent CFI checks will go through TEE.\n");
        fprintf(stderr, "[CFI-CLIENT] Performance will degrade but security is maintained.\n\n");
    }
    pthread_mutex_unlock(&degrade_mutex);
    pthread_mutex_lock(&g_mode_mutex);
    current_mode = CFI_MODE_DEGRADED;
    pthread_mutex_unlock(&g_mode_mutex);
    CFI_T_END("trigger_degrade");
    /* Record the attack and check if the automatic downgrade threshold has been reached */
    cfi_record_attack(type, reason);
    
}

/* ============================================================================
 * PAC-BTI Fault signal processor
 *
 * When PAC-BTI hardware detects that the return address has been tampered with, it triggers SIGSEGV or SIGILL.
 * This processor captures faults and overwrite the PC with the expected address saved by the shadow stack, 
 * allowing the program to continue executing.
 *
 * Workflow:
 * 1. CFI_EPILOGUE_TAG_PAC → cfi_epilogue_pac() Pop up the shadow stack and save the expected address
 * 2. ret instruction execution → PAC verification failed → CPU triggers fault
 * 3. This processor captures faults
 * 4. Cover PC with cfi_recovery_addr
 * 5. The program continues to execute from a legal return address
 * ============================================================================ */

/* ============================================================================
 * PAC-BTI Fault signal processor
 * ============================================================================ */

static void cfi_fault_handler(int sig, siginfo_t *info, void *ucontext) {
    /* Independent timing: does not rely on thread local nested stacks, avoiding conflicts with interrupted functions */
    uint64_t _cfi_handler_start = 0;
#if CFI_TIMING
    _cfi_handler_start = cfi_get_time_ns();
#endif

    ucontext_t *uc = (ucontext_t *)ucontext;
    mcontext_t *mc = &uc->uc_mcontext;

    /* Only attempt to recover within the CFI protection function and save the recovery address */
    if (!cfi_in_cfi_func || cfi_recovery_addr == 0) {
        fprintf(stderr, "[CFI-FAULT] Fatal fault outside CFI context: "
                "sig=%d addr=%p\n", sig, info->si_addr);
        g_cfi_state.pac_fault_count++;
        cfi_record_attack(CFI_EVENT_PAC_FAULT_FATAL,
                         "Fatal PAC fault outside CFI context");
        if (sig == SIGSEGV && cfi_signals_registered) {
            sigaction(SIGSEGV, &cfi_old_sigsegv, NULL);
        } else if (sig == SIGILL && cfi_signals_registered) {
            sigaction(SIGILL, &cfi_old_sigill, NULL);
        }
#if CFI_TIMING
        if (_cfi_handler_start) {
            cfi_timing_record("cfi_fault_handler_old",
                cfi_get_time_ns() - _cfi_handler_start);
        }
#endif
        return;
    }

    /* Obtain the fault address */
    uint64_t corrupted_pc = (uint64_t)mc->pc;
    uint64_t corrupted_lr = (uint64_t)mc->regs[30];

    fprintf(stderr, "[CFI-FAULT] PAC/BTI fault recovered! "
            "sig=%d corrupted_pc=0x%lx corrupted_lr=0x%lx "
            "-> recovered=0x%lx (tag=%u)\n",
            sig, corrupted_pc, corrupted_lr,
            cfi_recovery_addr, cfi_recovery_tag);

    /* recovery PC and LR */
    mc->pc = cfi_recovery_addr;
    mc->regs[30] = cfi_recovery_addr;

    /* log recovery event */
    g_cfi_state.pac_fault_count++;
    g_cfi_state.pac_recover_count++;
    cfi_log_event(CFI_EVENT_PAC_FAULT_RECOVERED, cfi_recovery_tag,
                  cfi_recovery_addr, corrupted_pc,
                  "PAC fault recovered via shadow stack");

    /* Clear recovery status */
    cfi_recovery_addr = 0;
    cfi_recovery_tag = 0;
    cfi_in_cfi_func = 0;

#if CFI_TIMING
    if (_cfi_handler_start) {
        cfi_timing_record("cfi_fault_handler",
            cfi_get_time_ns() - _cfi_handler_start);
    }
#endif
}

/**
 * cfi_register_signal_handlers - Register PAC-BTI Fault Signal Processor
 */
void cfi_register_signal_handlers(void) {
    CFI_T_START();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = cfi_fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    #if CFI_PROTECT
    /* CFI FWD: Register signal handlers with target validation */
    cfi_register_target((uint64_t)(uintptr_t)cfi_fault_handler, 0xDEAD0001U);
    #endif
    sigaction(SIGSEGV, &sa, &cfi_old_sigsegv);
    sigaction(SIGILL, &sa, &cfi_old_sigill);
    cfi_signals_registered = 1;
    CFI_T_END("cfi_register_signals");
}

/**
 * cfi_unregister_signal_handlers - Cancel PAC-BTI fault signal processor
 */
void cfi_unregister_signal_handlers(void) {
    CFI_T_START();
    if (cfi_signals_registered) {
        sigaction(SIGSEGV, &cfi_old_sigsegv, NULL);
        sigaction(SIGILL, &cfi_old_sigill, NULL);
        cfi_signals_registered = 0;
    }
    CFI_T_END("cfi_unregister_signals");
}

/* ========== Thread management ========== */
static cfi_thread_data_t *cfi_get_thread_data(void) {
    cfi_thread_data_t *data = pthread_getspecific(cfi_tls_key);
    if (data == NULL) {
        data = calloc(1, sizeof(cfi_thread_data_t));
        if (!data) return NULL;

        data->rss = calloc(1, sizeof(ree_shadow_stack_t));
        if (!data->rss) {
            free(data);
            return NULL;
        }

        printf("[CFI-CLIENT] New thread detected, registering with TA...\n");
        TEEC_Operation op = {0};
        uint32_t err;
        op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_OUTPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE);
        CFI_T_START();
        TEEC_Result res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_REGISTER, &op, &err);
        CFI_T_END("tee_thread_register");
        if (res != TEEC_SUCCESS) {
            free(data->rss);
            free(data);
            return NULL;
        }
        CFI_T_START();
        data->rss->tid = (int)op.params[0].value.a;
        data->rss->registered = 1;
        data->rss->sp = 0;
        data->target_cache.count = 0;
	CFI_T_END("ree_thread_register");
        pthread_setspecific(cfi_tls_key, data);
        printf("[CFI-CLIENT] Thread registered, tid=%d\n", data->rss->tid);
    }
    return data;
}

static ree_shadow_stack_t *cfi_get_rss(void) {
    cfi_thread_data_t *data = cfi_get_thread_data();
    return data ? data->rss : NULL;
}

static target_cache_t *cfi_get_target_cache(void) {
    cfi_thread_data_t *data = cfi_get_thread_data();
    return data ? &data->target_cache : NULL;
}

/* ========== SURVIVAL Mode: Clear all REE cache ========== */
static void cfi_clear_all_caches(void) {
    CFI_T_START();
    golden_cache_count = 0;
    memset(golden_cache, 0, sizeof(golden_cache));

    local_tag_cache.count = 0;
    memset(&local_tag_cache, 0, sizeof(local_tag_cache));

    cfi_thread_data_t *data = cfi_get_thread_data();
    if (data) {
        memset(&data->target_cache, 0, sizeof(data->target_cache));
    }
    CFI_T_END("cfi_clear_all_caches");
    fprintf(stderr, "[CFI-CLIENT] All REE caches cleared for SURVIVAL mode\n");
}



static int tag_in_local_cache(uint32_t tag) {
    for (int i = 0; i < local_tag_cache.count; i++) {
        if (local_tag_cache.tags[i] == tag) return 1;
    }
    return 0;
}

static void tag_add_local_cache(uint32_t tag) {
    if (local_tag_cache.count < TAG_CACHE_SIZE) {
        local_tag_cache.tags[local_tag_cache.count++] = tag;
    }
}

/* ========== TEE Full push/check (downgrade mode) ========== */
static void tee_push_addr(uint64_t addr, int tid) {
    CFI_T_START();
    TEEC_Operation op = {0};
    uint32_t err;
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE);
    op.params[0].value.a = (uint32_t)tid;
    op.params[1].value.a = (uint32_t)(addr >> 32);
    op.params[1].value.b = (uint32_t)addr;
    TEEC_InvokeCommand(&sess, TA_CFI_CMD_PUSH, &op, &err);
    CFI_T_END("tee_push");
}

static int tee_check_addr(uint64_t addr, int tid) {
    CFI_T_START();
    TEEC_Operation op = {0};
    uint32_t err;
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_INPUT, TEEC_VALUE_OUTPUT, TEEC_NONE);
    op.params[0].value.a = (uint32_t)tid;
    op.params[1].value.a = (uint32_t)(addr >> 32);
    op.params[1].value.b = (uint32_t)addr;
    TEEC_Result res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_CHECK, &op, &err);
    if (res != TEEC_SUCCESS) { CFI_T_END("tee_check_error"); return 0;}
    uint64_t match = ((uint64_t)op.params[2].value.a << 32) | op.params[2].value.b;
    CFI_T_END("tee_check");
    return (int)match;
}

/* ============================================================================
 * TEE Golden Backup Query (for Degradation mode)
 *
 * Design: after freeze, golden table save (tag -> golden_addr) mapping.
 * After degrading, check no longer trusts the REE shadow stack, but directly checks the TEE golden table.
 * Reduce TEE round trip through REE side golden cache.
 * ============================================================================ */



static int golden_cache_lookup(uint32_t tag, uint64_t *addr_out) {
    CFI_T_START();
    if (cfi_get_protection_mode() == CFI_MODE_SURVIVAL) {
        /* SURVIVAL: forbidden golden_cache, directly use TEE */
        CFI_T_END("golden_cache_lookup_survival");
        return 0;
    }
    for (int i = 0; i < golden_cache_count; i++) {
        if (golden_cache[i].valid && golden_cache[i].tag == tag) {
            *addr_out = golden_cache[i].golden_addr;
            CFI_T_END("golden_cache_lookup");
            return 1;
        }
    }
    CFI_T_END("golden_cache_lookup_error");
    return 0;
}

static void golden_cache_insert(uint32_t tag, uint64_t addr) {
    CFI_T_START();
    if (cfi_get_protection_mode() == CFI_MODE_SURVIVAL) {
        /* SURVIVAL: forbidden golden_cache */
        CFI_T_END("golden_cache_insert_error");
        return;
    }
    for (int i = 0; i < golden_cache_count; i++) {
        if (golden_cache[i].tag == tag) {
            golden_cache[i].golden_addr = addr;
            golden_cache[i].valid = 1;
            CFI_T_END("golden_cache_insert_exist");
            return;
        }
    }
    if (golden_cache_count < GOLDEN_CACHE_SIZE) {
        golden_cache[golden_cache_count].tag = tag;
        golden_cache[golden_cache_count].golden_addr = addr;
        golden_cache[golden_cache_count].valid = 1;
        golden_cache_count++;
    }
    CFI_T_END("golden_cache_insert");
}

/**
 * tee_get_golden - Retrieve the golden address corresponding to the tag from the TEE golden table
 * @param tag: Function Tags
 * @param addr_out: Output parameters, return golden address
 * @return: 1=Success, 0=Unknown tag (TEE.ERROR_SAD_STATE), -1=Other errors
 */
static int tee_get_golden(uint32_t tag, uint64_t *addr_out) {
    CFI_T_START();
    TEEC_Operation op = {0};
    uint32_t err;
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_OUTPUT, TEEC_NONE, TEEC_NONE);
    op.params[0].value.a = tag;

    TEEC_Result res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_GET_BACKUP, &op, &err);
    if (res == TEEC_ERROR_BAD_STATE) {
    	CFI_T_END("tee_get_golden_error");
        return 0;  /* Unknown tag after freeze */
    }
    if (res != TEEC_SUCCESS) {
        fprintf(stderr, "[CFI-CLIENT] GET_BACKUP failed: res=0x%x origin=0x%x\n", res, err);
        CFI_T_END("tee_get_golden_error");
        return -1;
    }

    uint64_t golden_addr = ((uint64_t)op.params[1].value.a << 32) | op.params[1].value.b;
    if (addr_out) *addr_out = golden_addr;
    CFI_T_END("tee_get_golden");
    return 1;
}

/* ========== Core: Labeled push (initial synchronization of TEE, subsequent pure REE) ========== */
void cfi_push_addr_tag(uint64_t addr, uint32_t tag) {
    CFI_T_START();
    ree_shadow_stack_t *rss = cfi_get_rss();
    if (!rss) { CFI_T_END("cfi_push_addr_tag_error"); return; }

    if (ree_degraded) {
        /* Downgrade mode: The golden table has frozen and will no longer PUSH_BACK.
         * Only REE local push to maintain stack depth synchronization. */
        if (rss->sp >= REE_STACK_DEPTH) {
            fprintf(stderr, "[CFI-CLIENT] REE stack overflow!\n");
            CFI_T_END("cfi_push_addr_tag_error");
            return;
        }
        static __thread uint32_t seq_counter_dg = 0;
        rss->stack[rss->sp].ret_addr = addr;
        rss->stack[rss->sp].tag = tag;
        rss->stack[rss->sp].seq = seq_counter_dg++;
        rss->sp++;
        CFI_T_END("cfi_push_addr_tag_degraded");
        return;
    }

    if (!tag_in_local_cache(tag)) {
        TEEC_Operation op = {0};
        uint32_t err;
        /* push_backup returns status in params[2].value.a */
        op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
                                          TEEC_VALUE_INPUT,
                                          TEEC_VALUE_OUTPUT,
                                          TEEC_NONE);
        op.params[0].value.a = tag;
        op.params[1].value.a = (uint32_t)(addr >> 32);
        op.params[1].value.b = (uint32_t)addr;

        CFI_T_START();
        TEEC_Result res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_PUSH_BACKUP, &op, &err);
        CFI_T_END("tee_push_backup");
        if (res == TEEC_SUCCESS || res == TEEC_ERROR_BAD_STATE) {
            tag_add_local_cache(tag);

            /* Check freeze breach / probe signals */
            uint32_t backup_status = op.params[2].value.a;
            if (backup_status == CFI_BACKUP_NEW_FROZEN) {
                pthread_mutex_lock(&learning_mutex);
                freeze_breach_count++;
                if (freeze_breach_count >= 3) {
                    g_cfi_state.status = CFI_RUNTIME_STATUS_EMERGENCY;
                }
                pthread_mutex_unlock(&learning_mutex);
                cfi_log_event(CFI_EVENT_FREEZE_BREACH, tag,
                              0, addr,
                              "Freeze breach: new call site after freeze");
                fprintf(stderr, "[CFI-CLIENT] FREEZE BREACH #%u! "
                        "tag=%u addr=0x%lx\n", freeze_breach_count, tag, addr);
                trigger_degrade(CFI_EVENT_FREEZE_BREACH,
                               "Freeze breach: new call site after freeze");
                
            } else if (backup_status == CFI_BACKUP_PROBE) {
                fprintf(stderr, "[CFI-CLIENT] PROBE ALERT! tag=%u "
                        "has too many call sites\n", tag);
                cfi_record_attack(CFI_EVENT_FREEZE_BREACH,
                                 "Probe attack: too many call sites for tag");
            }
        }
    }

    if (rss->sp >= REE_STACK_DEPTH) {
        fprintf(stderr, "[CFI-CLIENT] REE stack overflow!\n");
        CFI_T_END("cfi_push_addr_tag_error");
        return;
    }

    static __thread uint32_t seq_counter = 0;
    rss->stack[rss->sp].ret_addr = addr;
    rss->stack[rss->sp].tag = tag;
    rss->stack[rss->sp].seq = seq_counter++;
    rss->sp++;
    CFI_T_END("cfi_push_addr_tag_ree");
}

/* ========== Core: Labeled check (pure REE, TEE recovery in case of exceptions) ========== */
int cfi_check_addr_tag(uint64_t addr, uint32_t tag) {
    CFI_T_START();
    ree_shadow_stack_t *rss = cfi_get_rss();
    if (!rss || rss->sp <= 0) {
        fprintf(stderr, "[CFI-CLIENT] CHECK failed: invalid stack state\n");
        CFI_T_END("cfi_check_addr_tag_error");
        return 0;
    }

    /* Degradation mode: Shadow stack always serves as the recovery source, 
     * while golden table performs authoritative verification */
    if (ree_degraded) {
        if (rss->sp <= 0) { CFI_T_END("cfi_check_addr_tag_error"); return 0; }
        rss->sp--;
        ree_entry_t *entry = &rss->stack[rss->sp];
        uint64_t golden_addr = 0;
        int have_golden = golden_cache_lookup(tag, &golden_addr);
        if (!have_golden) {
            int result = tee_get_golden(tag, &golden_addr);
            if (result == 1) {
                golden_cache_insert(tag, golden_addr);
                have_golden = 1;
            } else if (result == 0) {
                have_golden = 0;
                fprintf(stderr, "[CFI-TEE] Unknown tag=%u after freeze (degraded check)\n", tag);
            } else {
                fprintf(stderr, "[CFI-TEE] Failed to query golden for tag=%u\n", tag);
            }
        }

        /* Recovery source selection: Golden table validation shadow stack */
        uint64_t recovery_addr = entry->ret_addr;
        if (have_golden && golden_addr != entry->ret_addr) {
            /* The shadow stack has been tampered with! Restore with Golden Table */
            fprintf(stderr, "[CFI-TEE] Shadow stack CORRUPTED! tag=%u "
                    "golden=0x%lx shadow=0x%lx. Using golden.\n",
                    tag, golden_addr, entry->ret_addr);
            recovery_addr = golden_addr;
        }

        if (recovery_addr == addr && entry->tag == tag) {
            CFI_T_END("cfi_check_addr_tag_recovery_degraded");
            return 1; /* normal */
        }

        /* LR tampered with: Save recovery address and allow EPILOGUE macro to recover x30 */
        fprintf(stderr, "[CFI-TEE] LR tampered! tag=%u restoring 0x%lx (lr=0x%lx)\n",
                tag, recovery_addr, addr);
        cfi_strict_recovery_addr = recovery_addr;
        CFI_T_END("cfi_check_addr_tag_error");
        return 0; /* Tell EPILOGUE: mismatch, LR needs to be restored */
    }
    CFI_T_END("cfi_check_addr_tag_ree_degraded_end");
    CFI_T_START();
    rss->sp--;
    ree_entry_t *entry = &rss->stack[rss->sp];

    /* Fast path: REE local verification */
    if (entry->tag == tag && entry->ret_addr == addr) {
        CFI_T_END("cfi_check_addr_tag_ree");
        return 1;
    }

    /* === Abnormal path === */
    fprintf(stderr, "[CFI-CLIENT] *** VIOLATION *** tid=%d tag=%u expected_addr=0x%lx got=0x%lx\n",
            rss->tid, tag, entry->ret_addr, addr);

    /* After freeze: save recovery addr, degrade, let epilogue restore LR */
    if (!learning_mode_active) {
        fprintf(stderr, "[CFI-CLIENT] Mismatch after freeze, auto-degrade (tag=%u)\n", tag);
        cfi_strict_recovery_addr = entry->ret_addr;
        trigger_degrade(CFI_EVENT_DEGRADE_TRIGGERED,
                       "Mismatch after freeze detected");
        CFI_T_END("cfi_check_addr_tag_trigger_degrade");
        return 0; /* tell CFI_EPILOGUE_TAG to restore LR */
    }
    CFI_T_END("cfi_check_addr_tag_ree_end");
    /* Enter TEE to obtain golden backup */
    TEEC_Operation op = {0};
    uint32_t err;
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_OUTPUT, TEEC_NONE, TEEC_NONE);
    op.params[0].value.a = tag;

    CFI_T_START();
    TEEC_Result res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_GET_BACKUP, &op, &err);
    CFI_T_END("tee_get_backup");
    
    CFI_T_START();
    if (res == TEEC_SUCCESS) {
        uint64_t backup_addr = ((uint64_t)op.params[1].value.a << 32) | op.params[1].value.b;
        fprintf(stderr, "[CFI-CLIENT] RECOVERY: Using golden backup addr=0x%lx for tag=%u\n",
                backup_addr, tag);

        entry->ret_addr = backup_addr;
        entry->tag = tag;
        CFI_T_END("cfi_check_addr_tag_tee_backup");
        return 1;
    }else if (res == TEEC_ERROR_BAD_STATE) {
        /* Tag has no golden backup (new call site after freeze
         * or a thread whose call sites were never backed up).
         * Auto-degrade to TEE-only path for safety.
         */
        fprintf(stderr, "[CFI-CLIENT] RECOVERY: tag=%u has no golden backup "
                "(new after freeze). Triggering degrade.\n", tag);
        cfi_strict_recovery_addr = entry->ret_addr;
        trigger_degrade(CFI_EVENT_DEGRADE_TRIGGERED,
                       "No golden backup for tag (new after freeze)");
        CFI_T_END("cfi_check_addr_tag_trigger_degrade");
        return 0; /* let EPILOGUE restore LR */ 
    }else {
        fprintf(stderr, "[CFI-CLIENT] RECOVERY FAILED: tag=%u res=0x%x\n",
                tag, res);
	trigger_degrade(CFI_EVENT_DEGRADE_TRIGGERED,
                       "Golden backup recovery failed");

        cfi_strict_recovery_addr = entry->ret_addr;
        CFI_T_END("cfi_check_addr_tag_trigger_degrade");
        return 0; /* let EPILOGUE restore LR */
    }
}

/* ============================================================================
 * cfi_epilogue_pac - PAC-BTI Safety Epilogue (Not Abort, Dependent on Signal Recovery)
 *
 * Specially designed for PAC-BTI hardware protection:
 * 1. Pop up the expected return address from the shadow stack
 * 2. Save to the __thread variable cfi_recovery_addr (for use by signal processors)
 * 3. Do not abort - let the ret instruction execute
 * 4. If LR is tampered with, PAC-BTI triggers SIGSEGV/SIGILL during ret
 * 5. The signal processor cfifault_handler uses the saved address to restore the PC
 *
 * The key difference between cfiucheck_dedr_tag and cfi:
 * - cfi_check_addr_tag: When there is a mismatch, return 0 (the caller is responsible for abort)
 * - cfi_epilogue_pac: When there is a mismatch, return 1 (let ret execute, PAC-BTI will catch it)
 *
 * @param addr: __builtin_return_address(0) (Current return address)
 * @param tag: Function Tags
 * @return: 1 (Always return 1, let ret execute)
 * ============================================================================ */
int cfi_epilogue_pac(uint64_t addr, uint32_t tag) {
    CFI_T_START();
    /* Clear the previous recovery state */
    cfi_recovery_addr = 0;
    cfi_recovery_tag = 0;
    cfi_in_cfi_func = 0;

    ree_shadow_stack_t *rss = cfi_get_rss();
    if (!rss || rss->sp <= 0) {
        /* No shadow stack entry - not in CFI protection context */
        CFI_T_END("cfi_epilogue_pac_error");
        return 1;  /* Let ret execute, if there is a fault, it cannot be restored */
    }


    /* Degradation mode: Shadow stack as recovery source, Golden table for authoritative verification */
    if (ree_degraded) {
        if (rss->sp <= 0) {
            CFI_T_END("cfi_epilogue_pac_error");
            return 1; /* let ret execute */
        }
        rss->sp--;
        ree_entry_t *entry = &rss->stack[rss->sp];
        uint64_t golden_addr = 0;
        int have_golden = golden_cache_lookup(tag, &golden_addr);
        if (!have_golden) {
            int result = tee_get_golden(tag, &golden_addr);
            if (result == 1) {
                golden_cache_insert(tag, golden_addr);
                have_golden = 1;
            } else if (result == 0) {
            	have_golden = 0;
                fprintf(stderr, "[CFI-TEE] Unknown tag=%u after freeze (degraded PAC)\n", tag);
            }
        }

        /* Recovery source selection: Golden table validation shadow stack */
        uint64_t recovery_addr = entry->ret_addr;
        if (have_golden && golden_addr != entry->ret_addr) {
            /* The shadow stack has been tampered with! Restore with Golden Table */
            fprintf(stderr, "[CFI-TEE] Shadow stack CORRUPTED! tag=%u "
                    "golden=0x%lx shadow=0x%lx. Using golden.\n",
                    tag, golden_addr, entry->ret_addr);
            recovery_addr = golden_addr;
        }

        cfi_recovery_addr = recovery_addr;
        cfi_recovery_tag = tag;
        cfi_in_cfi_func = 1;

        if (recovery_addr != addr || entry->tag != tag) {
            fprintf(stderr, "[CFI-TEE] LR tampered! tag=%u restoring 0x%lx (lr=0x%lx)\n",
                    tag, recovery_addr, addr);
        }
        CFI_T_END("cfi_epilogue_pac_ree_degraded");
        return 1;  /* Let ret execute, PAC-BTI+signal processor provides backup recovery */
    }

    /* REE Quick Path: Pop up and Verify */

    rss->sp--;
    ree_entry_t *entry = &rss->stack[rss->sp];

    /* Save expected address (whether matched or not, used for signal recovery) */
    cfi_recovery_addr = entry->ret_addr;
    cfi_recovery_tag = tag;
    cfi_in_cfi_func = 1;

    if (entry->tag == tag && entry->ret_addr == addr) {
        /* Match - Normal Path */

        CFI_T_END("cfi_epilogue_pac");
        return 1;
    }

    /* Mismatch - LR may have been tampered with */
    fprintf(stderr, "[CFI] Shadow stack mismatch! expected=0x%lx got=0x%lx "
            "tag=%u. Relying on PAC-BTI signal recovery.\n",
            entry->ret_addr, addr, tag);

    /* Still return 1- let ret execute, PAC-BTI hardware will catch tampering during ret */
    CFI_T_END("cfi_epilogue_pac_ree_end");
    return 1;
}

/* ========== Compatible with old interfaces ========== */
void cfi_push_addr(uint64_t addr) {
    cfi_push_addr_tag(addr, 0);
}

int cfi_check_addr(uint64_t addr) {
    return cfi_check_addr_tag(addr, 0);
}

/* ========== Get stack depth ========== */
int cfi_get_depth(void) {
    ree_shadow_stack_t *rss = cfi_get_rss();
    if (!rss) return -1;
    return rss->sp;
}

/* ============================================================================
 * Forward edge implementation (runtime learning mode - symmetric with backward edge pushbacking)
 *
 * Core design:
 * - When cfi_verify_forward_target (addr, label) is called for the first time, TEE automatically learns the target
 *   (Register it as a legitimate target and learn return addr symmetry with the push backup of the backward edge)
 * - When calling the same (addr, label) combination in the future, the REE cache quickly passes through
 * - The same addr is called by different labels → type obfuscation attack, rejected
 * - No need to precompile address table, no need for double compilation
 * ============================================================================ */

static int target_in_cache(target_cache_t *cache, uint64_t addr, uint32_t label) {
    if (!cache) return 0;
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].valid && cache->entries[i].addr == addr
            && cache->entries[i].label == label)
            return 1;
    }
    return 0;
}

static void target_add_cache(target_cache_t *cache, uint64_t addr, uint32_t label) {
    if (!cache || cache->count >= TARGET_CACHE_SIZE) return;
    cache->entries[cache->count].addr = addr;
    cache->entries[cache->count].label = label;
    cache->entries[cache->count].valid = 1;
    cache->count++;
}

/**
 * tee_verify_or_learn - TEE validation target, automatic learning of unknown targets (first registration)
 *
 * Process (symmetric with the backward edge pushbacking/check):
 * 1. TEE query target table
 * 2. Target exists and label matches → Passed through
 * 3. Target exists but label does not match → Type confusion, rejected
 * 4. Target does not exist → First encounter, automatic registration (learning mode), through
 */
static int tee_verify_or_learn(uint64_t addr, uint32_t label) {
    if (!initialized) return 1;

    TEEC_Operation op = {0};
    uint32_t err;
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_INPUT, TEEC_VALUE_OUTPUT, TEEC_NONE);
    op.params[0].value.a = (uint32_t)(addr >> 32);
    op.params[0].value.b = (uint32_t)addr;
    op.params[1].value.a = label;

    CFI_T_START();
    TEEC_Result res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_VERIFY_TARGET, &op, &err);
    CFI_T_END("tee_verify_target");
    if (res != TEEC_SUCCESS) {
        fprintf(stderr, "[CFI-CLIENT] TEE verify_target invoke failed: 0x%x\n", res);
        return 0;
    }

    int result = (int)op.params[2].value.a;
    uint32_t type_class = op.params[2].value.b;

    /* Meaning of result (TEE definition):
     * 1 = Validation passed (known target, label matching)
     * 0 = Verification failed (known target, label mismatch → type obfuscation attack)
     * If TEE returns 0 and type_class==0xFF → target unknown, learning is required
     */
    if (result == 1) {
        return 1;  /* Known legitimate targets */
    }

    if (type_class == 0xFF) {
        /* Unknown target: Automatic learning (symmetric with pushbacking learning return addr) */
        TEEC_Operation reg_op = {0};
        reg_op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE);
        reg_op.params[0].value.a = (uint32_t)(addr >> 32);
        reg_op.params[0].value.b = (uint32_t)addr;
        reg_op.params[1].value.a = label;
        reg_op.params[1].value.b = CFI_TARGET_TYPE_GENERIC;

        CFI_T_START();
        TEEC_Result reg_res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_REGISTER_TARGET, &reg_op, &err);
        CFI_T_END("tee_register_target_learn");
        if (reg_res == TEEC_SUCCESS || reg_res == TEEC_ERROR_BAD_STATE) {
            return 1;  /* learn success */
        }
        fprintf(stderr, "[CFI-CLIENT] Forward-edge auto-learn failed: 0x%x\n", reg_res);
        return 0;
    }

    /* result==0 and type_class!=0xFF → type confusion attack */
    fprintf(stderr, "[CFI-CLIENT] *** FORWARD TYPE MISMATCH *** addr=0x%lx label=%u\n",
            addr, label);
    cfi_record_attack(CFI_EVENT_FORWARD_VIOLATION,
                     "Forward-edge type mismatch attack");
    return 0;
}

/**
 * cfi_verify_forward_target - Forward edge target verification (three-layer system, symmetrical with the backward edge)
 *
 * 1. REE cache fast path (symmetric with REE shadow stack)
 * 2. TEE verification+automatic learning path (symmetric with TEE pushbacking/check)
 * 3. Downgrade mode: All use TEE
 */
int cfi_verify_forward_target(uint64_t addr, uint32_t label) {
    CFI_T_START();
    if (!initialized || addr == 0) { CFI_T_END("cfi_verify_forward_error"); return 1; }

    target_cache_t *cache = cfi_get_target_cache();

    /* Level 1: REE cache fast path */
    if (cache && target_in_cache(cache, addr, label)) {
        CFI_T_END("cfi_verify_forward_ree");
        return 1;
    }

    /* Level 2: TEE Validation (Automatic Learning of Unknown Targets) */
    int result = tee_verify_or_learn(addr, label);

    if (result && cache) {
        /* Verified, add REE cache to accelerate subsequent calls */
        target_add_cache(cache, addr, label);
    }

    CFI_T_END("cfi_verify_forward_tee");
    return result;
}

/* Old interface compatibility */
int cfi_verify_target(uint64_t addr, uint32_t label) {
    return cfi_verify_forward_target(addr, label);
}

/**
 * cfi_register_target - Explicitly registering forward edge targets 
 * (for pre registration scenarios such as thread creation)
 * Symmetrical with the backward edge cfi_push_dedr_tag: explicitly pushing legitimate targets to TEE
 */
void cfi_register_target(uint64_t addr, uint32_t label) {
    CFI_T_START();
    if (!initialized || addr == 0) { CFI_T_END("cfi_register_target_error"); return; }

    /* Add REE cache first */
    target_cache_t *cache = cfi_get_target_cache();
    if (cache && !target_in_cache(cache, addr, label)) {
        target_add_cache(cache, addr, label);
    }
    CFI_T_END("ree_register_target");
    CFI_T_START();
    /* Synchronize to TEE */
    TEEC_Operation op = {0};
    uint32_t err;
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE);
    op.params[0].value.a = (uint32_t)(addr >> 32);
    op.params[0].value.b = (uint32_t)addr;
    op.params[1].value.a = label;
    op.params[1].value.b = CFI_TARGET_TYPE_GENERIC;

    TEEC_InvokeCommand(&sess, TA_CFI_CMD_REGISTER_TARGET, &op, &err);
    CFI_T_END("tee_register_target");
}

/* ============================================================================
 * GOT/PLT protection
 * ============================================================================ */

int cfi_got_init(void) {
    CFI_T_START();
    if (!initialized) {
        fprintf(stderr, "[CFI-GOT] CFI not initialized!\n");
        CFI_T_END("cfi_got_init_error");
        return -1;
    }

    if (got_parse_self(&got_entries, &got_entry_count) < 0) {
        fprintf(stderr, "[CFI-GOT] Failed to parse GOT\n");
        CFI_T_END("cfi_got_init_error");
        return -1;
    }

    printf("[CFI-GOT] Found %d GOT entries\n", got_entry_count);

    if (got_entry_count > 0) {
        size_t batch_size = got_entry_count * 16;
        TEEC_SharedMemory shm;
        shm.size = batch_size;
        shm.flags = TEEC_MEM_INPUT;
        TEEC_Result res = TEEC_AllocateSharedMemory(&ctx, &shm);
        if (res != TEEC_SUCCESS) {
            fprintf(stderr, "[CFI-GOT] Failed to allocate shared memory\n");
            got_free_entries(got_entries);
            got_entries = NULL;
            CFI_T_END("cfi_got_init_error");
            return -1;
        }

        uint8_t *data = (uint8_t *)shm.buffer;
        for (int i = 0; i < got_entry_count; i++) {
            /* plt_addr is the only key: section headers mode = PLT addr
             * program headers mode = got_addr (Unique during program runtime) */
            for (int j = 0; j < 8; j++) {
                data[i*16+j] = (got_entries[i].plt_addr >> (j*8)) & 0xFF;
                data[i*16+8+j] = (got_entries[i].got_target >> (j*8)) & 0xFF;
            }
        }

        TEEC_Operation op = {0};
        uint32_t err;
        op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_WHOLE, TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE);
        op.params[0].memref.parent = &shm;
        op.params[0].memref.size = batch_size;
        op.params[0].memref.offset = 0;
        op.params[1].value.a = (uint32_t)got_entry_count;

        CFI_T_START();
        res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_REGISTER_GOT_BATCH, &op, &err);
        CFI_T_END("tee_register_got_batch");
        TEEC_ReleaseSharedMemory(&shm);

        if (res != TEEC_SUCCESS) {
            fprintf(stderr, "[CFI-GOT] Failed to register GOT batch: 0x%x\n", res);
            got_free_entries(got_entries);
            got_entries = NULL;
            CFI_T_END("cfi_got_init_error");
            return -1;
        }

        printf("[CFI-GOT] Registered %d GOT entries to TEE\n", got_entry_count);
    }

    got_initialized = 1;
    CFI_T_END("cfi_got_init");
    return 0;
}

void cfi_got_fini(void) {
    CFI_T_START();
    if (got_entries) {
        got_free_entries(got_entries);
        got_entries = NULL;
    }
    got_entry_count = 0;
    got_initialized = 0;
    CFI_T_END("cfi_got_fini");
}

/* ============================================================================
 * GOT integrity verify
 * ============================================================================ */
int cfi_got_verify(void) {
    CFI_T_START();
    if (!got_initialized || !got_entries) { CFI_T_END("cfi_got_verify_error"); return 0; }
    if (ree_degraded) { CFI_T_END("cfi_got_verify_error"); return 0; }

    uint8_t *current_data = malloc(got_entry_count * 8);
    if (!current_data) { CFI_T_END("cfi_got_verify_error"); return -1; }

    for (int i = 0; i < got_entry_count; i++) {
        uint64_t current = got_read_target(got_entries[i].got_addr);
        for (int j = 0; j < 8; j++) {
            current_data[i*8+j] = (current >> (j*8)) & 0xFF;
        }
    }

    uint8_t hash[32];
    EVP_MD_CTX *sha_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(sha_ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(sha_ctx, current_data, got_entry_count * 8);
    EVP_DigestFinal_ex(sha_ctx, hash, NULL);
    EVP_MD_CTX_free(sha_ctx);
    CFI_T_END("cfi_got_verify_ree");
    
    CFI_T_START();
    TEEC_Operation op = {0};
    uint32_t err;
    TEEC_SharedMemory shm;
    shm.size = 32;
    shm.flags = TEEC_MEM_INPUT;
    TEEC_Result res = TEEC_AllocateSharedMemory(&ctx, &shm);
    if (res != TEEC_SUCCESS) {
        free(current_data);
        CFI_T_END("cfi_got_verify_error");
        return -1;
    }

    memcpy(shm.buffer, hash, 32);
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_WHOLE, TEEC_VALUE_INPUT, TEEC_VALUE_OUTPUT, TEEC_NONE);
    op.params[0].memref.parent = &shm;
    op.params[0].memref.size = 32;
    op.params[0].memref.offset = 0;
    op.params[1].value.a = (uint32_t)got_entry_count;
    CFI_T_END("cfi_got_verify_allocate_memory");
    
    CFI_T_START();
    res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_VERIFY_GOT_INTEGRITY, &op, &err);
    CFI_T_END("tee_verify_got_integrity");
    
    CFI_T_START();
    TEEC_ReleaseSharedMemory(&shm);
    free(current_data);

    if (res != TEEC_SUCCESS) {
        fprintf(stderr, "[CFI-GOT] TEE verify failed: 0x%x\n", res);
        CFI_T_END("cfi_got_verify_error");
        return -1;
    }

    uint32_t result = op.params[2].value.a;
    if (result == CFI_INTEGRITY_FAIL) {
        fprintf(stderr, "[CFI-GOT] *** GOT INTEGRITY FAIL ***\n");
        trigger_degrade(CFI_EVENT_INTEGRITY_FAIL_GOT,
                       "GOT integrity check failed");
        CFI_T_END("cfi_got_verify_trigger_degrade");
        return 1;
    } else if (result == CFI_INTEGRITY_FIRST) {
        printf("[CFI-GOT] GOT baseline established\n");
        CFI_T_END("cfi_got_verify_baseline_establish");
        return 2;
    }
    CFI_T_END("cfi_got_verify");
    return 0;
}

/* ============================================================================
 * Integrity verification (backward edge+forward edge caching)
 * ============================================================================ */
/* ---------- original shadow stack extract integrity verification as an independent function ---------- */
static int cfi_verify_shadow_stack_integrity(void) {
    CFI_T_START();
    cfi_thread_data_t *thread_data = cfi_get_thread_data();
    if (!thread_data || !thread_data->rss) {
        CFI_T_END("cfi_verify_shadow_stack_integrity_error");
        return -1;
    }
    
    if (ree_degraded) { CFI_T_END("cfi_verify_integrity_error"); return 0; }
    
    ree_shadow_stack_t *rss = thread_data->rss;

    EVP_MD_CTX *sha_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(sha_ctx, EVP_sha256(), NULL);

    for (int i = 0; i < rss->sp; i++) {
        EVP_DigestUpdate(sha_ctx, &rss->stack[i].tag, sizeof(uint32_t));
        EVP_DigestUpdate(sha_ctx, &rss->stack[i].ret_addr, sizeof(uint64_t));
        EVP_DigestUpdate(sha_ctx, &rss->stack[i].seq, sizeof(uint32_t));
    }

    target_cache_t *tc = &thread_data->target_cache;
    for (int i = 0; i < tc->count; i++) {
        if (tc->entries[i].valid) {
            EVP_DigestUpdate(sha_ctx, &tc->entries[i].addr, sizeof(uint64_t));
            EVP_DigestUpdate(sha_ctx, &tc->entries[i].label, sizeof(uint32_t));
        }
    }

    uint8_t hash[32];
    EVP_DigestFinal_ex(sha_ctx, hash, NULL);
    EVP_MD_CTX_free(sha_ctx);

    TEEC_Operation op = {0};
    uint32_t err;
    TEEC_SharedMemory shm;
    shm.size = 32;
    shm.flags = TEEC_MEM_INPUT;
    TEEC_Result res = TEEC_AllocateSharedMemory(&ctx, &shm);
    if (res != TEEC_SUCCESS) {
        fprintf(stderr, "[CFI-CLIENT] Failed to allocate shared memory\n");
        CFI_T_END("cfi_verify_shadow_stack_integrity_error");
        return -1;
    }

    memcpy(shm.buffer, hash, 32);
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_MEMREF_WHOLE, TEEC_VALUE_OUTPUT, TEEC_NONE);
    op.params[0].value.a = (uint32_t)rss->tid;
    op.params[0].value.b = (uint32_t)rss->sp;
    op.params[1].memref.parent = &shm;
    op.params[1].memref.size = 32;
    op.params[1].memref.offset = 0;

    CFI_T_START();
    res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_VERIFY_INTEGRITY, &op, &err);
    CFI_T_END("tee_verify_integrity");

    TEEC_ReleaseSharedMemory(&shm);

    if (res != TEEC_SUCCESS) {
        fprintf(stderr, "[CFI-CLIENT] Integrity check invoke failed: 0x%x\n", res);
        CFI_T_END("cfi_verify_shadow_stack_integrity_error");
        return -1;
    }

    uint32_t result = op.params[2].value.a;
    switch (result) {
        case CFI_INTEGRITY_OK:
            g_cfi_state.integrity_pass_count++;
            CFI_T_END("cfi_verify_shadow_stack_integrity");
            return 0;
        case CFI_INTEGRITY_FIRST:
            printf("[CFI-CLIENT] Baseline established (tid=%d)\n", rss->tid);
            CFI_T_END("cfi_verify_shadow_stack_integrity_baseline_establish");
            return 2;
        case CFI_INTEGRITY_FAIL:
            fprintf(stderr, "[CFI-CLIENT] *** INTEGRITY FAIL *** tid=%d\n", rss->tid);
            g_cfi_state.integrity_fail_count++;
            cfi_log_event(CFI_EVENT_INTEGRITY_FAIL_SS, 0, 0, 0,
                          "Shadow stack hash mismatch");
            trigger_degrade(CFI_EVENT_INTEGRITY_FAIL_SS,
                           "Shadow stack integrity failure");
            CFI_T_END("cfi_verify_shadow_stack_integrity_trigger_degrade");
            return 1;
        default:
            fprintf(stderr, "[CFI-CLIENT] Unknown integrity result: %u\n", result);
            CFI_T_END("cfi_verify_shadow_stack_integrity_error");
            return -1;
    }
}

/* ---------- DEGRADED mode：golden_cache integrity verification ---------- */
static int cfi_verify_golden_cache_integrity(void) {
    CFI_T_START();

    /* Count the number of valid entries */
    uint32_t valid_count = 0;
    for (int i = 0; i < golden_cache_count; i++) {
        if (golden_cache[i].valid) valid_count++;
    }

    EVP_MD_CTX *sha_ctx = EVP_MD_CTX_new();
    if (!sha_ctx) {
    	CFI_T_END("cfi_verify_golden_cache_error");
        return -1;
    }

    EVP_DigestInit_ex(sha_ctx, EVP_sha256(), NULL);

    for (int i = 0; i < golden_cache_count; i++) {
        if (golden_cache[i].valid) {
            EVP_DigestUpdate(sha_ctx, &golden_cache[i].tag, sizeof(uint32_t));
            EVP_DigestUpdate(sha_ctx, &golden_cache[i].golden_addr, sizeof(uint64_t));
            int valid_flag = 1;
            EVP_DigestUpdate(sha_ctx, &valid_flag, sizeof(int));
        }
    }

    uint8_t hash[32];
    EVP_DigestFinal_ex(sha_ctx, hash, NULL);
    EVP_MD_CTX_free(sha_ctx);

    TEEC_Operation op = {0};
    uint32_t err;
    TEEC_SharedMemory shm;
    shm.size = 32;
    shm.flags = TEEC_MEM_INPUT;
    TEEC_Result res = TEEC_AllocateSharedMemory(&ctx, &shm);
    if (res != TEEC_SUCCESS) {
        fprintf(stderr, "[CFI-CLIENT] Failed to allocate shared memory for golden cache\n");
        CFI_T_END("cfi_verify_golden_cache_error");
        return -1;
    }

    memcpy(shm.buffer, hash, 32);
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_WHOLE, TEEC_VALUE_INPUT, TEEC_VALUE_OUTPUT, TEEC_NONE);
    op.params[0].memref.parent = &shm;
    op.params[0].memref.size = 32;
    op.params[0].memref.offset = 0;
    op.params[1].value.a = valid_count;  /* Transmitting the number of valid entries */

    
    CFI_T_START();
    res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_VERIFY_GOLDEN_CACHE_INTEGRITY, &op, &err);
    CFI_T_END("tee_verify_golden_cache");

    TEEC_ReleaseSharedMemory(&shm);
    
    if (res != TEEC_SUCCESS) {
        fprintf(stderr, "[CFI-CLIENT] Golden cache integrity check invoke failed: 0x%x\n", res);
        CFI_T_END("cfi_verify_golden_cache_error");
        return -1;
    }

    uint32_t result = op.params[2].value.a;
    switch (result) {
        case CFI_INTEGRITY_OK:
            g_cfi_state.integrity_pass_count++;
            CFI_T_END("cfi_verify_golden_cache");
            return 0;
        case CFI_INTEGRITY_FIRST:
            printf("[CFI-CLIENT] Golden cache baseline established/updated (count=%u)\n", valid_count);
            CFI_T_END("cfi_verify_golden_cache_baseline_establish");
            return 2;
        case CFI_INTEGRITY_FAIL:
            fprintf(stderr, "[CFI-CLIENT] *** GOLDEN CACHE INTEGRITY FAIL ***\n");
            g_cfi_state.integrity_fail_count++;
            cfi_log_event(CFI_EVENT_INTEGRITY_FAIL_GOLDEN_CACHE, 0, 0, 0,
                          "Golden cache hash mismatch - REE cache tampered");
            trigger_degrade(CFI_EVENT_INTEGRITY_FAIL_GOLDEN_CACHE,
                           "Golden cache tampered in DEGRADED mode");
            CFI_T_END("cfi_verify_golden_cache_trigger_degrade");
            return 1;
        default:
            fprintf(stderr, "[CFI-CLIENT] Unknown golden cache integrity result: %u\n", result);
            CFI_T_END("cfi_verify_golden_cache_error");
            return -1;
    }
}

/* ---------- Main entrance: Distribution by mode ---------- */
int cfi_verify_integrity(void) {
    cfi_protection_mode_t mode = cfi_get_protection_mode();

    if (mode == CFI_MODE_SURVIVAL) {
        /* SURVIVAL: No REE verification is performed, and all golden verifications are completed within TEE */
        return 0;
    }

    if (mode == CFI_MODE_DEGRADED) {
        /* DEGRADED: REE shadow stack is no longer trusted, please verify the integrity of golden_cache instead */
        return cfi_verify_golden_cache_integrity();
    }

    /* NORMAL / SUSPECT: verify the integrity of REE shadow stack */
    return cfi_verify_shadow_stack_integrity();
}

/*
 * Timing Security Encapsulation: Preventing misalignment of nested timing stacks
 * When an internal function is called trigger_degrade -> cfi_record_attack
 * The timing stack may be damaged. This package saves/restores the timing stack pointer to ensure isolation.
 */
int cfi_verify_integrity_safe(void) {
    int saved_sp = cfi_timing_sp;
    int result = cfi_verify_integrity();
    if (cfi_timing_sp < 0) {
        fprintf(stderr, "[CFI-TIMING] WARNING: timing stack underflow detected, resetting to 0\n");
        cfi_timing_sp = 0;
    }
    /* If the timing stack is damaged by internal operations, restore it to the state it was in when it entered */
    while (cfi_timing_sp > saved_sp) {
        cfi_timing_sp--;
    }
    return result;
}

/* ============================================================================
 * Learning Mode Management (Approach 3: Learning + Freeze)
 * ============================================================================ */

/**
 * cfi_freeze - Freeze the Golden backup table and end the learning phase
 *
 * After calling, any new (tag, addr) pairs will be rejected by TEE (considered as freeze bread)
 * Call timing: After the PLC program is loaded and all valid call points have been recorded
 */
void cfi_freeze(void) {
    CFI_T_START();
    if (!initialized) { CFI_T_END("cfi_freeze_error"); return; }

    TEEC_Operation op = {0};
    uint32_t err;
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE);
    op.params[0].value.a = 1;  /* 1 = FROZEN */
    CFI_T_END("cfi_freeze_op");
    CFI_T_START();
    TEEC_Result res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_SET_LEARNING_MODE, &op, &err);
    CFI_T_END("tee_set_learning_mode");
    CFI_T_START();
    if (res == TEEC_SUCCESS) {
        pthread_mutex_lock(&learning_mutex);
        learning_mode_active = 0;
        pthread_mutex_unlock(&learning_mutex);
        printf("[CFI-CLIENT] Golden table FROZEN. New call sites will be rejected.\n");
    } else {
        fprintf(stderr, "[CFI-CLIENT] Failed to freeze: 0x%x\n", res);
    }
    CFI_T_END("cfi_freeze");
}

/**
 * cfi_is_learning - Check if the current stage is in the learning phase
 * @return: 1=Learning stage (before freezing), 0=frozen
 */
int cfi_is_learning(void) {
    return learning_mode_active;
}

/**
 * cfi_clear_golden - Clear the golden backup table and restart learning
 *
 * When used for PLC program changes: the new program may have different calling relationships and needs to be relearned
 * Automatically return to learning mode
 */
void cfi_clear_golden(void) {
    if (!initialized) { return;}

    TEEC_Operation op = {0};
    uint32_t err;
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_NONE, TEEC_NONE, TEEC_NONE);
    TEEC_Result res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_CLEAR_GOLDEN, &op, &err);
    if (res == TEEC_SUCCESS) {
        pthread_mutex_lock(&learning_mutex);
        learning_mode_active = 1;
        freeze_breach_count = 0;
        pthread_mutex_unlock(&learning_mutex);
        printf("[CFI-CLIENT] Golden table CLEARED. Back to learning mode.\n");
    } else {
        fprintf(stderr, "[CFI-CLIENT] Failed to clear golden: 0x%x\n", res);
    }
}


/* ============================================================================
 * Runtime Stage - Three-Layer Architecture
 * ============================================================================
 * Layer 1: Real-Time Status Monitoring & Forensic Event Logging
 * Layer 2: Adaptive Response (dynamic period + progressive degrade)
 * Layer 3: Operator Command Interface (Unix Domain Socket)
 * ============================================================================ */

/* ---------- Layer 1: Global State & Event Logging ---------- */

/* Non-static definitions (declared extern in forward decl section above) */
cfi_runtime_state_t g_cfi_state = {
    .status = CFI_RUNTIME_STATUS_NORMAL,
    .current_integrity_period = 100,
};
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;

cfi_protection_mode_t current_mode = CFI_MODE_NORMAL;


volatile int cfi_safety_stop_requested = 0;
pthread_t operator_thread;
volatile int operator_server_running = 0;

const char* cfi_event_type_name(cfi_event_type_t type) {
    switch (type) {
        case CFI_EVENT_FREEZE_BREACH:      return "FREEZE_BREACH";
        case CFI_EVENT_INTEGRITY_FAIL_SS:  return "INTEGRITY_FAIL_SS";
        case CFI_EVENT_INTEGRITY_FAIL_GOT: return "INTEGRITY_FAIL_GOT";
        case CFI_EVENT_DEGRADE_TRIGGERED:  return "DEGRADE_TRIGGERED";
        case CFI_EVENT_PAC_FAULT_RECOVERED:return "PAC_FAULT_RECOVERED";
        case CFI_EVENT_PAC_FAULT_FATAL:    return "PAC_FAULT_FATAL";
        case CFI_EVENT_FORWARD_VIOLATION:  return "FORWARD_VIOLATION";
        case CFI_EVENT_MANUAL_CHECK:       return "MANUAL_CHECK";
        case CFI_EVENT_MODE_CHANGE:        return "MODE_CHANGE";
        case CFI_EVENT_INTEGRITY_FAIL_GOLDEN_CACHE: return "INTEGRITY_FAIL_GC";
        case CFI_EVENT_AUTO_DEGRADE:        return "AUTO_DEGRADE";
        default:                           return "UNKNOWN";
    }
}

const char* cfi_status_name(cfi_runtime_status_t s) {
    switch (s) {
        case CFI_RUNTIME_STATUS_NORMAL:    return "NORMAL";
        case CFI_RUNTIME_STATUS_SUSPECT:   return "SUSPECT";
        case CFI_RUNTIME_STATUS_DEGRADED:  return "DEGRADED";
        case CFI_RUNTIME_STATUS_EMERGENCY: return "EMERGENCY";
        default:                   return "UNKNOWN";
    }
}

const char* cfi_mode_name(cfi_protection_mode_t m) {
    switch (m) {
        case CFI_MODE_NORMAL:   return "NORMAL";
        case CFI_MODE_SUSPECT:  return "SUSPECT";
        case CFI_MODE_DEGRADED: return "DEGRADED";
        case CFI_MODE_SURVIVAL: return "SURVIVAL";
        default:                return "UNKNOWN";
    }
}

const cfi_runtime_state_t* cfi_get_runtime_state(void) {
    return &g_cfi_state;
}

void cfi_log_event(cfi_event_type_t type, uint32_t tag,
                   uint64_t expected, uint64_t actual,
                   const char* context) {
    pthread_mutex_lock(&g_state_mutex);

    uint32_t idx = g_cfi_state.event_count % 64;
    cfi_event_record_t *ev = &g_cfi_state.events[idx];

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ev->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    ev->type = type;
    ev->tag = tag;
    ev->expected_addr = expected;
    ev->actual_addr = actual;
    ev->tid = 0; /* will be filled by caller if needed */
    ev->cycle_count = g_cfi_state.cycle_count;
    if (context) {
        strncpy(ev->context, context, sizeof(ev->context) - 1);
        ev->context[sizeof(ev->context) - 1] = '\0';
    } else {
        ev->context[0] = '\0';
    }

    g_cfi_state.event_count++;

    /* Update counters */
    switch (type) {
        case CFI_EVENT_FREEZE_BREACH:      g_cfi_state.freeze_breach_count++; break;
        case CFI_EVENT_DEGRADE_TRIGGERED:  g_cfi_state.degrade_count++; break;
        case CFI_EVENT_PAC_FAULT_RECOVERED: g_cfi_state.pac_recover_count++; break;
        case CFI_EVENT_PAC_FAULT_FATAL:    g_cfi_state.pac_fault_count++; break;
        case CFI_EVENT_FORWARD_VIOLATION:  g_cfi_state.forward_violation_count++; break;
        default: break;
    }

    pthread_mutex_unlock(&g_state_mutex);
}

void cfi_print_event_log(void) {
    pthread_mutex_lock(&g_state_mutex);
    uint32_t total = g_cfi_state.event_count;
    uint32_t start = (total > 64) ? (total - 64) : 0;

    printf("\n+===========================================================================+\n");
    printf("|     CFI Runtime Event Log (showing last %u of %u events)                  |\n",
           (total > 64) ? 64 : total, total);
    printf("+==================+==========+=========+==========+==========+=============+\n");
    printf("| Event            | Cycle    | Tag     | Expected | Actual   | Context     |\n");
    printf("+==================+==========+=========+==========+==========+=============+\n");

    for (uint32_t i = start; i < total; i++) {
        cfi_event_record_t *ev = &g_cfi_state.events[i % 64];
        printf("| %-16s | %8u | 0x%08X | %08lX | %08lX | %-11s |\n",
               cfi_event_type_name(ev->type),
               ev->cycle_count,
               ev->tag,
               (unsigned long)ev->expected_addr,
               (unsigned long)ev->actual_addr,
               ev->context);
    }
    printf("+==================+==========+=========+==========+==========+=============+\n");
    pthread_mutex_unlock(&g_state_mutex);
}

void cfi_print_status_banner(void) {
    const cfi_runtime_state_t* s = cfi_get_runtime_state();
    printf("\n+================== CFI Runtime Status ==================+\n");
    printf("| Status:        %-8s  Mode:        %-8s       |\n",
           cfi_status_name(s->status), cfi_mode_name(current_mode));
    printf("| Degraded:      %-4d      Int.Period:  %-4d cycles     |\n",
           ree_degraded, s->current_integrity_period);
    printf("| Integrity:     %u/%u pass   FreezeBreaches: %u           |\n",
           s->integrity_pass_count,
           s->integrity_pass_count + s->integrity_fail_count,
           s->freeze_breach_count);
    printf("| PAC faults:    %u (recov %u)  Degrades:      %u           |\n",
           s->pac_fault_count, s->pac_recover_count, s->degrade_count);
    printf("| Attacks:       %u/%u       AutoDegrades:  %u           |\n",
           s->attack_count, s->attack_threshold, s->auto_degrade_count);
    printf("| Cycles:        %-8u  Events:      %-4u            |\n",
           s->cycle_count, s->event_count);
    printf("+========================================================+\n");
}

/* ---------- Layer 2: Adaptive Response ---------- */

void cfi_set_protection_mode(cfi_protection_mode_t mode) {
    CFI_T_START();
    pthread_mutex_lock(&g_mode_mutex);
    cfi_protection_mode_t old = current_mode;
    current_mode = mode;

    switch (mode) {
        case CFI_MODE_NORMAL:
            ree_degraded = 0;
            g_cfi_state.status = CFI_RUNTIME_STATUS_NORMAL;
            g_cfi_state.current_integrity_period = 100;
            g_cfi_state.attack_count = 0;
            g_cfi_state.attack_threshold = CFI_ATTACK_THRESHOLD;
            CFI_T_END("cfi_set_protection_mode");
            break;
        case CFI_MODE_SUSPECT:
            ree_degraded = 0;
            g_cfi_state.status = CFI_RUNTIME_STATUS_SUSPECT;
            g_cfi_state.current_integrity_period = 50;
            g_cfi_state.attack_count = 0;
            g_cfi_state.attack_threshold = CFI_ATTACK_THRESHOLD;
            CFI_T_END("cfi_set_protection_mode");
            break;
        case CFI_MODE_DEGRADED:
            ree_degraded = 1;
            g_cfi_state.status = CFI_RUNTIME_STATUS_DEGRADED;
            g_cfi_state.current_integrity_period = 25;
            g_cfi_state.attack_count = 0;
            g_cfi_state.attack_threshold = CFI_ATTACK_THRESHOLD;
            CFI_T_END("cfi_set_protection_mode");
            break;
        case CFI_MODE_SURVIVAL:
            ree_degraded = 1;
            g_cfi_state.status = CFI_RUNTIME_STATUS_EMERGENCY;
            g_cfi_state.current_integrity_period = 10;
            g_cfi_state.attack_count = 0;
            g_cfi_state.attack_threshold = CFI_ATTACK_THRESHOLD;
            /* Clear all REE caches and force all TEE to run */
            cfi_clear_all_caches();
            CFI_T_END("cfi_set_protection_mode");
            break;
        default:
	    pthread_mutex_unlock(&g_mode_mutex);
	    CFI_T_END("cfi_set_protection_mode_error");
	    return;
    }
    pthread_mutex_unlock(&g_mode_mutex);

    cfi_log_event(CFI_EVENT_MODE_CHANGE, (uint32_t)mode, old, mode,
                  cfi_mode_name(mode));
}

cfi_protection_mode_t cfi_get_protection_mode(void) {
    return current_mode;
}

void cfi_adapt_integrity_period(cfi_runtime_status_t new_status) {
    switch (new_status) {
        case CFI_RUNTIME_STATUS_NORMAL:
            g_cfi_state.current_integrity_period = 100;
            break;
        case CFI_RUNTIME_STATUS_SUSPECT:
            g_cfi_state.current_integrity_period = 50;
            break;
        case CFI_RUNTIME_STATUS_DEGRADED:
        case CFI_RUNTIME_STATUS_EMERGENCY:
            g_cfi_state.current_integrity_period = 25;
            break;
    }
}

int cfi_get_current_integrity_period(void) {
    return g_cfi_state.current_integrity_period;
}


/* ============================================================================
 * Automatic downgrade management
 * ============================================================================
 * Count each time an attack is detected, and automatically lower the protection mode 
 * by one level when the threshold is reached.
 * After degrading, the attack count is reset to zero and accumulates again in the new mode.
 * ============================================================================ */
static void cfi_record_attack(cfi_event_type_t type, const char *context) {
    /* 1. Record events */
    cfi_log_event(type, 0, 0, 0, context);

    /* 2. Update attack count */
    pthread_mutex_lock(&g_state_mutex);
    g_cfi_state.attack_count++;
    uint32_t count = g_cfi_state.attack_count;
    uint32_t threshold = g_cfi_state.attack_threshold;
    pthread_mutex_unlock(&g_state_mutex);

    /* 3. Check if the automatic downgrade threshold has been reached */
    if (count >= threshold) {
        cfi_protection_mode_t current = cfi_get_protection_mode();
        cfi_protection_mode_t next = current;

        switch (current) {
            case CFI_MODE_NORMAL:   next = CFI_MODE_SUSPECT;  break;
            case CFI_MODE_SUSPECT:  next = CFI_MODE_DEGRADED; break;
            case CFI_MODE_DEGRADED: next = CFI_MODE_SURVIVAL; break;
            case CFI_MODE_SURVIVAL:
            default:
                fprintf(stderr, "[CFI-CLIENT] Attack threshold reached in SURVIVAL, "
                        "no further degrade possible. Staying in SURVIVAL.\n");
                return;
        }

        fprintf(stderr, "\n[CFI-CLIENT] *** AUTO-DEGRADE TRIGGERED ***\n");
        fprintf(stderr, "[CFI-CLIENT] Attack count %u reached threshold %u\n", count, threshold);
        fprintf(stderr, "[CFI-CLIENT] Auto-degrading: %s -> %s\n",
                cfi_mode_name(current), cfi_mode_name(next));
        fprintf(stderr, "[CFI-CLIENT] All REE caches invalidated.\n\n");

        cfi_set_protection_mode(next);

        pthread_mutex_lock(&g_state_mutex);
        g_cfi_state.auto_degrade_count++;
        pthread_mutex_unlock(&g_state_mutex);

        cfi_log_event(CFI_EVENT_AUTO_DEGRADE, (uint32_t)current, (uint32_t)next, 0,
                      "Automatic protection mode degrade due to repeated attacks");
    }

}

/* ---------- Layer 3: Operator Command Interface ---------- */

int cfi_request_safety_stop(void) {
    cfi_safety_stop_requested = 1;
    cfi_log_event(CFI_EVENT_MANUAL_CHECK, 0, 0, 0, "Safety stop requested by operator");
    return 0;
}

int cfi_operator_handle_cmd(cfi_operator_request_t* req, cfi_operator_response_t* resp) {
    resp->status = 0;
    resp->msg[0] = '\0';

    switch (req->cmd) {
        case CMD_GET_STATUS: {
            const cfi_runtime_state_t* s = cfi_get_runtime_state();
            snprintf(resp->msg, sizeof(resp->msg),
                "status=%s mode=%s degrade=%d "
                "integrity=%u/%u events=%u period=%d safety_stop=%d "
                "attacks=%u/%u autodegrades=%u",
                cfi_status_name(s->status),
                cfi_mode_name(current_mode),
                ree_degraded,
                s->integrity_pass_count,
                s->integrity_pass_count + s->integrity_fail_count,
                s->event_count,
                s->current_integrity_period,
                cfi_safety_stop_requested,
                s->attack_count,
                s->attack_threshold,
                s->auto_degrade_count);
            break;
        }
        case CMD_GET_EVENTS:
            cfi_print_event_log();
            snprintf(resp->msg, sizeof(resp->msg),
                "Event log printed (total %u events)", g_cfi_state.event_count);
            break;

        case CMD_MANUAL_INTEGRITY: {
            int r = cfi_verify_integrity();
            g_cfi_state.integrity_check_count++;
            if (r == 0) g_cfi_state.integrity_pass_count++;
            if (r == 1) g_cfi_state.integrity_fail_count++;
            snprintf(resp->msg, sizeof(resp->msg),
                "Manual integrity check: %s",
                r == 0 ? "PASS" : (r == 2 ? "BASELINE" : "FAIL"));
            cfi_log_event(CFI_EVENT_MANUAL_CHECK, 0, 0, 0, resp->msg);
            resp->status = r;
            break;
        }
        case CMD_SET_MODE_NORMAL: {
            /* Require integrity check before returning to NORMAL */
            int r = cfi_verify_integrity();
            if (r == 0 || r == 2) {
                cfi_set_protection_mode(CFI_MODE_NORMAL);
                snprintf(resp->msg, sizeof(resp->msg),
                    "Mode set to NORMAL (integrity pre-check %s)",
                    r == 0 ? "PASS" : "BASELINE");
            } else {
                resp->status = -1;
                snprintf(resp->msg, sizeof(resp->msg),
                    "Cannot set NORMAL: integrity check failed");
            }
            break;
        }
        case CMD_SET_MODE_SUSPECT:
            cfi_set_protection_mode(CFI_MODE_SUSPECT);
            snprintf(resp->msg, sizeof(resp->msg), "Mode set to SUSPECT");
            break;
        case CMD_SET_MODE_DEGRADED:
            cfi_set_protection_mode(CFI_MODE_DEGRADED);
            snprintf(resp->msg, sizeof(resp->msg), "Mode set to DEGRADED");
            break;
        case CMD_SET_MODE_SURVIVAL:
            cfi_set_protection_mode(CFI_MODE_SURVIVAL);
            snprintf(resp->msg, sizeof(resp->msg), "Mode set to SURVIVAL");
            break;
        case CMD_SET_INTEGRITY_PERIOD:
            g_cfi_state.current_integrity_period = (int)req->arg;
            snprintf(resp->msg, sizeof(resp->msg),
                "Integrity period set to %d cycles", (int)req->arg);
            break;
        case CMD_REQUEST_SAFETY_STOP:
            cfi_request_safety_stop();
            snprintf(resp->msg, sizeof(resp->msg), "Safety stop requested");
            break;
        case CMD_CLEAR_DEGRADE:
            cfi_set_protection_mode(CFI_MODE_NORMAL);
            snprintf(resp->msg, sizeof(resp->msg),
                "Degrade cleared, mode set to NORMAL");
            break;
        default:
            resp->status = -1;
            snprintf(resp->msg, sizeof(resp->msg), "Unknown command");
            break;
    }
    return resp->status;
}

static void* cfi_operator_server_thread(void* arg) {
    (void)arg;
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[CFI-OP] socket");
        return NULL;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/openplc_cfi.sock", sizeof(addr.sun_path) - 1);
    unlink(addr.sun_path);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[CFI-OP] bind");
        close(sock);
        return NULL;
    }
    listen(sock, 5);
    operator_server_running = 1;
    printf("[CFI-OP] Operator server listening on %s\n", addr.sun_path);

    while (operator_server_running && initialized) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(sock + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        int client = accept(sock, NULL, NULL);
        if (client < 0) continue;

        cfi_operator_request_t req = {0};
        ssize_t n = recv(client, &req, sizeof(req), 0);
        if (n == sizeof(req)) {
            cfi_operator_response_t resp = {0};
            cfi_operator_handle_cmd(&req, &resp);
            send(client, &resp, sizeof(resp), 0);
        }
        close(client);
    }

    close(sock);
    unlink(addr.sun_path);
    printf("[CFI-OP] Operator server stopped\n");
    return NULL;
}

void cfi_start_operator_server(void) {
    if (!operator_server_running) {
        pthread_create(&operator_thread, NULL, cfi_operator_server_thread, NULL);
    }
}

void cfi_stop_operator_server(void) {
    operator_server_running = 0;
    pthread_join(operator_thread, NULL);
}


/* ============================================================================
 * Timing Statistics Report
 * ============================================================================ */
#if CFI_TIMING
void cfi_print_timing_stats(void) {
    pthread_mutex_lock(&cfi_timing_mutex);

    printf("\n");
    printf("+===========================================================================+\n");
    printf("|     PAC-BTI + REE/TEE Shadow Stack - Timing Statistics                    |\n");
    printf("|     (clock_gettime CLOCK_MONOTONIC, unit: us unless noted)                |\n");
    printf("+===============================+=========+==========+==========+==========+\n");
    printf("| Component                     | Count   | Total(ms)| Avg(us)  | Max(us)  |\n");
    printf("+===============================+=========+==========+==========+==========+\n");

    uint64_t grand_total_ns = 0;
    const char *last_category = NULL;
    for (int i = 0; i < cfi_timing_entry_count; i++) {
        cfi_timing_entry_t *e = &cfi_timing_entries[i];
        double total_ms = e->total_ns / 1000000.0;
        double avg_us = (e->count > 0) ? (e->total_ns / e->count) / 1000.0 : 0;
        double max_us = e->max_ns / 1000.0;

        /* Auto-detect category by prefix */
        const char *category = NULL;
        if (strstr(e->name, "init") || strstr(e->name, "fini") || strstr(e->name, "freeze"))
            category = "Lifecycle";
        else if (strstr(e->name, "push_addr") || strstr(e->name, "check_addr") || strstr(e->name, "epilogue"))
            category = "Backward-Edge";
        else if (strstr(e->name, "verify_forward") || strstr(e->name, "register_target") || strstr(e->name, "callback") || strstr(e->name, "signals"))
            category = "Forward-Edge";
        else if (strstr(e->name, "verify_integrity") || strstr(e->name, "got_verify"))
            category = "Periodic";
        else if (strstr(e->name, "register_got") || strstr(e->name, "thread_reg") || strstr(e->name, "learning"))
            category = "TEE World-Switch";
        else
            category = "Other";

        if (category && (!last_category || strcmp(category, last_category) != 0)) {
            printf("| [%s]%*s |         |          |          |          |\n",
                   category, (int)(27 - strlen(category)), "");
            last_category = category;
        }

        printf("| %-29s | %7lu | %8.2f | %8.3f | %8.3f |\n",
               e->name, e->count, total_ms, avg_us, max_us);
        grand_total_ns += e->total_ns;
    }

    printf("+===============================+=========+==========+==========+==========+\n");
    printf("| GRAND TOTAL                   |         | %8.2f |          |          |\n",
           grand_total_ns / 1000000.0);
    printf("+===============================+=========+==========+==========+==========+\n");

    printf("| Legend: cfi_*  = REE full op (inc. TEE round-trip where applicable)      |\n");
    printf("|         tee_*  = TEE round-trip only (TEEC_InvokeCommand)                 |\n");
    printf("|         fwd_*  = Forward-edge CFI (indirect call target verification)    |\n");
    printf("| 分析: REE-only = cfi_* - tee_* (local REE portion of a hybrid op)       |\n");
    printf("+===========================================================================+\n");

    pthread_mutex_unlock(&cfi_timing_mutex);
}
#endif /* CFI_TIMING */
