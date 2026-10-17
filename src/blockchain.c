/* ============================================================
 *  File: src/blockchain.c
 *  Records, Merkle root, proof-of-work sealing, validation,
 *  pending pool, and binary persistence.
 * ============================================================ */
#include "blockchain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY   16
#define CHAIN_MAGIC        "LBCT2"
#define CHAIN_MAGIC_LEN    5
#define MAX_CHAIN_BLOCKS   100000
#define MSG_MAX            1024
#define HASH_INPUT_MAX     2048

/* Proof-of-work target: hash must begin with d '0' characters. */
static int meets_difficulty(const char *h, int d)
{
    if (d < 0 || d > 64) return 0;
    for (int i = 0; i < d; i++)
        if (h[i] != '0') return 0;
    return 1;
}

static int chain_reserve(Chain *c, int need)
{
    int    newcap;
    Block *tmp;

    if (need <= c->capacity) return 0;
    newcap = c->capacity ? c->capacity : INITIAL_CAPACITY;
    while (newcap < need) newcap *= 2;

    tmp = realloc(c->blocks, sizeof(Block) * (size_t)newcap);
    if (!tmp) { fprintf(stderr, "ERROR: out of memory\n"); return -1; }
    c->blocks   = tmp;
    c->capacity = newcap;
    return 0;
}

/* ============================================================
 *  Record layer: canonical message, ECDSA sign and verify
 * ============================================================ */

/* Fields covered by the record signature. The nonce and block hash
 * are excluded: the record is signed once, and proof-of-work only
 * changes the nonce. */
static void record_message(const Block *b, char *out, size_t outsz)
{
    snprintf(out, outsz,
             "%ld|%s|%s|%s|%s|%s|%s|%s|%d|%d|%s",
             (long)b->timestamp,
             b->book_id, b->book_title, b->copy_id,
             b->member_id, b->member_name,
             b->action, b->due_date,
             b->token_reward, b->fine_amount,
             b->transaction_id);
}

static int record_sign(Block *b)
{
    char msg[MSG_MAX];
    unsigned char sig[ECDSA_SIG_MAX];
    unsigned int  len = 0;

    record_message(b, msg, sizeof msg);
    if (ecdsa_sign((const unsigned char *)msg, strlen(msg), sig, &len) != 0)
        return -1;

    memset(b->signature, 0, sizeof b->signature);
    memcpy(b->signature, sig, len);
    return 0;
}

static int record_verify(const Block *b)
{
    char msg[MSG_MAX];
    record_message(b, msg, sizeof msg);
    return ecdsa_verify((const unsigned char *)msg, strlen(msg), b->signature);
}

/* ============================================================
 *  Merkle tree
 * ============================================================ */

/* Bottom-up pairwise SHA-256: parent = SHA-256(left || right).
 * An odd node is paired with itself. The level array is overwritten. */
static void merkle_root(char (*level)[HASH_HEX_LEN], int n, char out[HASH_HEX_LEN])
{
    char joined[2 * HASH_HEX_LEN];

    if (n <= 0) { sha256_hex("", 0, out); return; }

    while (n > 1) {
        int next = 0;
        for (int i = 0; i < n; i += 2) {
            const char *right = (i + 1 < n) ? level[i + 1] : level[i];
            snprintf(joined, sizeof joined, "%s%s", level[i], right);
            sha256_hex(joined, strlen(joined), level[next]);
            next++;
        }
        n = next;
    }
    memcpy(out, level[0], HASH_HEX_LEN);
}

/* Each block carries one record, so its Merkle root is the record's
 * leaf hash. The function still builds a general tree, so batching
 * several records per block only needs more leaves. */
static void block_compute_merkle(const Block *b, char out[HASH_HEX_LEN])
{
    char leaves[1][HASH_HEX_LEN];
    char msg[MSG_MAX];

    record_message(b, msg, sizeof msg);
    sha256_hex(msg, strlen(msg), leaves[0]);
    merkle_root(leaves, 1, out);
}

/* ============================================================
 *  Block hashing
 * ============================================================ */
static void block_compute_hash(const Block *b, char out[HASH_HEX_LEN])
{
    char msg[MSG_MAX];
    char sig_hex[2 * BLOCK_SIG_LEN + 1];
    char buf[HASH_INPUT_MAX];
    size_t slen = der_sig_len(b->signature);

    /* Clamp: a malformed stored signature must not overflow sig_hex. */
    if (slen > BLOCK_SIG_LEN) slen = BLOCK_SIG_LEN;

    record_message(b, msg, sizeof msg);
    to_hex(b->signature, slen, sig_hex);

    snprintf(buf, sizeof buf, "%d|%s|%s|%d|%s|%lu|%s|%s",
             b->index, b->previous_hash, b->merkle_root,
             b->difficulty, b->sealed_by, b->nonce, sig_hex, msg);
    sha256_hex(buf, strlen(buf), out);
}

