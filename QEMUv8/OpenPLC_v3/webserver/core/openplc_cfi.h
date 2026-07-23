#ifndef OPENPLC_CFI_H
#define OPENPLC_CFI_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize/Close */
int  cfi_init(void);
void cfi_fini(void);

/* Core operation (new interface with tags) */
void cfi_push_addr_tag(uint64_t addr, uint32_t tag);
int  cfi_check_addr_tag(uint64_t addr, uint32_t tag);
int  cfi_get_depth(void);

/* Compatible with old interfaces (unlabeled, using default tag=0) */
void cfi_push_addr(uint64_t addr);
int  cfi_check_addr(uint64_t addr);

/* ========== Forward edge interface (runtime learning mode)========== */
int  cfi_verify_forward_target(uint64_t addr, uint32_t label);
void cfi_register_target(uint64_t addr, uint32_t label);

/* ========== GOT/PLT Protect interface ========== */
int  cfi_got_init(void);
int  cfi_got_verify(void);
void cfi_got_fini(void);

/* ========== integrity verify ========== */
int cfi_verify_integrity(void);
int cfi_verify_integrity_safe(void);  /* Timer Security Package Version */

/* ========== Status Inquiry ========== */
int cfi_is_degraded(void);

/* ========== Learning Mode Management (pproach 3: Learning + Freeze) ========== */
void cfi_freeze(void);
int  cfi_is_learning(void);
void cfi_clear_golden(void);

/* Default learning phase duration (seconds), 0=infinite (manual freeze required) */
#define CFI_LEARNING_DURATION  0

/* ========== PAC-BTI fault recovery ========== */

/* Strict mode recovery address retrieval (internal use of CFI_EPILOGUE_TAG macro) */
uint64_t cfi_get_strict_recovery_addr(void);

/**
 * cfi_epilogue_pac - PAC-BTI Safety Epilogue (Not Abort, Dependent on Signal Recovery)
 *
 * The difference between cfiucheck_dedr_tag and cfi:
 * - Pop up the expected return address from the shadow stack
 * - Save to __thread variable for signal processor use
 * - Do not abort when there is a mismatch, let the ret instruction execute
 * - If LR is tampered with, PAC-BTI hardware will trigger a fault when ret
 * - The signal processor captures the fault and restores the PC with the saved expected address
 *
 * @param addr: __builtin_return_address(0)
 * @param tag: Function Tags
 * @return: 1=Shadow stack matching, 0=Mismatch (but still saved recovery address)
 */
int cfi_epilogue_pac(uint64_t addr, uint32_t tag);

/* ========== Tag generation macro ========== */
#define CFI_TAG_FROM_FUNC() ((uint32_t)((uintptr_t)__func__))

/* Call point tag generation: based on call location hash */
#define CFI_LABEL_FROM_FUNC(func) \
    ((uint32_t)(((__LINE__ * 0x9E3779B9U) ^ (uintptr_t)__func__[0]) | 0x80000000U))

/* ========== Pile insertion macro (with label) ========== */

/**
 * CFI_PROLOGUE_TAG - Function entry: Save return address to shadow stack
 */
#define CFI_PROLOGUE_TAG(tag) do { \
    void *_ret = __builtin_return_address(0); \
    cfi_push_addr_tag((uint64_t)_ret, (tag)); \
} while(0)

/**
 * CFI_EPILOGUE_TAG - Function Exit: Strict Validation (Recovering LR from Shadow Stack in case of Mismatch)
 *
 * After detecting that the return address has been tampered with, 
 * restore LR using the correct address saved in the shadow stack,
 * The program continues to execute instead of abort. 
 * Simultaneously trigger degradation to increase the subsequent verification frequency.
 */
