#ifndef OPENPLC_GOT_H
#define OPENPLC_GOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GOT Item */
typedef struct {
    uint64_t plt_addr;       /* PLT item addr */
    uint64_t got_addr;       /* GOT item addr */
    uint64_t got_target;     /* Current GOT target addr */
} got_entry_t;

/* Analyze the current process GOT/PLT */
int got_parse_self(got_entry_t **entries, int *count);
void got_free_entries(got_entry_t *entries);

/* read GOT current value */
uint64_t got_read_target(uint64_t got_addr);

/* calcu GOT hash */
int got_compute_hash(const got_entry_t *entries, int count, uint8_t *hash_out);

#ifdef __cplusplus
}
#endif

#endif