/* ============================================================
 *  Chain lifecycle
 * ============================================================ */
void chain_init_struct(Chain *c)
{
    c->blocks   = NULL;
    c->count    = 0;
    c->capacity = 0;
}

void chain_free(Chain *c)
{
    free(c->blocks);
    c->blocks   = NULL;
    c->count    = 0;
    c->capacity = 0;
}

int chain_genesis(Chain *c)
{
    Block g;

    if (chain_reserve(c, 1) != 0) return -1;

    memset(&g, 0, sizeof g);
    g.index        = 0;
    g.timestamp    = time(NULL);
    g.record_count = 0;
    g.difficulty   = 0;
    g.nonce        = 0;
    snprintf(g.previous_hash, sizeof g.previous_hash, "%s", ZERO_HASH);
    snprintf(g.sealed_by,     sizeof g.sealed_by,     "genesis");
    snprintf(g.book_id,       sizeof g.book_id,       "GENESIS");
    snprintf(g.book_title,    sizeof g.book_title,    "Genesis Block");
    snprintf(g.copy_id,       sizeof g.copy_id,       "NONE");
    snprintf(g.member_id,     sizeof g.member_id,     "SYSTEM");
    snprintf(g.member_name,   sizeof g.member_name,   "Library System");
    snprintf(g.action,        sizeof g.action,        "GENESIS");
    snprintf(g.transaction_id, sizeof g.transaction_id, "GENESIS");

    if (record_sign(&g) != 0) return -1;
    block_compute_merkle(&g, g.merkle_root);
    block_compute_hash(&g, g.hash);

    c->blocks[c->count++] = g;
    return 0;
}

/* ============================================================
 *  Sealing: sign record, Merkle root, proof-of-work, append
 * ============================================================ */
int chain_seal(Chain *c, Block *rec, const char *sealer,
               int difficulty, int *attempts)
{
    const Block *prev;
    long tries = 0;

    if (!c || !rec || c->count == 0) return -1;
    if (difficulty < 0 || difficulty > MAX_DIFFICULTY) return -1;
    if (chain_reserve(c, c->count + 1) != 0) return -1;

    prev = &c->blocks[c->count - 1];

    rec->index        = c->count;
    rec->timestamp    = time(NULL);
    if (rec->timestamp <= prev->timestamp)     /* keep strictly increasing */
        rec->timestamp = prev->timestamp + 1;
    rec->record_count = 1;
    snprintf(rec->previous_hash, sizeof rec->previous_hash, "%s", prev->hash);
    snprintf(rec->sealed_by, sizeof rec->sealed_by, "%s", sealer ? sealer : "unknown");
    rec->difficulty   = difficulty;

    /* Sign the record once. The timestamp must already be final. */
    if (record_sign(rec) != 0) return -1;
    block_compute_merkle(rec, rec->merkle_root);

    /* Proof-of-work: vary only the nonce until the hash meets the target. */
    rec->nonce = 0;
    for (;;) {
        block_compute_hash(rec, rec->hash);
        tries++;
        if (meets_difficulty(rec->hash, difficulty)) break;
        rec->nonce++;
    }

    if (attempts) *attempts = (int)tries;
    c->blocks[c->count++] = *rec;
    return 0;
}

/* ============================================================
 *  Pending pool
 * ============================================================ */
void pending_pool_init(PendingPool *p)
{
    p->items    = NULL;
    p->count    = 0;
    p->capacity = 0;
}

void pending_pool_free(PendingPool *p)
{
    free(p->items);
    p->items    = NULL;
    p->count    = 0;
    p->capacity = 0;
}

int pending_pool_add(PendingPool *p, const Block *rec)
{
    Block *tmp;
    int    newcap;

    if (p->count >= MAX_PENDING) return -1;
    if (p->count >= p->capacity) {
        newcap = p->capacity ? p->capacity * 2 : 8;
        if (newcap > MAX_PENDING) newcap = MAX_PENDING;
        tmp = realloc(p->items, sizeof(Block) * (size_t)newcap);
        if (!tmp) return -1;
        p->items    = tmp;
        p->capacity = newcap;
    }
    p->items[p->count++] = *rec;
    return 0;
}

/* Priority 0: overdue-related (OVERDUE records, late returns with fines).
 * Priority 1: everything else. */
static int pending_priority(const Block *b)
{
    if (strcmp(b->action, "OVERDUE") == 0) return 0;
    if (strcmp(b->action, "RETURNED") == 0 && b->fine_amount > 0) return 0;
    return 1;
}

