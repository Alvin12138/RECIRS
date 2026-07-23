#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>
#include <string.h>
#include <cfi_ta.h>

#define MAX_STACK_DEPTH 1024

/* ============================================================================
 * OpenPLC CFI Trusted Application
 * Backward-edge: Shadow Stack + Golden Backup (Approach 3: Learning + Freeze)
 * Forward-edge: Runtime Target Verification with Auto-Learn
 * GOT/PLT: Integrity Check
 * ============================================================================
 *
 * Golden Backup - Learning Phase + Freeze Design:
 *
 *   Learning phase (boot/startup): push_backup records all (tag, addr) pairs.
 *                                  New call sites are welcomed and recorded.
 *   Freeze phase (normal): push_backup with existing pair = OK.
 *                          push_backup with NEW pair = VIOLATION.
 *   PLC program change: Clear golden table via CLEAR_GOLDEN, restart learning.
 *
 *   This gives "one golden backup per OBSERVED call site" semantics.
 *   An attacker cannot introduce new call sites after freeze.
 * ============================================================================ */

/* ========== Global security status ========== */
static uint32_t global_status = CFI_STATUS_NORMAL;

/* ========== Learning mode status ========== */
static uint32_t learning_mode = CFI_LEARNING_ACTIVE;  /* Learning by default upon startup */
static uint32_t freeze_breach_count = 0;
static uint32_t probe_alert_count = 0;

/* ========== Backward-edge: Shadow stack structure ========== */
typedef struct {
    uint64_t stack[MAX_STACK_DEPTH];
    int sp;
    int active;
} thread_shadow_stack_t;

static thread_shadow_stack_t thread_stacks[MAX_THREADS];
static int thread_count = 0;

/* ========== Backward-edge: Golden backup table (supports multiple addresses) ========== */
typedef struct {
    uint32_t tag;
    uint64_t golden_addr;
    int valid;
} golden_entry_t;

static golden_entry_t golden_table[MAX_BACKUP_ENTRIES];
static int golden_count = 0;

/* ========== Forward-edge: Legitimate target table ========== */
typedef struct {
    uint64_t func_addr;
    uint32_t label;
    uint32_t type_class;
    int valid;
} target_entry_t;

static target_entry_t target_table[MAX_TARGETS];
static int target_count = 0;

/* ========== GOT/PLT Protection table ========== */
typedef struct {
    uint64_t plt_addr;
    uint64_t got_target;
    int valid;
} got_entry_t;

static got_entry_t got_table[MAX_GOT_ENTRIES];
static int got_count = 0;

/* ========== Integrity verification status ========== */
typedef struct {
    uint8_t expected_hash[32];
    int has_baseline;
    uint32_t verify_count;
    uint32_t fail_count;
} integrity_state_t;

static integrity_state_t integrity_states[MAX_THREADS];

typedef struct {
    uint8_t expected_hash[32];
    int has_baseline;
    uint32_t verify_count;
    uint32_t fail_count;
} got_integrity_state_t;

static got_integrity_state_t got_integrity = {0};

/* TEE internel golden table integrity baseline */
static uint8_t golden_table_baseline[32] = {0};
static int golden_table_baseline_set = 0;

static void compute_golden_table_hash(uint8_t *hash_out) {
    uint64_t h[4] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL
    };
    for (int i = 0; i < golden_count; i++) {
        if (golden_table[i].valid) {
            uint64_t tag = golden_table[i].tag;
            uint64_t addr = golden_table[i].golden_addr;
            h[0] ^= tag; h[0] = (h[0] << 17) | (h[0] >> 47); h[0] += 0x9e3779b97f4a7c15ULL;
            h[1] ^= addr; h[1] = (h[1] << 19) | (h[1] >> 45); h[1] += 0x9e3779b97f4a7c15ULL;
            h[2] ^= h[0] ^ h[1]; h[2] = (h[2] << 23) | (h[2] >> 41);
            h[3] ^= h[1] ^ h[2]; h[3] = (h[3] << 29) | (h[3] >> 35);
        }
    }
    for (int i = 0; i < 4; i++) { h[i] ^= h[(i+1)%4]; h[i] = (h[i] << 31) | (h[i] >> 33); }
    memcpy(hash_out, h, 32);
}


