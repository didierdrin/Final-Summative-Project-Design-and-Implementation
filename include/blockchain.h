/* ============================================================
 *  File: include/blockchain.h
 * ============================================================ */
#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#include <time.h>
#include "crypto_utils.h"

#define HASH_HEX_LEN   65
#define BLOCK_SIG_LEN  72
#define MAX_PENDING    256
#define MAX_DIFFICULTY 6

/* 64 zeros: previous_hash of the genesis block. */
#define ZERO_HASH \
    "0000000000000000000000000000000000000000000000000000000000000000"

/* ------------------------------------------------------------
 *  One block = one lending record (or the genesis block).
 *  Header fields are set by sealing (mining). Record fields are
 *  signed once, before proof-of-work begins.
 * ------------------------------------------------------------ */
typedef struct {
    /* --- block header --- */
    int           index;                         /* 0 = genesis             */
    time_t        timestamp;                     /* strictly increasing     */
    int           record_count;                  /* always 1 in this build  */
    char          previous_hash[HASH_HEX_LEN];   /* hash of block i-1       */
    char          merkle_root[HASH_HEX_LEN];     /* Merkle root of record   */
    char          hash[HASH_HEX_LEN];            /* SHA-256 of the header   */
    char          sealed_by[24];                 /* miner / sealing node    */
    int           difficulty;                    /* leading zeros required  */
    unsigned long nonce;                         /* proof-of-work nonce     */

    /* --- lending record --- */
    char          book_id[20];                   /* FK -> book registry     */
    char          book_title[80];                /* snapshot at txn time    */
    char          copy_id[24];                   /* physical copy           */
    char          member_id[20];                 /* FK -> member registry   */
    char          member_name[50];               /* snapshot at txn time    */
    char          action[12];                    /* BORROWED, RETURNED, ... */
    char          due_date[11];                  /* YYYY-MM-DD or empty     */
    int           token_reward;                  /* 10 on time, 5 late      */
    int           fine_amount;                   /* fine on this record     */
    char          transaction_id[HASH_HEX_LEN];  /* SHA-256 of txn seed     */
    unsigned char signature[BLOCK_SIG_LEN];      /* ECDSA (DER) over record */
} Block;

typedef struct {
    Block *items;
    int    count;
    int    capacity;
} PendingPool;

typedef struct {
    Block *blocks;
    int    count;
    int    capacity;
} Chain;

/* --- chain lifecycle --------------------------------------- */
void chain_init_struct(Chain *c);
int  chain_load(Chain *c, const char *path);   /* 0 ok, 1 no file, -1 bad */
int  chain_save(const Chain *c, const char *path);
int  chain_genesis(Chain *c);
void chain_free(Chain *c);

/* --- sealing (mining) -------------------------------------- */
/* Fills the header fields of `rec`, signs the record once, builds the
 * Merkle root, runs proof-of-work on the nonce until the hash has
 * `difficulty` leading zeros, then appends the block to the chain. */
int  chain_seal(Chain *c, Block *rec, const char *sealer,
                int difficulty, int *attempts);

/* Sorts the pool, seals every item in order, removes sealed items.
 * Returns the number of blocks sealed. */
int  chain_mine_pending(Chain *c, PendingPool *p, const char *sealer,
                        int difficulty, int *total_attempts);

/* --- pending pool ------------------------------------------ */
void pending_pool_init(PendingPool *p);
void pending_pool_free(PendingPool *p);
int  pending_pool_add(PendingPool *p, const Block *rec);
void pending_pool_sort(PendingPool *p);
void pending_pool_print(const PendingPool *p);

/* --- queries and checks ------------------------------------ */
int  chain_validate(const Chain *c, int *first_bad_index); /* 1 = valid */
const Block *chain_last_for_copy(const Chain *c, const char *copy_id);
const Block *chain_last_for_book(const Chain *c, const char *book_id);
void chain_print(const Chain *c);
void chain_tamper_demo(const Chain *c);

#endif /* BLOCKCHAIN_H */