static int cmp_pending(const void *a, const void *b)
{
    const Block *x = (const Block *)a;
    const Block *y = (const Block *)b;
    int px = pending_priority(x);
    int py = pending_priority(y);

    if (px != py) return px - py;
    if (x->timestamp != y->timestamp)
        return (x->timestamp < y->timestamp) ? -1 : 1;
    return 0;
}

void pending_pool_sort(PendingPool *p)
{
    if (p->count > 1)
        qsort(p->items, (size_t)p->count, sizeof *p->items, cmp_pending);
}

void pending_pool_print(const PendingPool *p)
{
    printf("\n=== PENDING POOL (%d) ===\n", p->count);
    if (p->count == 0) {
        printf("No records waiting to be mined.\n");
        return;
    }
    for (int i = 0; i < p->count; i++) {
        const Block *b = &p->items[i];
        printf("[%d] %-9s book=%-8s copy=%-8s member=%-8s %s reward=%d fine=%d\n",
               i + 1, b->action, b->book_id, b->copy_id, b->member_id,
               pending_priority(b) == 0 ? "PRIORITY" : "normal",
               b->token_reward, b->fine_amount);
    }
}

int chain_mine_pending(Chain *c, PendingPool *p, const char *sealer,
                       int difficulty, int *total_attempts)
{
    int  mined = 0;
    long total = 0;

    if (total_attempts) *total_attempts = 0;
    if (!p || p->count == 0) return 0;

    pending_pool_sort(p);

    for (int i = 0; i < p->count; i++) {
        Block rec = p->items[i];
        int   attempts = 0;

        if (chain_seal(c, &rec, sealer, difficulty, &attempts) != 0) break;
        mined++;
        total += attempts;
        printf("[mine] block #%d sealed: %d hash attempt(s), nonce %lu, hash %.16s...\n",
               rec.index, attempts, rec.nonce, rec.hash);
    }

    /* Remove the sealed items; keep the rest in order. */
    if (mined > 0) {
        memmove(p->items, p->items + mined,
                sizeof(Block) * (size_t)(p->count - mined));
        p->count -= mined;
    }
    if (total_attempts) *total_attempts = (int)total;
    return mined;
}

/* ============================================================
 *  Persistence (binary snapshot)
 * ============================================================ */
int chain_save(const Chain *c, const char *path)
{
    FILE *fp = fopen(path, "wb");
    int   ok = 1;

    if (!fp) { perror("chain_save"); return -1; }

    ok = ok && fwrite(CHAIN_MAGIC, 1, CHAIN_MAGIC_LEN, fp) == CHAIN_MAGIC_LEN;
    ok = ok && fwrite(&c->count, sizeof(int), 1, fp) == 1;
    for (int i = 0; ok && i < c->count; i++)
        ok = fwrite(&c->blocks[i], sizeof(Block), 1, fp) == 1;

    if (fclose(fp) != 0) ok = 0;
    return ok ? 0 : -1;
}

int chain_load(Chain *c, const char *path)
{
    FILE *fp = fopen(path, "rb");
    char  magic[CHAIN_MAGIC_LEN];
    int   count = 0;

    if (!fp) return 1;

    if (fread(magic, 1, CHAIN_MAGIC_LEN, fp) != CHAIN_MAGIC_LEN ||
        memcmp(magic, CHAIN_MAGIC, CHAIN_MAGIC_LEN) != 0) {
        fclose(fp);
        fprintf(stderr, "ERROR: '%s' is not an LBCT2 chain file. "
                        "Delete it if it came from an older build.\n", path);
        return -1;
    }
    if (fread(&count, sizeof count, 1, fp) != 1 ||
        count < 0 || count > MAX_CHAIN_BLOCKS) {
        fclose(fp);
        fprintf(stderr, "ERROR: '%s' has an invalid block count\n", path);
        return -1;
    }
    if (count == 0) {
        fclose(fp);
        c->count = 0;
        return 0;
    }
    if (chain_reserve(c, count) != 0) { fclose(fp); return -1; }

    if (fread(c->blocks, sizeof(Block), (size_t)count, fp) != (size_t)count) {
        fclose(fp);
        fprintf(stderr, "ERROR: '%s' is truncated\n", path);
        return -1;
    }
    fclose(fp);
    c->count = count;
    return 0;
}

/* ============================================================
 *  Validation
 * ============================================================ */