static uint32_t type_class_counts[CFI_TARGET_TYPE_MAX] = {0};
static uint32_t forward_verify_count = 0;
static uint32_t forward_violation_count = 0;

/* Forward declaration */
TEE_Result TA_CreateEntryPoint(void);
void TA_DestroyEntryPoint(void);
TEE_Result TA_OpenSessionEntryPoint(uint32_t pt, TEE_Param params[4], void **sess);
void TA_CloseSessionEntryPoint(void *sess);
TEE_Result TA_InvokeCommandEntryPoint(void *sess, uint32_t cmd,
                                       uint32_t pt, TEE_Param params[4]);

/* ========== helper function ========== */

/**
 * count_addrs_for_tag - Statistically count how many different golden_dedr there are for a certain tag
 */
static int count_addrs_for_tag(uint32_t tag) {
    int count = 0;
    for (int i = 0; i < golden_count; i++) {
        if (golden_table[i].valid && golden_table[i].tag == tag)
            count++;
    }
    return count;
}

/**
 * is_known_pair - Check if the (tag, addr) pair is already in the golden table
 */
static int is_known_pair(uint32_t tag, uint64_t addr) {
    for (int i = 0; i < golden_count; i++) {
        if (golden_table[i].valid &&
            golden_table[i].tag == tag &&
            golden_table[i].golden_addr == addr)
            return 1;
    }
    return 0;
}

TEE_Result TA_CreateEntryPoint(void) {
    DMSG("CFI TA Created - Backward(Freeze) + Forward + GOT/PLT + Integrity");
    memset(thread_stacks, 0, sizeof(thread_stacks));
    memset(golden_table, 0, sizeof(golden_table));
    memset(target_table, 0, sizeof(target_table));
    memset(got_table, 0, sizeof(got_table));
    memset(integrity_states, 0, sizeof(integrity_states));
    memset(&got_integrity, 0, sizeof(got_integrity));
    memset(type_class_counts, 0, sizeof(type_class_counts));
    global_status = CFI_STATUS_NORMAL;
    learning_mode = CFI_LEARNING_ACTIVE;
    freeze_breach_count = 0;
    probe_alert_count = 0;
    thread_count = 0;
    golden_count = 0;
    target_count = 0;
    got_count = 0;
    forward_verify_count = 0;
    forward_violation_count = 0;
    return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void) {
    DMSG("CFI TA Destroyed - learning=%s breaches=%u probes=%u targets=%d golden=%d",
         learning_mode == CFI_LEARNING_ACTIVE ? "ACTIVE" : "FROZEN",
         freeze_breach_count, probe_alert_count, target_count, golden_count);
}

TEE_Result TA_OpenSessionEntryPoint(uint32_t pt, TEE_Param params[4], void **sess) {
    (void)pt; (void)params; (void)sess;
    return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void *sess) {
    (void)sess;
}

/* ========== Thread management ========== */
static int alloc_thread_slot(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!thread_stacks[i].active) {
            thread_stacks[i].active = 1;
            thread_stacks[i].sp = 0;
            thread_count++;
            return i;
        }
    }
    return -1;
}

static void free_thread_slot(int tid) {
    if (tid >= 0 && tid < MAX_THREADS) {
        thread_stacks[tid].active = 0;
        thread_stacks[tid].sp = 0;
        memset(&integrity_states[tid], 0, sizeof(integrity_state_t));
        thread_count--;
    }
}

static thread_shadow_stack_t *get_thread_stack(int tid) {
    if (tid >= 0 && tid < MAX_THREADS && thread_stacks[tid].active)
        return &thread_stacks[tid];
    return NULL;
}

/* ========== Backward-edge command ========== */
static TEE_Result thread_register(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;
    int tid = alloc_thread_slot();
    if (tid < 0) return TEE_ERROR_OVERFLOW;
    params[0].value.a = (uint32_t)tid;
    params[0].value.b = 0;
    DMSG("Thread registered: tid=%d", tid);
    return TEE_SUCCESS;
}

static TEE_Result thread_unregister(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;
    int tid = (int)params[0].value.a;
    free_thread_slot(tid);
    DMSG("Thread unregistered: tid=%d", tid);
    return TEE_SUCCESS;
}