#define CFI_EPILOGUE_TAG(tag) do { \
    void *_ret = __builtin_return_address(0); \
    if (!cfi_check_addr_tag((uint64_t)_ret, (tag))) { \
        uint64_t _rec = cfi_get_strict_recovery_addr(); \
        if (_rec) { \
            fprintf(stderr, "[CFI] VIOLATION at %p in %s! Restoring LR to 0x%lx\n", \
                    _ret, __func__, (unsigned long)_rec); \
            __asm__ volatile("mov x30, %0" :: "r"(_rec) : "x30"); \
        } else { \
            fprintf(stderr, "[CFI] VIOLATION detected at %p in %s! Aborting.\n", \
                    _ret, __func__); \
            abort(); \
        } \
    } \
} while(0)

/**
 * CFI_EPILOGUE_SOFT_TAG - Function exit: Soft checksum (only records without abort)
 */
#define CFI_EPILOGUE_SOFT_TAG(tag) do { \
    void *_ret = __builtin_return_address(0); \
    if (!cfi_check_addr_tag((uint64_t)_ret, (tag))) { \
        fprintf(stderr, "[CFI] VIOLATION detected at %p in %s!\n", \
                _ret, __func__); \
    } \
} while(0)

/**
 * CFI_EPILOGUE_TAG_PAC - PAC-BTI safe epilogue
 *
 * Specially designed for PAC-BTI hardware protection:
 * 1. Pop up the expected return address from the shadow stack
 * 2. Save to thread local variable (for SIGSEGV/SIGILL processor recovery)
 * 3. Do not abort - let the ret instruction execute
 * 4. If LR is tampered with, PAC-BTI triggers a fault during ret
 * 5. The signal processor uses the saved address to restore the PC, and the program continues to execute
 *
 * Prerequisite for use: PAC-BTI must be enabled (PAC-INABLE=1)
 */
#define CFI_EPILOGUE_TAG_PAC(tag) do { \
    void *_ret = __builtin_return_address(0); \
    cfi_epilogue_pac((uint64_t)_ret, (tag)); \
} while(0)

/* ============================================================================
 * Forward edge insertion macro (runtime learning mode)
 * ============================================================================
 * Design and backward symmetry:
 * - Target: Indirectly jump to the target address (thread function, callback function, vtable method)
 * - Label: Unique identifier of the calling context
 * REE cache fast path → TEE learning/validation → downgrade to full TEE
 * ============================================================================ */

/**
 * CFI_INDIRECT_CALL - Verify function pointer and return (can be used for assignment/parameter passing)
 * @param fp: Function pointer variable
 * @param label: Call context tags
 * @return: Validation passed fp, or NULL (in case of validation failure)
 */
#define CFI_INDIRECT_CALL(fp, label) \
    (cfi_verify_forward_target((uint64_t)(uintptr_t)(fp), (label)) ? (fp) : NULL)

/**
 * CFI_INDIRECT_CALL_VOID - Validate function pointer (void return, abort upon failure)
 * @param fp: Function pointer variable
 * @param label: Call context tags
 */
#define CFI_INDIRECT_CALL_VOID(fp, label) do { \
    if (!cfi_verify_forward_target((uint64_t)(uintptr_t)(fp), (label))) { \
        fprintf(stderr, "[CFI] FORWARD VIOLATION at %s:%d! target=%p label=0x%x\n", \
                __FILE__, __LINE__, (fp), (unsigned)(label)); \
        abort(); \
    } \
} while(0)

/**
 * CFI_PTHREAD_CREATE - Secure pthread_create (forward edge protection)
 * Register start_routine to TEE target table before creating thread
 * @param thread: pthread_t*
 * @param attr: pthread_attr_t*
 * @param start_routine: Thread entry function
 * @param arg: Thread parameters
 */
#define CFI_PTHREAD_CREATE(thread, attr, start_routine, arg) do { \
    uint32_t _label = CFI_LABEL_FROM_FUNC(start_routine); \
    cfi_register_target((uint64_t)(uintptr_t)(start_routine), _label); \
    pthread_create((thread), (attr), (start_routine), (arg)); \
} while(0)

/**
 * CFI_CALLBACK_ASSIGN - Validation during callback function pointer assignment
 * Used for function pointer variable assignment scenarios (such as callback=function_2;)
 * @param var: Function pointer variable (left value)
 * @param func: Function Name (right Value)
 * @param label: Call context tags
 */