int chain_validate(const Chain *c, int *first_bad_index)
{
    int ok = 1;

    if (first_bad_index) *first_bad_index = -1;
    if (c->count == 0) {
        if (first_bad_index) *first_bad_index = 0;
        return 0;
    }

    for (int i = 0; i < c->count; i++) {
        const Block *b = &c->blocks[i];
        const char  *want_prev = (i == 0) ? ZERO_HASH : c->blocks[i - 1].hash;
        char         expect[HASH_HEX_LEN];
        int          bad = 0;

        if (strcmp(b->previous_hash, want_prev) != 0) bad = 1;        /* linkage  */
        if (i > 0 && b->timestamp <= c->blocks[i - 1].timestamp)
            bad = 1;                                                  /* time     */
        if (!record_verify(b)) bad = 1;                               /* ECDSA    */

        block_compute_merkle(b, expect);
        if (strcmp(expect, b->merkle_root) != 0) bad = 1;             /* Merkle   */

        block_compute_hash(b, expect);
        if (strcmp(expect, b->hash) != 0) bad = 1;                    /* hash     */

        if (!meets_difficulty(b->hash, b->difficulty)) bad = 1;       /* PoW      */

        if (bad && ok) {
            ok = 0;
            if (first_bad_index) *first_bad_index = i;
        }
    }
    return ok;
}

/* ============================================================
 *  Queries
 * ============================================================ */
const Block *chain_last_for_copy(const Chain *c, const char *copy_id)
{
    for (int i = c->count - 1; i >= 0; i--)
        if (strcmp(c->blocks[i].copy_id, copy_id) == 0)
            return &c->blocks[i];
    return NULL;
}

const Block *chain_last_for_book(const Chain *c, const char *book_id)
{
    for (int i = c->count - 1; i >= 0; i--)
        if (strcmp(c->blocks[i].book_id, book_id) == 0)
            return &c->blocks[i];
    return NULL;
}

/* ============================================================
 *  Display
 * ============================================================ */
static void fmt_time(time_t t, char *out, size_t n)
{
    struct tm *tm_info = localtime(&t);
    if (!tm_info) { snprintf(out, n, "(invalid)"); return; }
    strftime(out, n, "%Y-%m-%d %H:%M:%S", tm_info);
}

void chain_print(const Chain *c)
{
    char tbuf[32];

    printf("\n=============== LIBRARY LENDING LEDGER ===============\n");
    printf("Blocks in chain: %d\n", c->count);

    for (int i = 0; i < c->count; i++) {
        const Block *b = &c->blocks[i];
        fmt_time(b->timestamp, tbuf, sizeof tbuf);

        printf("\n-----------------------------------------------------\n");
        printf("Block #%d [%s] %s\n", b->index, b->action, tbuf);
        printf("  Book        : %s - \"%s\"\n", b->book_id, b->book_title);
        printf("  Copy        : %s\n", b->copy_id);
        printf("  Member      : %s - %s\n", b->member_id, b->member_name);
        printf("  Due         : %s\n", b->due_date[0] ? b->due_date : "-");
        printf("  Reward/Fine : %d / %d\n", b->token_reward, b->fine_amount);
        printf("  Sealed by   : %s (difficulty %d, nonce %lu)\n",
               b->sealed_by, b->difficulty, b->nonce);
        printf("  Merkle root : %.16s...\n", b->merkle_root);
        printf("  Hash        : %.16s...\n", b->hash);
        printf("  Signature   : %s\n", record_verify(b) ? "VALID" : "INVALID");
    }
    printf("\n-----------------------------------------------------\n");
}

/* Edits a copy of block #1 in memory only, then validates the copy. */
void chain_tamper_demo(const Chain *c)
{
    Chain copy;
    int   bad = -1;
    char  before[HASH_HEX_LEN];

    printf("\n=============== TAMPER DETECTION DEMO ===============\n");
    if (c->count < 2) {
        printf("Need at least 2 blocks. Borrow and return a book, then mine.\n");
        return;
    }

    chain_init_struct(&copy);
    copy.blocks = malloc(sizeof(Block) * (size_t)c->count);
    if (!copy.blocks) { printf("Out of memory.\n"); return; }
    memcpy(copy.blocks, c->blocks, sizeof(Block) * (size_t)c->count);
    copy.count    = c->count;
    copy.capacity = c->count;

    snprintf(before, sizeof before, "%s", copy.blocks[1].hash);
    printf("Target block #1, action before: %s\n", copy.blocks[1].action);
    snprintf(copy.blocks[1].action, sizeof copy.blocks[1].action, "RETURNED");
    printf("Edited copy of block #1, action after: %s\n", copy.blocks[1].action);
    printf("Stored hash (unchanged): %.16s...\n", before);

    if (chain_validate(&copy, &bad))
        printf("RESULT: UNEXPECTED - tampered copy still reports VALID.\n");
    else
        printf("RESULT: tampering detected at block #%d\n", bad);

    printf("The live chain and chain.dat were NOT modified.\n");
    printf("=====================================================\n");
    chain_free(&copy);
}