static TEE_Result push_addr(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;
    int tid = (int)params[0].value.a;
    uint64_t addr = ((uint64_t)params[1].value.a << 32) | params[1].value.b;
    thread_shadow_stack_t *tss = get_thread_stack(tid);
    if (!tss) return TEE_ERROR_BAD_PARAMETERS;
    if (tss->sp >= MAX_STACK_DEPTH) return TEE_ERROR_OVERFLOW;
    tss->stack[tss->sp++] = addr;
    return TEE_SUCCESS;
}

static TEE_Result pop_check(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;
    int tid = (int)params[0].value.a;
    uint64_t addr = ((uint64_t)params[1].value.a << 32) | params[1].value.b;
    thread_shadow_stack_t *tss = get_thread_stack(tid);
    if (!tss) return TEE_ERROR_BAD_PARAMETERS;
    if (tss->sp <= 0) {
        params[2].value.a = 0; params[2].value.b = 0;
        return TEE_ERROR_BAD_STATE;
    }
    uint64_t expected = tss->stack[--tss->sp];
    uint64_t match = (expected == addr) ? 1 : 0;
    params[2].value.a = (uint32_t)(match >> 32);
    params[2].value.b = (uint32_t)match;
    if (!match) {
        EMSG("CFI VIOLATION! tid=%d expected=0x%lx got=0x%lx", tid, expected, addr);
    }
    return TEE_SUCCESS;
}

static TEE_Result get_depth(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;
    int tid = (int)params[0].value.a;
    thread_shadow_stack_t *tss = get_thread_stack(tid);
    if (!tss) return TEE_ERROR_BAD_PARAMETERS;
    params[1].value.a = tss->sp;
    params[1].value.b = 0;
    return TEE_SUCCESS;
}

/* ============================================================================
 * push_backup - Golden Backup with Learning + Freeze (Approach 3)
 *
 * Learning phase (CFI_LEARNING_ACTIVE):
 *   - New (tag, addr) pair: record it as golden
 *   - Existing pair: OK, already known
 *   - Probe check: if tag has > MAX_CALL_SITES_PER_TAG addrs, alert
 *
 * Freeze phase (CFI_LEARNING_FROZEN):
 *   - Existing pair: OK
 *   - New pair: VIOLATION (freeze breach), trigger degrade
 *
 * Return via params[2].value.a:
 *   CFI_BACKUP_OK(0)        = success
 *   CFI_BACKUP_NEW_FROZEN(1)= new pair rejected (frozen)
 *   CFI_BACKUP_PROBE(2)     = probe attack detected
 * ============================================================================ */
static TEE_Result push_backup(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint32_t tag = params[0].value.a;
    uint64_t addr = ((uint64_t)params[1].value.a << 32) | params[1].value.b;

    /* Known (tag, addr) pairs - always OK */
    if (is_known_pair(tag, addr)) {
        params[2].value.a = CFI_BACKUP_OK;
        return TEE_SUCCESS;
    }

    /* ==== Freeze phase: Reject new call points ==== */
    if (learning_mode == CFI_LEARNING_FROZEN) {
        freeze_breach_count++;
        EMSG("FREEZE BREACH! tag=%u addr=0x%lx (freeze_breach #%u)",
             tag, addr, freeze_breach_count);
        params[2].value.a = CFI_BACKUP_NEW_FROZEN;

        if (global_status == CFI_STATUS_NORMAL) {
            global_status = CFI_STATUS_COMPROMISED;
            EMSG("*** SYSTEM DEGRADED: FREEZE BREACH ***");
        }
        return TEE_SUCCESS;  /* Return the violation signal to let REE decide */
    }

    /* ==== Learning stage: Record new call points ==== */

    /* Detection: whether there are too many different addrs for the same tag */
    int addr_count = count_addrs_for_tag(tag);
    if (addr_count >= MAX_CALL_SITES_PER_TAG) {
        probe_alert_count++;
        EMSG("PROBE ALERT! tag=%u has %d distinct addrs (max=%d)",
             tag, addr_count, MAX_CALL_SITES_PER_TAG);
        params[2].value.a = CFI_BACKUP_PROBE;
        /* Still recording (learning stage tolerance), but issuing a warning */
    } else {
        params[2].value.a = CFI_BACKUP_OK;
    }

    if (golden_count >= MAX_BACKUP_ENTRIES)
        return TEE_ERROR_OVERFLOW;

    golden_table[golden_count].tag = tag;
    golden_table[golden_count].golden_addr = addr;
    golden_table[golden_count].valid = 1;
    golden_count++;
    DMSG("Golden backup: tag=%u addr=0x%lx (total=%d, addrs_for_tag=%d)",
         tag, addr, golden_count, addr_count + 1);
    return TEE_SUCCESS;
}