#define CFI_CALLBACK_ASSIGN(var, func, label) do { \
    cfi_register_target((uint64_t)(uintptr_t)(func), (label)); \
    (var) = (func); \
} while(0)

/**
 * CFI_VCALL - C++ Virtual function call validation
 * Verify the vtable integrity of this pointer at the entrance of the virtual function body
 * Detecting illegal virtual function jumps after vtable has been tampered with
 * @param obj: this Pointer (Object Pointer)
 * @param method_name: Method name string (used for tag generation)
 * @note: Used in conjunction with CFI-PROLOGUE-TAG and placed in front of it
 */
#ifdef __cplusplus
#include <typeinfo>
#define CFI_VCALL(obj, method_name) do { \
    uint32_t _vlabel = CFI_LABEL_FROM_FUNC(method_name); \
    if (!cfi_verify_forward_target((uint64_t)(uintptr_t)(obj), _vlabel)) { \
        fprintf(stderr, "[CFI] VCALL VIOLATION at %s:%d! obj=%p method=%s\n", \
                __FILE__, __LINE__, (obj), #method_name); \
    } \
} while(0)
#else
#define CFI_VCALL(obj, method_name) ((void)0)
#endif

/**
 * CFI_SIGACTION - Registration of secure signal processing functions
 * Verify the handler address before sigaction
 * @param sig: Signal number
 * @param act: struct sigaction*
 * @param oldact: struct sigaction*
 */
#define CFI_SIGACTION(sig, act, oldact) do { \
    if (act && (act)->sa_handler != SIG_IGN && (act)->sa_handler != SIG_DFL) { \
        uint32_t _slabel = CFI_TAG_FROM_FUNC(); \
        cfi_register_target((uint64_t)(uintptr_t)(act)->sa_sigaction, _slabel); \
    } \
    sigaction((sig), (act), (oldact)); \
} while(0)

/* ========== Old macro (compatible) ========== */
#define CFI_PROLOGUE() do { \
    void *_ret = __builtin_return_address(0); \
    cfi_push_addr((uint64_t)_ret); \
} while(0)

#define CFI_EPILOGUE() do { \
    void *_ret = __builtin_return_address(0); \
    if (!cfi_check_addr((uint64_t)_ret)) { \
        fprintf(stderr, "[CFI] VIOLATION detected at %p in %s! Aborting.\n", \
                _ret, __func__); \
        abort(); \
    } \
} while(0)

#define CFI_EPILOGUE_SOFT() do { \
    void *_ret = __builtin_return_address(0); \
    if (!cfi_check_addr((uint64_t)_ret)) { \
        fprintf(stderr, "[CFI] VIOLATION detected at %p in %s!\n", \
                _ret, __func__); \
    } \
} while(0)


/* ============================================================================
 * Timing Measurement Control
 * ============================================================================
 * CFI time measurement is enabled by default, and can be disabled by passing - DCFI_TIMING=0 during compilation
 * Measurement based on clock_getime (CLOCKMYOTONIC), in nanoseconds
 * ============================================================================ */
#ifndef CFI_TIMING
#define CFI_TIMING 1
#endif

#if CFI_TIMING
void cfi_print_timing_stats(void);
#endif /* CFI_TIMING */

/* ============================================================================
 * Runtime Stage - Three-Layer Architecture
 * Layer 1: Monitoring & Forensics
 * Layer 2: Adaptive Response  
 * Layer 3: Operator Actions
 * ============================================================================ */

/* ---------- Layer 1: Runtime State Machine & Event Logging ---------- */

typedef enum {
    CFI_RUNTIME_STATUS_NORMAL = 0,
    CFI_RUNTIME_STATUS_SUSPECT,
    CFI_RUNTIME_STATUS_DEGRADED,
    CFI_RUNTIME_STATUS_EMERGENCY
} cfi_runtime_status_t;

