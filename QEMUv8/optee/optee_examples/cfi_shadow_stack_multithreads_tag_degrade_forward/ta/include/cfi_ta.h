#ifndef CFI_TA_H
#define CFI_TA_H

#define TA_CFI_UUID     { 0x12345678, 0x1234, 0x1234, { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 } }

/* ========== Backward-edge command ========== */
#define TA_CFI_CMD_REGISTER         0
#define TA_CFI_CMD_UNREGISTER       1
#define TA_CFI_CMD_PUSH             2
#define TA_CFI_CMD_CHECK            3
#define TA_CFI_CMD_GET_DEPTH        4
#define TA_CFI_CMD_PUSH_BACKUP      5
#define TA_CFI_CMD_GET_BACKUP       6
#define TA_CFI_CMD_VERIFY_INTEGRITY 7

/* ========== Forward-edge command ========== */
#define TA_CFI_CMD_REGISTER_TARGET       8   /* Register a single legitimate indirect call target */
#define TA_CFI_CMD_VERIFY_TARGET         9   /* Verify indirect call targets */
#define TA_CFI_CMD_REGISTER_TARGET_BATCH 14  /* Batch registration of forward edge targets */
#define TA_CFI_CMD_QUERY_TARGET_LABEL    15  /* Query target tags (debugging) */

/* ========== GOT/PLT Protection Command ========== */
#define TA_CFI_CMD_REGISTER_GOT          10  /* Register a single GOT entry */
#define TA_CFI_CMD_REGISTER_GOT_BATCH    11  /* Batch registration of GOT entries */
#define TA_CFI_CMD_VERIFY_GOT_HASH       12  /* Verify GOT hash */
#define TA_CFI_CMD_VERIFY_GOT_INTEGRITY  13  /* GOT integrity verification */

/* ========== Learning mode command (Learning+Freeze) ========== */
#define TA_CFI_CMD_SET_LEARNING_MODE 16  /* Setting learning mode: params[0].value.a=1/0 */
#define TA_CFI_CMD_GET_LEARNING_MODE 17  /* Getting learning mode status */
#define TA_CFI_CMD_CLEAR_GOLDEN      18  /* Clear the golden table and relearn */

/* ========== Forward-edge: Target type classification ========== */
#define CFI_TARGET_TYPE_GENERIC     0
#define CFI_TARGET_TYPE_PLC_PROG    1
#define CFI_TARGET_TYPE_PLC_INIT    2
#define CFI_TARGET_TYPE_IEC_FUNC    3
#define CFI_TARGET_TYPE_THREAD      4
#define CFI_TARGET_TYPE_CALLBACK    5
#define CFI_TARGET_TYPE_VIRTUAL     6
#define CFI_TARGET_TYPE_SERVER      7
#define CFI_TARGET_TYPE_IO          8
#define CFI_TARGET_TYPE_UTIL        9
#define CFI_TARGET_TYPE_EXTERNAL   10
#define CFI_TARGET_TYPE_MAX        11

/* ========== DEGRADED/SURVIVAL special command ========== */
#define TA_CFI_CMD_VERIFY_GOLDEN_CACHE_INTEGRITY 19  /* verify REE golden_cache integrity */
#define TA_CFI_CMD_VERIFY_GOLDEN_TABLE_INTERNAL  20  /* TEE-internel golden_table self-check */

/* ========== Configure constants ========== */
#define MAX_BACKUP_ENTRIES    4096
#define MAX_THREADS           64
#define MAX_TARGETS           8192
#define MAX_GOT_ENTRIES       512
#define MAX_CALL_SITES_PER_TAG 16   /* The maximum number of call points allowed for each tag */

/* Integrity verification results */
#define CFI_INTEGRITY_OK        0
#define CFI_INTEGRITY_FAIL      1
#define CFI_INTEGRITY_FIRST     2

/* Learning mode status */
#define CFI_LEARNING_ACTIVE     0   /* Learning stage: Record all new (tags, addr) */
#define CFI_LEARNING_FROZEN     1   /* Freeze phase: Reject new (tag, addr) */

/* Security Status */
#define CFI_STATUS_NORMAL       0
#define CFI_STATUS_COMPROMISED  1

/* push_backup return code */
#define CFI_BACKUP_OK           0   /* Record success or already exists */
#define CFI_BACKUP_NEW_FROZEN   1   /* New call point encountered during freezing phase */
#define CFI_BACKUP_PROBE        2   /* Detection attack: Too many addr for the same tag */

#endif