static TEE_Result get_backup(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint32_t tag = params[0].value.a;
    /* Return MOST RECENT entry matching this tag */
    for (int i = golden_count - 1; i >= 0; i--) {
        if (golden_table[i].valid && golden_table[i].tag == tag) {
            uint64_t addr = golden_table[i].golden_addr;
            params[1].value.a = (uint32_t)(addr >> 32);
            params[1].value.b = (uint32_t)addr;
            // DMSG("Golden backup found: tag=%u addr=0x%lx", tag, addr);
            return TEE_SUCCESS;
        }
    }
    EMSG("Golden backup NOT FOUND for tag=%u", tag);
    return TEE_ERROR_ITEM_NOT_FOUND;
}

/* ========== Learning Mode Management Command ========== */

static TEE_Result set_learning_mode(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint32_t mode = params[0].value.a;
    if (mode == 0) {
        learning_mode = CFI_LEARNING_ACTIVE;
        golden_table_baseline_set = 0;
        memset(golden_table_baseline, 0, sizeof(golden_table_baseline));
        DMSG("Learning mode: ACTIVE");
    } else {
        learning_mode = CFI_LEARNING_FROZEN;
        compute_golden_table_hash(golden_table_baseline);
        golden_table_baseline_set = 1;
        DMSG("Learning mode: FROZEN (golden_count=%d, baseline established)", golden_count);
    }
    return TEE_SUCCESS;
}

static TEE_Result get_learning_mode(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    params[0].value.a = learning_mode;
    params[0].value.b = (uint32_t)golden_count;
    return TEE_SUCCESS;
}

static TEE_Result clear_golden(uint32_t pt, TEE_Param params[4]) {
    (void)pt; (void)params;
    DMSG("Golden table cleared: %d entries removed", golden_count);
    memset(golden_table, 0, sizeof(golden_table));
    golden_count = 0;
    freeze_breach_count = 0;
    golden_table_baseline_set = 0;
    memset(golden_table_baseline, 0, sizeof(golden_table_baseline));
    learning_mode = CFI_LEARNING_ACTIVE;
    return TEE_SUCCESS;
}

static TEE_Result verify_golden_table_internal(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint8_t current_hash[32];
    compute_golden_table_hash(current_hash);

    if (!golden_table_baseline_set) {
        memcpy(golden_table_baseline, current_hash, 32);
        golden_table_baseline_set = 1;
        params[0].value.a = CFI_INTEGRITY_FIRST;
        params[0].value.b = (uint32_t)golden_count;
        DMSG("Golden table internal baseline established");
    } else if (memcmp(golden_table_baseline, current_hash, 32) == 0) {
        params[0].value.a = CFI_INTEGRITY_OK;
        params[0].value.b = (uint32_t)golden_count;
    } else {
        params[0].value.a = CFI_INTEGRITY_FAIL;
        params[0].value.b = (uint32_t)golden_count;
        EMSG("GOLDEN TABLE INTERNAL INTEGRITY FAIL! Golden table tampered inside TEE");
        global_status = CFI_STATUS_COMPROMISED;
    }
    return TEE_SUCCESS;
}

