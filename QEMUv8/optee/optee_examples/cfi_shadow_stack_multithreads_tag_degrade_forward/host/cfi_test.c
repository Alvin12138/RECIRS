#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <tee_client_api.h>
#include <cfi_ta.h>

// This is the initial version and is not applicable to the shadow stack of (addr, tag) for the current ta

static TEEC_Context ctx;
static TEEC_Session sess;

int init_ta(void) {
    TEEC_Result res;
    TEEC_UUID uuid = TA_CFI_UUID;
    uint32_t err;

    res = TEEC_InitializeContext(NULL, &ctx);
    if (res != TEEC_SUCCESS) {
        printf("TEEC_InitializeContext failed: 0x%x\n", res);
        return -1;
    }

    res = TEEC_OpenSession(&ctx, &sess, &uuid, TEEC_LOGIN_PUBLIC, NULL, NULL, &err);
    if (res != TEEC_SUCCESS) {
        printf("TEEC_OpenSession failed: 0x%x origin 0x%x\n", res, err);
        TEEC_FinalizeContext(&ctx);
        return -1;
    }
    return 0;
}

void close_ta(void) {
    TEEC_CloseSession(&sess);
    TEEC_FinalizeContext(&ctx);
}

static void cfi_push(uint64_t addr) {
    TEEC_Operation op = {0};
    uint32_t err;
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE);
    op.params[0].value.a = (uint32_t)(addr >> 32);
    op.params[0].value.b = (uint32_t)addr;
    TEEC_InvokeCommand(&sess, TA_CFI_CMD_PUSH, &op, &err);
}

static int cfi_check(uint64_t addr) {
    TEEC_Operation op = {0};
    uint32_t err;
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_OUTPUT, TEEC_NONE, TEEC_NONE);
    op.params[0].value.a = (uint32_t)(addr >> 32);
    op.params[0].value.b = (uint32_t)addr;
    TEEC_Result res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_CHECK, &op, &err);
    if (res != TEEC_SUCCESS) return 0;
    uint64_t match = ((uint64_t)op.params[1].value.a << 32) | op.params[1].value.b;
    return (int)match;
}

#define CFI_PROLOGUE() do { \
    void *_ret = __builtin_return_address(0); \
    cfi_push((uint64_t)_ret); \
} while(0)

#define CFI_EPILOGUE() do { \
    void *_ret = __builtin_return_address(0); \
    if (!cfi_check((uint64_t)_ret)) { \
        printf("CFI VIOLATION at %p! Aborting.\n", _ret); \
        abort(); \
    } \
} while(0)

int __attribute__((noinline)) func_c(int x) {
    CFI_PROLOGUE();
    int r = x * 2;
    CFI_EPILOGUE();
    return r;
}

int __attribute__((noinline)) func_b(int x) {
    CFI_PROLOGUE();
    int r = func_c(x) + 10;
    CFI_EPILOGUE();
    return r;
}

int __attribute__((noinline)) func_a(int x) {
    CFI_PROLOGUE();
    int r = func_b(x) + 1;
    CFI_EPILOGUE();
    return r;
}

int __attribute__((noinline)) base_c(int x) { return x * 2; }
int __attribute__((noinline)) base_b(int x) { return base_c(x) + 10; }
int __attribute__((noinline)) base_a(int x) { return base_b(x) + 1; }

int main(void) {
    if (init_ta() != 0) return 1;

    printf("=== CFI Shadow Stack Test ===\n");

    // Correctness test
    printf("func_a(5) = %d\n", func_a(5));

    // Performance: CFI
    struct timespec t0, t1;
    int n = 1000;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n; i++) func_a(i);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double t_cfi = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    // Performance: baseline
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n; i++) base_a(i);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double t_base = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    printf("\n%d iterations:\n", n);
    printf("Baseline: %.3f ms (%.3f us/call)\n", t_base, t_base * 1000 / n);
    printf("CFI:      %.3f ms (%.3f us/call)\n", t_cfi, t_cfi * 1000 / n);
    printf("Overhead: %.1fx\n", t_cfi / t_base);

    close_ta();
    return 0;
}