typedef enum {
    CFI_EVENT_FREEZE_BREACH = 0,
    CFI_EVENT_INTEGRITY_FAIL_SS,
    CFI_EVENT_INTEGRITY_FAIL_GOT,
    CFI_EVENT_DEGRADE_TRIGGERED,
    CFI_EVENT_PAC_FAULT_RECOVERED,
    CFI_EVENT_PAC_FAULT_FATAL,
    CFI_EVENT_FORWARD_VIOLATION,
    CFI_EVENT_MANUAL_CHECK,
    CFI_EVENT_MODE_CHANGE,
    CFI_EVENT_INTEGRITY_FAIL_GOLDEN_CACHE,  /* NEW: DEGRADED mode golden_cache fail */
    CFI_EVENT_AUTO_DEGRADE                  /* NEW: auto degrade */
} cfi_event_type_t;

typedef struct {
    uint64_t timestamp_ns;
    cfi_event_type_t type;
    uint32_t tag;
    uint64_t expected_addr;
    uint64_t actual_addr;
    int tid;
    uint32_t cycle_count;
    char context[64];
} cfi_event_record_t;

typedef struct {
    cfi_runtime_status_t status;
    uint32_t cycle_count;
    uint32_t integrity_check_count;
    uint32_t integrity_pass_count;
    uint32_t integrity_fail_count;
    uint32_t degrade_count;
    uint32_t pac_fault_count;
    uint32_t pac_recover_count;
    uint32_t freeze_breach_count;
    uint32_t forward_violation_count;
    int current_integrity_period;
    uint64_t last_integrity_ns;
    uint32_t event_count;
    cfi_event_record_t events[64]; /* ring buffer, 64 entries */
    /* NEW: Automatic degrade attack count */
    uint32_t attack_count;        /* Continuous attack count in current mode */
    uint32_t attack_threshold;    /* The attack threshold of the current mode */
    uint32_t auto_degrade_count;  /* Automatic degrade frequency */
} cfi_runtime_state_t;

const char* cfi_event_type_name(cfi_event_type_t type);
const char* cfi_status_name(cfi_runtime_status_t s);
const cfi_runtime_state_t* cfi_get_runtime_state(void);
void cfi_log_event(cfi_event_type_t type, uint32_t tag,
                   uint64_t expected, uint64_t actual,
                   const char* context);
void cfi_print_event_log(void);
void cfi_print_status_banner(void);

/* ---------- Layer 2: Adaptive Response ---------- */

typedef enum {
    CFI_MODE_NORMAL = 0,
    CFI_MODE_SUSPECT,
    CFI_MODE_DEGRADED,
    CFI_MODE_SURVIVAL
} cfi_protection_mode_t;

const char* cfi_mode_name(cfi_protection_mode_t m);
void cfi_set_protection_mode(cfi_protection_mode_t mode);
cfi_protection_mode_t cfi_get_protection_mode(void);
void cfi_adapt_integrity_period(cfi_runtime_status_t new_status);
int cfi_get_current_integrity_period(void);

/* ---------- Layer 3: Operator Command Interface ---------- */

typedef enum {
    CMD_GET_STATUS = 0,
    CMD_GET_EVENTS,
    CMD_MANUAL_INTEGRITY,
    CMD_SET_MODE_NORMAL,
    CMD_SET_MODE_SUSPECT,
    CMD_SET_MODE_DEGRADED,
    CMD_SET_MODE_SURVIVAL,
    CMD_SET_INTEGRITY_PERIOD,
    CMD_REQUEST_SAFETY_STOP,
    CMD_CLEAR_DEGRADE
} cfi_operator_cmd_t;

typedef struct {
    cfi_operator_cmd_t cmd;
    uint32_t arg;
} cfi_operator_request_t;

typedef struct {
    int status;
    char msg[256];
} cfi_operator_response_t;

void cfi_start_operator_server(void);
void cfi_stop_operator_server(void);
int cfi_operator_handle_cmd(cfi_operator_request_t* req, cfi_operator_response_t* resp);
int cfi_request_safety_stop(void);
extern volatile int cfi_safety_stop_requested;

#ifdef __cplusplus
}
#endif

#endif