/* ========== integrity check ========== */
static TEE_Result verify_integrity(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_MEMREF_INPUT,
                                    TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    int tid = (int)params[0].value.a;
    if (tid < 0 || tid >= MAX_THREADS) return TEE_ERROR_BAD_PARAMETERS;

    integrity_state_t *state = &integrity_states[tid];
    state->verify_count++;

    uint8_t *ree_hash = params[1].memref.buffer;
    uint32_t ree_hash_len = params[1].memref.size;
    if (ree_hash_len != 32 || !ree_hash) return TEE_ERROR_BAD_PARAMETERS;

    if (!state->has_baseline) {
        memcpy(state->expected_hash, ree_hash, 32);
        state->has_baseline = 1;
        params[2].value.a = CFI_INTEGRITY_FIRST;
        params[2].value.b = state->fail_count;
        DMSG("Shadow stack baseline established for tid=%d", tid);
        return TEE_SUCCESS;
    }

    if (memcmp(state->expected_hash, ree_hash, 32) == 0) {
        params[2].value.a = CFI_INTEGRITY_OK;
        params[2].value.b = state->fail_count;
        return TEE_SUCCESS;
    } else {
        state->fail_count++;
        params[2].value.a = CFI_INTEGRITY_FAIL;
        params[2].value.b = state->fail_count;
        EMSG("SHADOW STACK INTEGRITY FAIL! tid=%d fails=%u", tid, state->fail_count);

        if (global_status == CFI_STATUS_NORMAL) {
            global_status = CFI_STATUS_COMPROMISED;
            EMSG("*** SYSTEM DEGRADED: SHADOW STACK TAMPERED ***");
        }
        return TEE_SUCCESS;
    }
}

/* ========== Forward-edge command ========== */

static TEE_Result register_target(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint64_t func_addr = ((uint64_t)params[0].value.a << 32) | params[0].value.b;
    uint32_t label = params[1].value.a;
    uint32_t type_class = params[1].value.b;

    if (type_class >= CFI_TARGET_TYPE_MAX)
        type_class = CFI_TARGET_TYPE_GENERIC;

    for (int i = 0; i < target_count; i++) {
        if (target_table[i].valid && target_table[i].func_addr == func_addr) {
            if (target_table[i].label != label) {
                EMSG("TARGET LABEL CONFLICT! addr=0x%lx", func_addr);
                return TEE_ERROR_BAD_STATE;
            }
            return TEE_SUCCESS;
        }
    }

    if (target_count >= MAX_TARGETS)
        return TEE_ERROR_OVERFLOW;

    target_table[target_count].func_addr = func_addr;
    target_table[target_count].label = label;
    target_table[target_count].type_class = type_class;
    target_table[target_count].valid = 1;
    type_class_counts[type_class]++;
    target_count++;
    return TEE_SUCCESS;
}

static TEE_Result verify_target(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint64_t func_addr = ((uint64_t)params[0].value.a << 32) | params[0].value.b;
    uint32_t expected_label = params[1].value.a;

    forward_verify_count++;

    for (int i = 0; i < target_count; i++) {
        if (target_table[i].valid && target_table[i].func_addr == func_addr) {
            if (target_table[i].label == expected_label) {
                params[2].value.a = 1;
                params[2].value.b = target_table[i].type_class;
                return TEE_SUCCESS;
            } else {
                EMSG("FORWARD TYPE MISMATCH! addr=0x%lx expected_label=%u got_label=%u",
                     func_addr, expected_label, target_table[i].label);
                params[2].value.a = 0;
                params[2].value.b = target_table[i].type_class;
                forward_violation_count++;
                return TEE_SUCCESS;
            }
        }
    }

    /* Unknown target signal: type_class=0xFF for REE to automatically learn */
    params[2].value.a = 0;
    params[2].value.b = 0xFF;
    return TEE_SUCCESS;
}

static TEE_Result register_target_batch(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint8_t *data = params[0].memref.buffer;
    uint32_t count = params[1].value.a;
    uint32_t entry_size = 16;

    if (!data || count == 0 || params[0].memref.size != count * entry_size)
        return TEE_ERROR_BAD_PARAMETERS;

    for (uint32_t i = 0; i < count; i++) {
        if (target_count >= MAX_TARGETS)
            return TEE_ERROR_OVERFLOW;

        uint64_t func_addr = 0;
        uint32_t label = 0;
        uint32_t type_class = 0;
        for (int j = 0; j < 8; j++)
            func_addr |= ((uint64_t)data[i*16+j]) << (j*8);
        for (int j = 0; j < 4; j++) {
            label |= ((uint32_t)data[i*16+8+j]) << (j*8);
            type_class |= ((uint32_t)data[i*16+12+j]) << (j*8);
        }

        if (type_class >= CFI_TARGET_TYPE_MAX)
            type_class = CFI_TARGET_TYPE_GENERIC;

        int dup = 0;
        for (int j = 0; j < target_count; j++) {
            if (target_table[j].valid && target_table[j].func_addr == func_addr) {
                if (target_table[j].label != label)
                    return TEE_ERROR_BAD_STATE;
                dup = 1;
                break;
            }
        }
        if (!dup) {
            target_table[target_count].func_addr = func_addr;
            target_table[target_count].label = label;
            target_table[target_count].type_class = type_class;
            target_table[target_count].valid = 1;
            type_class_counts[type_class]++;
            target_count++;
        }
    }
    return TEE_SUCCESS;
}

static TEE_Result query_target_label(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint64_t func_addr = ((uint64_t)params[0].value.a << 32) | params[0].value.b;

    for (int i = 0; i < target_count; i++) {
        if (target_table[i].valid && target_table[i].func_addr == func_addr) {
            params[1].value.a = target_table[i].label;
            params[1].value.b = target_table[i].type_class;
            return TEE_SUCCESS;
        }
    }
    params[1].value.a = 0;
    params[1].value.b = 0xFFFFFFFF;
    return TEE_ERROR_ITEM_NOT_FOUND;
}

/* ========== GOT/PLT protection ========== */

static TEE_Result register_got(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint64_t plt_addr = ((uint64_t)params[0].value.a << 32) | params[0].value.b;
    uint64_t got_target = ((uint64_t)params[1].value.a << 32) | params[1].value.b;

    for (int i = 0; i < got_count; i++) {
        if (got_table[i].valid && got_table[i].plt_addr == plt_addr) {
            if (got_table[i].got_target != got_target)
                return TEE_ERROR_BAD_STATE;
            return TEE_SUCCESS;
        }
    }

    if (got_count >= MAX_GOT_ENTRIES) return TEE_ERROR_OVERFLOW;
    got_table[got_count].plt_addr = plt_addr;
    got_table[got_count].got_target = got_target;
    got_table[got_count].valid = 1;
    got_count++;
    return TEE_SUCCESS;
}

static TEE_Result register_got_batch(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_NONE,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint8_t *data = params[0].memref.buffer;
    uint32_t count = params[1].value.a;
    uint32_t entry_size = 16;

    if (!data || count == 0 || params[0].memref.size != count * entry_size)
        return TEE_ERROR_BAD_PARAMETERS;

    for (uint32_t i = 0; i < count; i++) {
        if (got_count >= MAX_GOT_ENTRIES) return TEE_ERROR_OVERFLOW;

        uint64_t plt_addr = 0, got_target = 0;
        for (int j = 0; j < 8; j++) {
            plt_addr |= ((uint64_t)data[i*16+j]) << (j*8);
            got_target |= ((uint64_t)data[i*16+8+j]) << (j*8);
        }

        int dup = 0;
        for (int j = 0; j < got_count; j++) {
            if (got_table[j].valid && got_table[j].plt_addr == plt_addr) {
                if (got_table[j].got_target != got_target)
                    return TEE_ERROR_BAD_STATE;
                dup = 1;
                break;
            }
        }
        if (!dup) {
            got_table[got_count].plt_addr = plt_addr;
            got_table[got_count].got_target = got_target;
            got_table[got_count].valid = 1;
            got_count++;
        }
    }
    return TEE_SUCCESS;
}

static TEE_Result verify_got_hash(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint8_t *current_data = params[0].memref.buffer;
    uint32_t count = params[1].value.a;

    if (!current_data || count == 0 || count > (uint32_t)got_count)
        return TEE_ERROR_BAD_PARAMETERS;

    int match = 1;
    for (int i = 0; i < got_count && i < (int)count; i++) {
        if (!got_table[i].valid) continue;

        uint64_t current_target = 0;
        for (int j = 0; j < 8; j++)
            current_target |= ((uint64_t)current_data[i*8+j]) << (j*8);

        if (got_table[i].got_target != current_target) {
            EMSG("GOT MISMATCH! idx=%d", i);
            match = 0;
            break;
        }
    }

    if (match) {
        params[2].value.a = 1;
    } else {
        params[2].value.a = 0;
        if (global_status == CFI_STATUS_NORMAL) {
            global_status = CFI_STATUS_COMPROMISED;
            EMSG("*** SYSTEM DEGRADED: GOT TAMPERED ***");
        }
    }
    params[2].value.b = 0;
    return TEE_SUCCESS;
}

static TEE_Result verify_got_integrity(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint8_t *ree_hash = params[0].memref.buffer;
    uint32_t hash_len = params[0].memref.size;
    uint32_t count = params[1].value.a;

    if (!ree_hash || hash_len != 32)
        return TEE_ERROR_BAD_PARAMETERS;

    got_integrity.verify_count++;

    if (!got_integrity.has_baseline) {
        memcpy(got_integrity.expected_hash, ree_hash, 32);
        got_integrity.has_baseline = 1;
        params[2].value.a = CFI_INTEGRITY_FIRST;
        params[2].value.b = got_integrity.fail_count;
        DMSG("GOT baseline established");
        return TEE_SUCCESS;
    }

    if (memcmp(got_integrity.expected_hash, ree_hash, 32) == 0) {
        params[2].value.a = CFI_INTEGRITY_OK;
        params[2].value.b = got_integrity.fail_count;
        return TEE_SUCCESS;
    } else {
        got_integrity.fail_count++;
        params[2].value.a = CFI_INTEGRITY_FAIL;
        params[2].value.b = got_integrity.fail_count;
        EMSG("GOT INTEGRITY FAIL! fails=%u", got_integrity.fail_count);
        memset(got_integrity.expected_hash, 0, 32);
	memcpy(got_integrity.expected_hash, ree_hash, 32);
        if (global_status == CFI_STATUS_NORMAL) {
            global_status = CFI_STATUS_COMPROMISED;
            EMSG("*** SYSTEM DEGRADED: GOT INTEGRITY FAIL ***");
        }
        return TEE_SUCCESS;
    }
}
/* ============================================================================
 * verify_golden_cache_integrity - DEGRADED mode specific
 *
 * Key fix: golden_cache is a dynamically growing cache and cannot simply compare hashes.
 * Strategy:
 *   1. Record expected_comunt (number of valid entries)
 *   2. count increase → new entry is added legally, baseline is automatically updated, return OK
 *   3. count decrease → Exception, return FAIL
 *   4. count remains unchanged but hash does not match → tampered with, returns FAIL
 * ============================================================================ */

/* Define a separate integrity state with count for golden_cache */
typedef struct {
    uint8_t expected_hash[32];
    int has_baseline;
    uint32_t verify_count;
    uint32_t fail_count;
    uint32_t expected_count;  /* Expected number of entries */
} golden_cache_integrity_state_t;

static golden_cache_integrity_state_t golden_cache_integrity = {0};

static TEE_Result verify_golden_cache_integrity(uint32_t pt, TEE_Param params[4]) {
    uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
                                    TEE_PARAM_TYPE_VALUE_INPUT,
                                    TEE_PARAM_TYPE_VALUE_OUTPUT,
                                    TEE_PARAM_TYPE_NONE);
    if (pt != exp) return TEE_ERROR_BAD_PARAMETERS;

    uint8_t *ree_hash = params[0].memref.buffer;
    uint32_t ree_hash_len = params[0].memref.size;
    uint32_t ree_count = params[1].value.a;  /* REE side golden_cache effective number of entries */

    if (ree_hash_len != 32 || !ree_hash) return TEE_ERROR_BAD_PARAMETERS;

    golden_cache_integrity.verify_count++;

    /* Establishing a baseline for the first time */
    if (!golden_cache_integrity.has_baseline) {
        memcpy(golden_cache_integrity.expected_hash, ree_hash, 32);
        golden_cache_integrity.expected_count = ree_count;
        golden_cache_integrity.has_baseline = 1;
        params[2].value.a = CFI_INTEGRITY_FIRST;
        params[2].value.b = 0;
        DMSG("Golden cache baseline established (count=%u)", ree_count);
        return TEE_SUCCESS;
    }

    /* Increase in number of entries: Legitimate new cache entries, automatically update baseline */
    if (ree_count > golden_cache_integrity.expected_count) {
        memcpy(golden_cache_integrity.expected_hash, ree_hash, 32);
        golden_cache_integrity.expected_count = ree_count;
        params[2].value.a = CFI_INTEGRITY_OK;  /* Considered as a normal update, not the first time */
        params[2].value.b = golden_cache_integrity.fail_count;
        DMSG("Golden cache baseline updated (count %u -> %u)",
             golden_cache_integrity.expected_count, ree_count);
        return TEE_SUCCESS;
    }

    /* Decreased number of entries: abnormal, possibly deleted entries */
    if (ree_count < golden_cache_integrity.expected_count) {
        golden_cache_integrity.fail_count++;
        params[2].value.a = CFI_INTEGRITY_FAIL;
        params[2].value.b = golden_cache_integrity.fail_count;
        EMSG("GOLDEN CACHE COUNT DECREASED! %u -> %u, fails=%u",
             golden_cache_integrity.expected_count, ree_count,
             golden_cache_integrity.fail_count);
        if (global_status == CFI_STATUS_NORMAL) {
            global_status = CFI_STATUS_COMPROMISED;
            EMSG("*** SYSTEM DEGRADED: GOLDEN CACHE TAMPERED ***");
        }
        return TEE_SUCCESS;
    }

    /* Number of entries remains unchanged: strict comparison of hashes */
    if (memcmp(golden_cache_integrity.expected_hash, ree_hash, 32) == 0) {
        params[2].value.a = CFI_INTEGRITY_OK;
        params[2].value.b = golden_cache_integrity.fail_count;
        return TEE_SUCCESS;
    } else {
        golden_cache_integrity.fail_count++;
        params[2].value.a = CFI_INTEGRITY_FAIL;
        params[2].value.b = golden_cache_integrity.fail_count;
        EMSG("GOLDEN CACHE INTEGRITY FAIL! fails=%u", golden_cache_integrity.fail_count);
        if (global_status == CFI_STATUS_NORMAL) {
            global_status = CFI_STATUS_COMPROMISED;
            EMSG("*** SYSTEM DEGRADED: GOLDEN CACHE TAMPERED ***");
        }
        return TEE_SUCCESS;
    }
}

/* ========== Command distribution ========== */
TEE_Result TA_InvokeCommandEntryPoint(void *sess, uint32_t cmd,
                                       uint32_t pt, TEE_Param params[4]) {
    (void)sess;
    switch (cmd) {
        /* Backward-edge commands */
        case TA_CFI_CMD_REGISTER:       return thread_register(pt, params);
        case TA_CFI_CMD_UNREGISTER:     return thread_unregister(pt, params);
        case TA_CFI_CMD_PUSH:           return push_addr(pt, params);
        case TA_CFI_CMD_CHECK:          return pop_check(pt, params);
        case TA_CFI_CMD_GET_DEPTH:      return get_depth(pt, params);
        case TA_CFI_CMD_PUSH_BACKUP:    return push_backup(pt, params);
        case TA_CFI_CMD_GET_BACKUP:     return get_backup(pt, params);
        case TA_CFI_CMD_VERIFY_INTEGRITY: return verify_integrity(pt, params);

        /* Learning Mode Commands */
        case TA_CFI_CMD_SET_LEARNING_MODE: return set_learning_mode(pt, params);
        case TA_CFI_CMD_GET_LEARNING_MODE: return get_learning_mode(pt, params);
        case TA_CFI_CMD_CLEAR_GOLDEN:      return clear_golden(pt, params);

        /* Forward-edge commands */
        case TA_CFI_CMD_REGISTER_TARGET: return register_target(pt, params);
        case TA_CFI_CMD_VERIFY_TARGET:   return verify_target(pt, params);
        case TA_CFI_CMD_REGISTER_TARGET_BATCH: return register_target_batch(pt, params);
        case TA_CFI_CMD_QUERY_TARGET_LABEL: return query_target_label(pt, params);

        /* GOT/PLT commands */
        case TA_CFI_CMD_REGISTER_GOT:    return register_got(pt, params);
        case TA_CFI_CMD_REGISTER_GOT_BATCH: return register_got_batch(pt, params);
        case TA_CFI_CMD_VERIFY_GOT_HASH: return verify_got_hash(pt, params);
        case TA_CFI_CMD_VERIFY_GOT_INTEGRITY: return verify_got_integrity(pt, params);
	
	/* GOLDEN */
	case TA_CFI_CMD_VERIFY_GOLDEN_CACHE_INTEGRITY: return verify_golden_cache_integrity(pt, params);
        case TA_CFI_CMD_VERIFY_GOLDEN_TABLE_INTERNAL:  return verify_golden_table_internal(pt, params);
        default: return TEE_ERROR_BAD_PARAMETERS;
    }
}

