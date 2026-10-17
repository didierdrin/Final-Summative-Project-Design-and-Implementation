/* ============================================================
 *  File: src/main.c
 *  Library Blockchain Tracker: command-line interface
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "crypto_utils.h"
#include "registry.h"
#include "blockchain.h"

#define BOOKS_FILE   "books.txt"
#define MEMBERS_FILE "members.txt"
#define COPIES_FILE  "copies.txt"
#define CHAIN_FILE   "chain.dat"
#define KEY_DIR      "keys"
#define NODE_NAME    "solo-node"

#define LOAN_DAYS         14
#define RENEW_DAYS        14
#define RENEW_LIMIT       2
#define MAX_CONCURRENT    3
#define REBORROW_SECONDS  (24L * 60L * 60L)
#define ON_TIME_REWARD    10
#define LATE_REWARD       5
#define FINE_PER_DAY      1

#define MAX_ACCOUNTS      256
#define MAX_UTXOS         1024
#define MAX_RESERVATIONS  256
#define MAX_FINES         256
#define MAX_SUSPICIOUS    64

typedef enum {
    MODEL_UTXO = 1,
    MODEL_ACCOUNT = 2
} TxModel;

typedef struct {
    char          member_id[20];
    int           balance;
    unsigned long nonce;          /* counts outgoing transfers only */
} AccountEntry;

typedef struct {
    char member_id[20];
    int  amount;
    int  spent;
} UtxoEntry;

typedef struct TxLog {
    char          sender[20];
    char          recipient[20];
    int           amount;
    int           fee;
    unsigned long nonce;
    struct TxLog *next;
} TxLog;

typedef struct {
    char   book_id[20];
    char   member_id[20];
    time_t reserved_at;
    char   status[12];            /* WAITING, OFFERED, FULFILLED */
} Reservation;

typedef struct {
    char member_id[20];
    int  amount;
    int  settled;
    char reason[48];
} FineEntry;

typedef struct {
    char request_id[16];
    char book_id[20];
    char member_id[20];
    char reason[64];
    char status[12];              /* FLAGGED, APPROVED, REJECTED */
} SuspiciousEntry;

static Chain         g_chain;
static PendingPool   g_pending;
static TxModel       g_model = MODEL_UTXO;
static int           g_difficulty = 2;
static unsigned long g_tx_counter = 0;

static AccountEntry  g_accounts[MAX_ACCOUNTS];
static int           g_account_count = 0;
static UtxoEntry     g_utxos[MAX_UTXOS];
static int           g_utxo_count = 0;
static TxLog        *g_history[MAX_ACCOUNTS];

static Reservation   g_reservations[MAX_RESERVATIONS];
static int           g_reservation_count = 0;
static FineEntry     g_fines[MAX_FINES];
static int           g_fine_count = 0;
static SuspiciousEntry g_suspicious[MAX_SUSPICIOUS];
static int           g_suspicious_count = 0;

/* ============================================================
 *  Input and date helpers
 * ============================================================ */
static int read_line(const char *prompt, char *buf, size_t n)
{
    size_t len;
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)n, stdin)) { buf[0] = '\0'; return 0; }

    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return 1;
}

/* IDs: 1 to 19 characters, letters, digits, '-' or '_'. */
static int valid_id(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 19) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_') return 0;
    return 1;
}

/* Civil date <-> day count (proleptic Gregorian), timezone-free. */
static long days_from_civil(long y, long m, long d)
{
    long era, yoe, doy, doe;
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, char out[11])
{
    long era, doe, yoe, y, doy, mp, d, m;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y   = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    d   = doy - (153 * mp + 2) / 5 + 1;
    m   = mp < 10 ? mp + 3 : mp - 9;
    y  += (m <= 2);
    snprintf(out, 11, "%04ld-%02ld-%02ld", y, m, d);
}

static long today_days(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) return 0;
    return days_from_civil(t->tm_year + 1900L, t->tm_mon + 1L, t->tm_mday);
}

/* Returns the day count for "YYYY-MM-DD", or -1 if malformed. */
static long date_to_days(const char *ymd)
{
    int y, m, d;
    if (!ymd || sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3) return -1;
    if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
    return days_from_civil(y, m, d);
}

/* ============================================================
 *  Ledgers: account balances, UTXO set, transaction history
 * ============================================================ */
static AccountEntry *find_account(const char *id)
{
    for (int i = 0; i < g_account_count; i++)
        if (strcmp(g_accounts[i].member_id, id) == 0) return &g_accounts[i];
    return NULL;
}

static void add_account_if_missing(const char *member_id)
{
    if (find_account(member_id) != NULL) return;
    if (g_account_count >= MAX_ACCOUNTS) return;
    snprintf(g_accounts[g_account_count].member_id,
             sizeof g_accounts[g_account_count].member_id, "%s", member_id);
    g_accounts[g_account_count].balance = 0;
    g_accounts[g_account_count].nonce   = 0;
    g_history[g_account_count] = NULL;
    g_account_count++;
}

static void account_credit(const char *member_id, int amount)
{
    AccountEntry *acc = find_account(member_id);
    if (!acc) {
        add_account_if_missing(member_id);
        acc = find_account(member_id);
    }
    if (acc) acc->balance += amount;
}

static void utxo_add_reward(const char *member_id, int amount)
{
    if (g_utxo_count >= MAX_UTXOS) {
        printf("WARNING: UTXO pool is full.\n");
        return;
    }
    snprintf(g_utxos[g_utxo_count].member_id,
             sizeof g_utxos[g_utxo_count].member_id, "%s", member_id);
    g_utxos[g_utxo_count].amount = amount;
    g_utxos[g_utxo_count].spent  = 0;
    g_utxo_count++;
}

static int utxo_balance_for_member(const char *member_id)
{
    int total = 0;
    for (int i = 0; i < g_utxo_count; i++)
        if (!g_utxos[i].spent && strcmp(g_utxos[i].member_id, member_id) == 0)
            total += g_utxos[i].amount;
    return total;
}

static void free_history_list(TxLog *node)
{
    while (node) {
        TxLog *next = node->next;
        free(node);
        node = next;
    }
}

static void record_history(const char *sender, const char *recipient,
                           int amount, int fee, unsigned long nonce)
{
    for (int i = 0; i < g_account_count; i++) {
        if (strcmp(g_accounts[i].member_id, recipient) != 0 &&
            strcmp(g_accounts[i].member_id, sender) != 0)
            continue;
        TxLog *node = malloc(sizeof *node);
        if (!node) return;
        memset(node, 0, sizeof *node);
        snprintf(node->sender,    sizeof node->sender,    "%s", sender);
        snprintf(node->recipient, sizeof node->recipient, "%s", recipient);
        node->amount = amount;
        node->fee    = fee;
        node->nonce  = nonce;
        node->next   = g_history[i];
        g_history[i] = node;
    }
}

/* Applies one confirmed block to both balance models. Only confirmed
 * RETURNED blocks with a reward change balances. */
static void apply_confirmed_block(const Block *b)
{
    if (strcmp(b->action, "RETURNED") != 0 || b->token_reward <= 0) return;
    utxo_add_reward(b->member_id, b->token_reward);
    account_credit(b->member_id, b->token_reward);
    record_history("SYSTEM", b->member_id, b->token_reward, 0, 0);
}

static void reset_ledgers(void)
{
    g_account_count = 0;
    g_utxo_count    = 0;
    for (int i = 0; i < MAX_ACCOUNTS; i++) {
        free_history_list(g_history[i]);
        g_history[i] = NULL;
    }
    for (int j = 0; j < registry_member_count(); j++) {
        const Member *m = registry_member_at(j);
        if (m) add_account_if_missing(m->member_id);
    }
}

/* Balances are derived from the confirmed chain, so they survive restarts. */
static void rebuild_ledgers_from_chain(void)
{
    reset_ledgers();
    for (int i = 0; i < g_chain.count; i++)
        apply_confirmed_block(&g_chain.blocks[i]);
}

static void print_member_history(const char *member_id)
{
    int found = 0;
    printf("\n--- TRANSACTION HISTORY FOR %s ---\n", member_id);
    for (int i = 0; i < g_account_count; i++) {
        if (strcmp(g_accounts[i].member_id, member_id) != 0) continue;
        for (const TxLog *cur = g_history[i]; cur; cur = cur->next) {
            printf("sender=%s recipient=%s amount=%d fee=%d nonce=%lu\n",
                   cur->sender, cur->recipient, cur->amount, cur->fee, cur->nonce);
            found = 1;
        }
    }
    if (!found) printf("No transactions recorded for this member.\n");
}

static void print_balances(void)
{
    int total = 0;

    if (g_model == MODEL_UTXO) {
        printf("\n=== UTXO BALANCES ===\n");
        for (int i = 0; i < registry_member_count(); i++) {
            const Member *m = registry_member_at(i);
            if (m) printf("  %-8s -> %d\n", m->member_id,
                          utxo_balance_for_member(m->member_id));
        }
        printf("UTXO set (all unspent outputs):\n");
        if (g_utxo_count == 0) { printf("  (empty)\n"); return; }
        for (int i = 0; i < g_utxo_count; i++) {
            if (g_utxos[i].spent) continue;
            printf("  %-8s : %d\n", g_utxos[i].member_id, g_utxos[i].amount);
            total += g_utxos[i].amount;
        }
        printf("Total unspent value: %d\n", total);
        return;
    }

    printf("\n=== ACCOUNT BALANCES ===\n");
    for (int i = 0; i < g_account_count; i++)
        printf("  %-8s balance=%d nonce=%lu\n",
               g_accounts[i].member_id, g_accounts[i].balance, g_accounts[i].nonce);
}

/* ============================================================
 *  Copy inventory and reservations
 * ============================================================ */
static Copy *copy_at(int i)
{
    const Copy *k = registry_copy_at(i);
    return k ? registry_find_copy(k->copy_id) : NULL;
}

static Copy *find_available_copy(const char *book_id)
{
    for (int i = 0; i < registry_copy_count(); i++) {
        Copy *c = copy_at(i);
        if (c && strcmp(c->book_id, book_id) == 0 &&
            strcmp(c->state, "AVAILABLE") == 0)
            return c;
    }
    return NULL;
}

/* Prefers a copy offered to this member from the reservation queue. */
static Copy *find_copy_for_borrow(const char *book_id, const char *member_id)
{
    Copy *free_copy = NULL;
    for (int i = 0; i < registry_copy_count(); i++) {
        Copy *c = copy_at(i);
        if (!c || strcmp(c->book_id, book_id) != 0) continue;
        if (strcmp(c->state, "RESERVED") == 0 &&
            strcmp(c->current_holder_id, member_id) == 0)
            return c;
        if (!free_copy && strcmp(c->state, "AVAILABLE") == 0)
            free_copy = c;
    }
    return free_copy;
}

static int member_borrow_count(const char *member_id)
{
    int n = 0;
    for (int i = 0; i < registry_copy_count(); i++) {
        const Copy *c = copy_at(i);
        if (c && strcmp(c->current_holder_id, member_id) == 0 &&
            strcmp(c->state, "BORROWED") == 0)
            n++;
    }
    return n;
}

/* First WAITING reservation for a title: the queue is FIFO by array order. */
static Reservation *next_waiting(const char *book_id)
{
    for (int i = 0; i < g_reservation_count; i++)
        if (strcmp(g_reservations[i].book_id, book_id) == 0 &&
            strcmp(g_reservations[i].status, "WAITING") == 0)
            return &g_reservations[i];
    return NULL;
}

static int reservation_exists(const char *book_id, const char *member_id)
{
    for (int i = 0; i < g_reservation_count; i++) {
        Reservation *r = &g_reservations[i];
        if (strcmp(r->book_id, book_id) == 0 &&
            strcmp(r->member_id, member_id) == 0 &&
            (strcmp(r->status, "WAITING") == 0 || strcmp(r->status, "OFFERED") == 0))
            return 1;
    }
    return 0;
}

static void fulfil_offer(const char *book_id, const char *member_id)
{
    for (int i = 0; i < g_reservation_count; i++) {
        Reservation *r = &g_reservations[i];
        if (strcmp(r->book_id, book_id) == 0 &&
            strcmp(r->member_id, member_id) == 0 &&
            strcmp(r->status, "OFFERED") == 0) {
            snprintf(r->status, sizeof r->status, "FULFILLED");
            return;
        }
    }
}

/* ============================================================
 *  Pending records and fines
 * ============================================================ */
static void fill_record(Block *rec, const char *book_id, const char *title,
                        const char *copy_id, const char *member_id,
                        const char *name, const char *action, const char *due,
                        int reward, int fine)
{
    char seed[256];

    memset(rec, 0, sizeof *rec);
    snprintf(rec->book_id,    sizeof rec->book_id,    "%s", book_id);
    snprintf(rec->book_title, sizeof rec->book_title, "%s", title);
    snprintf(rec->copy_id,    sizeof rec->copy_id,    "%s", copy_id);
    snprintf(rec->member_id,  sizeof rec->member_id,  "%s", member_id);
    snprintf(rec->member_name, sizeof rec->member_name, "%s", name);
    snprintf(rec->action,     sizeof rec->action,     "%s", action);
    snprintf(rec->due_date,   sizeof rec->due_date,   "%s", due);
    rec->token_reward = reward;
    rec->fine_amount  = fine;
    rec->timestamp    = time(NULL);

    g_tx_counter++;
    snprintf(seed, sizeof seed, "%s|%s|%s|%d|%d|%ld|%lu",
             action, copy_id, member_id, reward, fine,
             (long)rec->timestamp, g_tx_counter);
    sha256_hex(seed, strlen(seed), rec->transaction_id);
}

static void append_fine(const char *member_id, int amount, const char *reason)
{
    FineEntry *f;
    for (int i = 0; i < g_fine_count; i++) {
        if (strcmp(g_fines[i].member_id, member_id) == 0 && !g_fines[i].settled) {
            g_fines[i].amount += amount;
            snprintf(g_fines[i].reason, sizeof g_fines[i].reason, "%s", reason);
            return;
        }
    }
    if (g_fine_count >= MAX_FINES) return;
    f = &g_fines[g_fine_count++];
    memset(f, 0, sizeof *f);
    snprintf(f->member_id, sizeof f->member_id, "%s", member_id);
    snprintf(f->reason,    sizeof f->reason,    "%s", reason);
    f->amount  = amount;
    f->settled = 0;
}

/* Stages a BORROWED record and moves the copy to BORROWED. */
static int stage_borrow(const Book *b, Copy *copy, const Member *m)
{
    Block rec;
    char  due[11];

    civil_from_days(today_days() + LOAN_DAYS, due);
    fill_record(&rec, b->book_id, b->title, copy->copy_id, m->member_id,
                m->full_name, "BORROWED", due, 0, 0);
    if (pending_pool_add(&g_pending, &rec) != 0) return -1;

    if (strcmp(copy->state, "RESERVED") == 0)
        fulfil_offer(b->book_id, m->member_id);

    snprintf(copy->state, sizeof copy->state, "BORROWED");
    snprintf(copy->current_holder_id, sizeof copy->current_holder_id, "%s", m->member_id);
    snprintf(copy->due_date, sizeof copy->due_date, "%s", due);
    copy->renew_count = 0;
    copy->fined_days  = 0;
    return 0;
}

/* Stages OVERDUE records and accrues fines per day overdue.
 * Returns the number of copies whose fine changed. */
static int scan_overdue(int verbose)
{
    long today = today_days();
    int  flagged = 0;

    for (int i = 0; i < registry_copy_count(); i++) {
        Copy *c = copy_at(i);
        const Book *b;
        const Member *m;
        long due, days_over, delta;

        if (!c || strcmp(c->state, "BORROWED") != 0) continue;
        due = date_to_days(c->due_date);
        if (due < 0 || today <= due) continue;

        days_over = today - due;
        delta     = days_over - c->fined_days;
        if (delta <= 0) continue;

        b = registry_find_book(c->book_id);
        m = registry_find_member(c->current_holder_id);
        if (!b || !m) continue;

        /* One OVERDUE record per overdue spell, staged on first detection. */
        if (c->fined_days == 0) {
            Block rec;
            fill_record(&rec, b->book_id, b->title, c->copy_id, m->member_id,
                        m->full_name, "OVERDUE", c->due_date, 0,
                        (int)(days_over * FINE_PER_DAY));
            if (pending_pool_add(&g_pending, &rec) != 0) continue;
        }

        append_fine(m->member_id, (int)(delta * FINE_PER_DAY), "overdue fine");
        c->fined_days = (int)days_over;
        flagged++;

        if (verbose)
            printf("  OVERDUE: copy %s (%s) held by %s, %ld day(s) late\n",
                   c->copy_id, b->title, m->member_id, days_over);
    }
    return flagged;
}

/* ============================================================
 *  Fraud review
 * ============================================================ */
static int member_reborrowed_recently(const char *member_id, const char *book_id)
{
    time_t now = time(NULL);

    for (int i = 0; i < g_chain.count; i++) {
        const Block *x = &g_chain.blocks[i];
        if (strcmp(x->member_id, member_id) == 0 &&
            strcmp(x->book_id, book_id) == 0 &&
            strcmp(x->action, "RETURNED") == 0 &&
            now - x->timestamp <= REBORROW_SECONDS)
            return 1;
    }
    for (int i = 0; i < g_pending.count; i++) {
        const Block *x = &g_pending.items[i];
        if (strcmp(x->member_id, member_id) == 0 &&
            strcmp(x->book_id, book_id) == 0 &&
            strcmp(x->action, "RETURNED") == 0 &&
            now - x->timestamp <= REBORROW_SECONDS)
            return 1;
    }
    return 0;
}

static void flag_request(const Book *b, const char *member_id, const char *reason)
{
    SuspiciousEntry *s;

    if (g_suspicious_count >= MAX_SUSPICIOUS) {
        printf("ERROR: fraud review queue is full.\n");
        return;
    }
    s = &g_suspicious[g_suspicious_count];
    memset(s, 0, sizeof *s);
    snprintf(s->request_id, sizeof s->request_id, "SR%04d", g_suspicious_count + 1);
    snprintf(s->book_id,    sizeof s->book_id,    "%s", b->book_id);
    snprintf(s->member_id,  sizeof s->member_id,  "%s", member_id);
    snprintf(s->reason,     sizeof s->reason,     "%s", reason);
    snprintf(s->status,     sizeof s->status,     "FLAGGED");
    g_suspicious_count++;

    printf("ALERT: borrow flagged SUSPICIOUS (%s). Request %s is held for fraud_review.\n",
           reason, s->request_id);
}

static SuspiciousEntry *find_suspicious(const char *request_id)
{
    for (int i = 0; i < g_suspicious_count; i++)
        if (strcmp(g_suspicious[i].request_id, request_id) == 0)
            return &g_suspicious[i];
    return NULL;
}

static void print_suspicious(void)
{
    printf("\n--- SUSPICIOUS REQUESTS (%d) ---\n", g_suspicious_count);
    if (g_suspicious_count == 0) { printf("None.\n"); return; }
    for (int i = 0; i < g_suspicious_count; i++)
        printf("%s | member=%s | book=%s | %s | %s\n",
               g_suspicious[i].request_id, g_suspicious[i].member_id,
               g_suspicious[i].book_id, g_suspicious[i].reason,
               g_suspicious[i].status);
}

/* ============================================================
 *  Startup: restore copy states from the confirmed chain
 * ============================================================ */
static void rebuild_copy_states_from_chain(void)
{
    /* Start every copy from a clean state, except those marked LOST in copies.txt. */
    for (int i = 0; i < registry_copy_count(); i++) {
        Copy *c = copy_at(i);
        if (!c || strcmp(c->state, "LOST") == 0) continue;
        snprintf(c->state, sizeof c->state, "AVAILABLE");
        c->current_holder_id[0] = '\0';
        c->due_date[0]    = '\0';
        c->renew_count    = 0;
        c->fined_days     = 0;
    }

    /* Replay the chain in order; the last record per copy wins. */
    for (int i = 0; i < g_chain.count; i++) {
        const Block *b = &g_chain.blocks[i];
        Copy *c;

        if (b->copy_id[0] == '\0' || strcmp(b->copy_id, "NONE") == 0) continue;
        c = registry_find_copy(b->copy_id);
        if (!c) continue;

        if (strcmp(b->action, "BORROWED") == 0) {
            snprintf(c->state, sizeof c->state, "BORROWED");
            snprintf(c->current_holder_id, sizeof c->current_holder_id, "%s", b->member_id);
            snprintf(c->due_date, sizeof c->due_date, "%s", b->due_date);
            c->renew_count = 0;
            c->fined_days  = 0;
        } else if (strcmp(b->action, "RENEWED") == 0) {
            snprintf(c->due_date, sizeof c->due_date, "%s", b->due_date);
            c->renew_count++;
        } else if (strcmp(b->action, "OVERDUE") == 0) {
            snprintf(c->due_date, sizeof c->due_date, "%s", b->due_date);
            c->fined_days = b->fine_amount / FINE_PER_DAY;
        } else if (strcmp(b->action, "RETURNED") == 0) {
            snprintf(c->state, sizeof c->state, "AVAILABLE");
            c->current_holder_id[0] = '\0';
            c->due_date[0] = '\0';
            c->renew_count = 0;
            c->fined_days  = 0;
        } else if (strcmp(b->action, "LOST") == 0) {
            snprintf(c->state, sizeof c->state, "LOST");
            snprintf(c->condition, sizeof c->condition, "LOST");
            c->current_holder_id[0] = '\0';
            c->due_date[0] = '\0';
        }
    }
}

/* ============================================================
 *  Startup: chain load and validation
 * ============================================================ */
static void persist_or_warn(void)
{
    if (chain_save(&g_chain, CHAIN_FILE) != 0)
        printf("WARNING: chain could not be written to disk.\n");
}

static int init_chain(void)
{
    int r = chain_load(&g_chain, CHAIN_FILE);

    if (r < 0) {
        fprintf(stderr, "FATAL: chain file '%s' could not be read\n", CHAIN_FILE);
        return -1;
    }
    if (r == 1 || g_chain.count == 0) {
        if (chain_genesis(&g_chain) != 0) {
            fprintf(stderr, "FATAL: could not create genesis block\n");
            return -1;
        }
        if (chain_save(&g_chain, CHAIN_FILE) != 0) {
            fprintf(stderr, "FATAL: could not persist new chain\n");
            return -1;
        }
        printf("[chain] created new chain with genesis block -> %s\n", CHAIN_FILE);
        return 0;
    }

    {
        int bad = -1;
        if (!chain_validate(&g_chain, &bad)) {
            fprintf(stderr, "FATAL: loaded chain failed validation at block #%d\n", bad);
            return -1;
        }
    }
    printf("[chain] loaded and validated %d block(s) from %s\n", g_chain.count, CHAIN_FILE);
    return 0;
}

/* ============================================================
 *  Commands
 * ============================================================ */
static void do_borrow(void)
{
    char bid[64], mid[64];
    const Book *b;
    const Member *m;
    Copy *copy;

    printf("\n--- BORROW BOOK ---\n");
    if (!read_line("Book ID   : ", bid, sizeof bid)) return;
    if (!read_line("Member ID : ", mid, sizeof mid)) return;
    if (!valid_id(bid) || !valid_id(mid)) { printf("ERROR: invalid ID format.\n"); return; }

    b = registry_find_book(bid);
    m = registry_find_member(mid);
    if (!b || !m) { printf("ERROR: Book or Member not found\n"); return; }

    /* Fraud heuristics run before any copy is assigned. */
    if (member_borrow_count(m->member_id) >= MAX_CONCURRENT) {
        flag_request(b, m->member_id, "excessive concurrent borrows");
        return;
    }
    if (member_reborrowed_recently(m->member_id, b->book_id)) {
        flag_request(b, m->member_id, "rapid re-borrow within 24 hours");
        return;
    }

    copy = find_copy_for_borrow(b->book_id, m->member_id);
    if (!copy) {
        printf("ERROR: no copy is available. Use reserve_book to join the queue.\n");
        return;
    }
    if (stage_borrow(b, copy, m) != 0) {
        printf("ERROR: pending pool is full. Mine the pool first.\n");
        return;
    }

    printf("\nSUCCESS: %s (%s) borrowed \"%s\" copy %s, due %s.\n",
           m->full_name, m->member_id, b->title, copy->copy_id, copy->due_date);
    printf("Borrow staged in the pending pool. Mine it to confirm.\n");
}

static void do_return(void)
{
    char bid[64], cid[64], mid[64];
    const Book *b;
    const Member *m;
    Copy *copy;
    Reservation *next;
    Block rec;
    long today, due, days_late;
    int  reward, fine_total, fine_new;

    printf("\n--- RETURN BOOK ---\n");
    if (!read_line("Book ID   : ", bid, sizeof bid)) return;
    if (!read_line("Copy ID   : ", cid, sizeof cid)) return;
    if (!read_line("Member ID : ", mid, sizeof mid)) return;
    if (!valid_id(bid) || !valid_id(cid) || !valid_id(mid)) {
        printf("ERROR: invalid ID format.\n");
        return;
    }

    b = registry_find_book(bid);
    m = registry_find_member(mid);
    copy = registry_find_copy(cid);
    if (!b || !m) { printf("ERROR: Book or Member not found\n"); return; }
    if (!copy || strcmp(copy->book_id, b->book_id) != 0) {
        printf("ERROR: copy %s does not belong to book %s.\n", cid, bid);
        return;
    }
    if (strcmp(copy->state, "BORROWED") != 0 ||
        strcmp(copy->current_holder_id, m->member_id) != 0) {
        printf("ERROR: copy %s is not on loan to %s.\n", cid, mid);
        return;
    }

    /* Late means returned after the due date. */
    today     = today_days();
    due       = date_to_days(copy->due_date);
    days_late = (due >= 0 && today > due) ? today - due : 0;
    reward    = days_late > 0 ? LATE_REWARD : ON_TIME_REWARD;
    fine_total = (int)(days_late * FINE_PER_DAY);
    fine_new   = (int)((days_late - copy->fined_days) * FINE_PER_DAY);
    if (fine_new < 0) fine_new = 0;

    fill_record(&rec, b->book_id, b->title, copy->copy_id, m->member_id,
                m->full_name, "RETURNED", copy->due_date, reward, fine_total);
    if (pending_pool_add(&g_pending, &rec) != 0) {
        printf("ERROR: pending pool is full. Mine the pool first.\n");
        return;
    }
    if (fine_new > 0) append_fine(m->member_id, fine_new, "late return");

    /* Release the copy: offer it to the next waiting member, or make it available. */
    next = next_waiting(b->book_id);
    if (next) {
        snprintf(copy->state, sizeof copy->state, "RESERVED");
        snprintf(copy->current_holder_id, sizeof copy->current_holder_id, "%s", next->member_id);
        copy->due_date[0] = '\0';
        copy->renew_count = 0;
        copy->fined_days  = 0;
        snprintf(next->status, sizeof next->status, "OFFERED");
        printf("Copy offered to the next reservation: %s\n", next->member_id);
    } else {
        snprintf(copy->state, sizeof copy->state, "AVAILABLE");
        copy->current_holder_id[0] = '\0';
        copy->due_date[0] = '\0';
        copy->renew_count = 0;
        copy->fined_days  = 0;
    }

    printf("\nSUCCESS: copy %s returned by %s (%s).\n",
           cid, m->full_name, days_late > 0 ? "LATE" : "on time");
    printf("Reward pending: %d tokens. Fine on this return: %d.\n", reward, fine_total);
    printf("Mine the pending pool to confirm the reward.\n");
}

static void do_overdue(void)
{
    int n;
    printf("\n--- CHECK OVERDUE ---\n");
    n = scan_overdue(1);
    if (n == 0) printf("No newly overdue copies.\n");
    else printf("%d copy/copies updated. OVERDUE records are in the pending pool.\n", n);
}

static void do_reserve(void)
{
    char bid[64], mid[64];
    const Book *b;
    const Member *m;
    Reservation *r;
    int pos = 0;

    printf("\n--- RESERVE BOOK ---\n");
    if (!read_line("Book ID   : ", bid, sizeof bid)) return;
    if (!read_line("Member ID : ", mid, sizeof mid)) return;
    if (!valid_id(bid) || !valid_id(mid)) { printf("ERROR: invalid ID format.\n"); return; }

    b = registry_find_book(bid);
    m = registry_find_member(mid);
    if (!b || !m) { printf("ERROR: Book or Member not found\n"); return; }

    if (find_available_copy(b->book_id)) {
        printf("ERROR: a copy is available now. Use borrow_book.\n");
        return;
    }
    if (reservation_exists(b->book_id, m->member_id)) {
        printf("ERROR: %s already has a reservation for this title.\n", m->member_id);
        return;
    }
    if (g_reservation_count >= MAX_RESERVATIONS) {
        printf("ERROR: reservation queue is full.\n");
        return;
    }

    r = &g_reservations[g_reservation_count++];
    memset(r, 0, sizeof *r);
    snprintf(r->book_id,   sizeof r->book_id,   "%s", b->book_id);
    snprintf(r->member_id, sizeof r->member_id, "%s", m->member_id);
    r->reserved_at = time(NULL);
    snprintf(r->status, sizeof r->status, "WAITING");

    for (int i = 0; i < g_reservation_count; i++)
        if (strcmp(g_reservations[i].book_id, b->book_id) == 0 &&
            strcmp(g_reservations[i].status, "WAITING") == 0)
            pos++;

    printf("Reserved \"%s\" for %s. Queue position: %d\n", b->title, m->member_id, pos);
}

static void do_renew(void)
{
    char cid[64], mid[64], new_due[11];
    Copy *copy;
    const Member *m;
    const Book *b;
    const Reservation *w;
    long today, due;
    Block rec;

    printf("\n--- RENEW BOOK ---\n");
    if (!read_line("Copy ID   : ", cid, sizeof cid)) return;
    if (!read_line("Member ID : ", mid, sizeof mid)) return;
    if (!valid_id(cid) || !valid_id(mid)) { printf("ERROR: invalid ID format.\n"); return; }

    copy = registry_find_copy(cid);
    m    = registry_find_member(mid);
    if (!copy || !m) { printf("ERROR: copy or member not found.\n"); return; }
    if (strcmp(copy->state, "BORROWED") != 0 ||
        strcmp(copy->current_holder_id, m->member_id) != 0) {
        printf("ERROR: copy %s is not on loan to %s.\n", cid, mid);
        return;
    }
    if (copy->renew_count >= RENEW_LIMIT) {
        printf("ERROR: renewal limit (%d) reached.\n", RENEW_LIMIT);
        return;
    }

    today = today_days();
    due   = date_to_days(copy->due_date);
    if (due < 0 || due < today) {
        printf("ERROR: copy is overdue. Return it and settle the fine.\n");
        return;
    }

    w = next_waiting(copy->book_id);
    if (w && strcmp(w->member_id, m->member_id) != 0) {
        printf("ERROR: another member is waiting for this title.\n");
        return;
    }

    b = registry_find_book(copy->book_id);
    if (!b) { printf("ERROR: book record missing.\n"); return; }

    civil_from_days(due + RENEW_DAYS, new_due);
    fill_record(&rec, b->book_id, b->title, copy->copy_id, m->member_id,
                m->full_name, "RENEWED", new_due, 0, 0);
    if (pending_pool_add(&g_pending, &rec) != 0) {
        printf("ERROR: pending pool is full. Mine the pool first.\n");
        return;
    }

    snprintf(copy->due_date, sizeof copy->due_date, "%s", new_due);
    copy->renew_count++;
    printf("Renewed copy %s. New due date: %s (renewal %d of %d)\n",
           copy->copy_id, copy->due_date, copy->renew_count, RENEW_LIMIT);
}

static void do_view_fines(void)
{
    printf("\n--- FINES ---\n");
    if (g_fine_count == 0) { printf("No fines recorded.\n"); return; }
    for (int i = 0; i < g_fine_count; i++)
        printf("%-8s amount=%d settled=%s (%s)\n",
               g_fines[i].member_id, g_fines[i].amount,
               g_fines[i].settled ? "yes" : "no", g_fines[i].reason);
}

static void do_settle_fine(void)
{
    char mid[64];
    int  total = 0;

    printf("\n--- SETTLE FINE ---\n");
    if (!read_line("Member ID: ", mid, sizeof mid)) return;
    if (!valid_id(mid)) { printf("ERROR: invalid ID format.\n"); return; }

    for (int i = 0; i < g_fine_count; i++) {
        if (strcmp(g_fines[i].member_id, mid) == 0 && !g_fines[i].settled) {
            total += g_fines[i].amount;
            g_fines[i].settled = 1;
        }
    }
    if (total == 0) printf("No unsettled fine for %s.\n", mid);
    else printf("Settled %d for %s.\n", total, mid);
}

static void do_reserve_status(void)
{
    printf("\n--- RESERVATION QUEUE ---\n");
    if (g_reservation_count == 0) { printf("No reservations.\n"); return; }
    for (int i = 0; i < g_reservation_count; i++)
        printf("%-8s %-8s %s\n", g_reservations[i].book_id,
               g_reservations[i].member_id, g_reservations[i].status);
}

static void do_pool_view(void)
{
    pending_pool_print(&g_pending);
    print_suspicious();
}

static void do_fraud_review(void)
{
    print_suspicious();
}

static void do_approve(void)
{
    char rid[32];
    SuspiciousEntry *s;
    const Book *b;
    const Member *m;
    Copy *copy;

    printf("\n--- APPROVE SUSPICIOUS REQUEST ---\n");
    if (!read_line("Request ID: ", rid, sizeof rid)) return;
    s = find_suspicious(rid);
    if (!s || strcmp(s->status, "FLAGGED") != 0) {
        printf("ERROR: no flagged request %s.\n", rid);
        return;
    }
    b = registry_find_book(s->book_id);
    m = registry_find_member(s->member_id);
    if (!b || !m) { printf("ERROR: record missing for this request.\n"); return; }

    copy = find_copy_for_borrow(b->book_id, m->member_id);
    if (!copy) {
        printf("ERROR: no copy is available now. The request stays flagged.\n");
        return;
    }
    if (stage_borrow(b, copy, m) != 0) {
        printf("ERROR: pending pool is full.\n");
        return;
    }
    snprintf(s->status, sizeof s->status, "APPROVED");
    printf("Approved %s. Borrow of copy %s staged for mining.\n", rid, copy->copy_id);
}

static void do_reject(void)
{
    char rid[32];
    SuspiciousEntry *s;

    printf("\n--- REJECT SUSPICIOUS REQUEST ---\n");
    if (!read_line("Request ID: ", rid, sizeof rid)) return;
    s = find_suspicious(rid);
    if (!s || strcmp(s->status, "FLAGGED") != 0) {
        printf("ERROR: no flagged request %s.\n", rid);
        return;
    }
    snprintf(s->status, sizeof s->status, "REJECTED");
    printf("Rejected %s. Nothing was borrowed.\n", rid);
}

static void do_view_copies(void)
{
    registry_print_copies();
}

static void do_view_copy(void)
{
    char cid[64];
    if (!read_line("\nCopy ID: ", cid, sizeof cid)) return;
    registry_print_copy(cid);
}

static void do_add_copy(void)
{
    char cid[64], bid[64], cond[32], state[32];
    FILE *fp;

    printf("\n--- ADD COPY ---\n");
    if (!read_line("Copy ID   : ", cid, sizeof cid)) return;
    if (!read_line("Book ID   : ", bid, sizeof bid)) return;
    if (!read_line("Condition (NEW/GOOD/WORN): ", cond, sizeof cond)) return;
    if (!read_line("State (AVAILABLE): ", state, sizeof state)) return;

    if (!valid_id(cid) || !valid_id(bid)) { printf("ERROR: invalid ID format.\n"); return; }
    if (!registry_find_book(bid)) { printf("ERROR: book not found.\n"); return; }
    if (strcmp(cond, "NEW") != 0 && strcmp(cond, "GOOD") != 0 && strcmp(cond, "WORN") != 0) {
        printf("ERROR: condition must be NEW, GOOD or WORN.\n");
        return;
    }
    if (strcmp(state, "AVAILABLE") != 0) {
        printf("ERROR: a new copy must start AVAILABLE.\n");
        return;
    }
    if (registry_add_copy(cid, bid, cond, "AVAILABLE") != 0) {
        printf("ERROR: duplicate copy ID or registry full.\n");
        return;
    }

    /* Persist the new copy so it survives a restart. */
    fp = fopen(COPIES_FILE, "a");
    if (fp) {
        fprintf(fp, "\n%s,%s,%s,AVAILABLE\n", cid, bid, cond);
        fclose(fp);
    } else {
        printf("WARNING: could not write the copy to %s.\n", COPIES_FILE);
    }
    printf("Copy %s added.\n", cid);
}

static void do_mark_lost(void)
{
    char cid[64], reason[48];
    Copy *copy;
    const Book *b;
    Block rec;

    printf("\n--- MARK COPY LOST ---\n");
    if (!read_line("Copy ID : ", cid, sizeof cid)) return;
    if (!read_line("Reason  : ", reason, sizeof reason)) return;
    if (!valid_id(cid) || reason[0] == '\0') { printf("ERROR: copy ID and reason are required.\n"); return; }

    copy = registry_find_copy(cid);
    if (!copy) { printf("ERROR: copy not found.\n"); return; }
    if (strcmp(copy->state, "AVAILABLE") != 0 && strcmp(copy->state, "BORROWED") != 0) {
        printf("ERROR: only AVAILABLE or BORROWED copies can be marked lost (state is %s).\n",
               copy->state);
        return;
    }
    b = registry_find_book(copy->book_id);
    if (!b) { printf("ERROR: book record missing.\n"); return; }

    /* The reason is stored in the member_name field of the LOST record. */
    fill_record(&rec, b->book_id, b->title, copy->copy_id, "LIBRARIAN",
                reason, "LOST", "", 0, 0);
    if (pending_pool_add(&g_pending, &rec) != 0) {
        printf("ERROR: pending pool is full. Mine the pool first.\n");
        return;
    }
    snprintf(copy->state, sizeof copy->state, "LOST");
    snprintf(copy->condition, sizeof copy->condition, "LOST");
    copy->current_holder_id[0] = '\0';
    copy->due_date[0] = '\0';
    printf("Copy %s marked LOST (reason: %s). Mine the pool to confirm.\n", cid, reason);
}

static void do_validate(void)
{
    int bad = -1;
    printf("\n--- CHAIN VALIDATION ---\n");
    if (chain_validate(&g_chain, &bad))
        printf("CHAIN VALID: all %d block(s) verified.\n", g_chain.count);
    else
        printf("CHAIN INVALID: tampering detected at block #%d\n", bad);
}

static void do_audit_book(void)
{
    char bid[64];
    const Book *b;
    int found = 0, on_loan = 0, total = 0;

    printf("\n--- BOOK HISTORY ---\n");
    if (!read_line("Book ID: ", bid, sizeof bid)) return;
    b = registry_find_book(bid);
    if (!b) { printf("ERROR: Book not found\n"); return; }

    printf("History for \"%s\" (%s):\n", b->title, b->book_id);
    for (int i = 0; i < g_chain.count; i++) {
        const Block *x = &g_chain.blocks[i];
        char tbuf[32];
        struct tm *tm_info;

        if (strcmp(x->book_id, b->book_id) != 0) continue;
        tm_info = localtime(&x->timestamp);
        if (tm_info) strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", tm_info);
        else snprintf(tbuf, sizeof tbuf, "-");
        printf("  #%-3d %-9s copy=%-8s member=%-8s %s\n",
               x->index, x->action, x->copy_id, x->member_id, tbuf);
        found = 1;
    }
    if (!found) printf("  (no confirmed lending events)\n");

    for (int i = 0; i < registry_copy_count(); i++) {
        const Copy *c = registry_copy_at(i);
        if (c && strcmp(c->book_id, b->book_id) == 0) {
            total++;
            if (strcmp(c->state, "BORROWED") == 0) on_loan++;
        }
    }
    printf("  Copies on loan now: %d of %d\n", on_loan, total);
}

static void do_mine(void)
{
    int before, mined, attempts = 0;

    printf("\n--- MINE PENDING POOL (difficulty %d) ---\n", g_difficulty);
    if (g_pending.count == 0) { printf("No pending records to mine.\n"); return; }

    before = g_chain.count;
    mined  = chain_mine_pending(&g_chain, &g_pending, NODE_NAME, g_difficulty, &attempts);

    /* Apply balances only to the blocks that were actually appended. */
    for (int i = before; i < g_chain.count; i++)
        apply_confirmed_block(&g_chain.blocks[i]);

    if (mined > 0) persist_or_warn();

    printf("Confirmed %d block(s). Total hash attempts: %d\n", mined, attempts);
    print_balances();
}

static void do_switch_model(void)
{
    char sel[16];
    printf("\n--- SWITCH TRANSACTION MODEL ---\n");
    printf("1. UTXO model\n2. Account-based model\n");
    if (!read_line("Choice > ", sel, sizeof sel)) return;
    if (strcmp(sel, "1") == 0)      g_model = MODEL_UTXO;
    else if (strcmp(sel, "2") == 0) g_model = MODEL_ACCOUNT;
    else { printf("Invalid selection.\n"); return; }
    printf("Current model: %s\n", g_model == MODEL_UTXO ? "UTXO" : "Account-based");
}

static void do_set_difficulty(void)
{
    char sel[16];
    printf("\n--- SET MINING DIFFICULTY ---\n");
    if (!read_line("Difficulty (1-4): ", sel, sizeof sel)) return;
    g_difficulty = atoi(sel);
    if (g_difficulty < 1 || g_difficulty > 4) {
        g_difficulty = 2;
        printf("Difficulty out of range; reset to default 2.\n");
    }
    printf("Mining difficulty is now %d.\n", g_difficulty);
}

static void do_member_history(void)
{
    char mid[64];
    if (!read_line("\nMember ID: ", mid, sizeof mid)) return;
    if (!valid_id(mid) || registry_find_member(mid) == NULL) {
        printf("ERROR: Member not found.\n");
        return;
    }
    print_member_history(mid);
}

static void do_tamper(void)
{
    chain_tamper_demo(&g_chain);
}

static void print_menu(void)
{
    printf("\n==================================================\n");
    printf("  LIBRARY BLOCKCHAIN TRACKER\n");
    printf("==================================================\n");
    printf("  1. Borrow a book\n");
    printf("  2. Return a book (creates pending reward)\n");
    printf("  3. View copies\n");
    printf("  4. View lending ledger\n");
    printf("  5. Validate chain integrity\n");
    printf("  6. Tamper-detection demonstration\n");
    printf("  7. Show book registry\n");
    printf("  8. Show member registry\n");
    printf("  9. Audit a book's history\n");
    printf(" 10. Check overdue copies\n");
    printf(" 11. Reserve a book\n");
    printf(" 12. Renew a book\n");
    printf(" 13. View pending pool and fraud queue\n");
    printf(" 14. View fines\n");
    printf(" 15. Settle a fine\n");
    printf(" 16. Fraud review\n");
    printf(" 17. Approve suspicious request\n");
    printf(" 18. Reject suspicious request\n");
    printf(" 19. Mine pending pool\n");
    printf(" 20. Switch transaction model\n");
    printf(" 21. Set mining difficulty\n");
    printf(" 22. Show balances\n");
    printf(" 23. Show member transaction history\n");
    printf(" 24. Add copy\n");
    printf(" 25. View copy\n");
    printf(" 26. Mark copy lost\n");
    printf(" 27. View reservation queue\n");
    printf("  0. Exit\n");
    printf("--------------------------------------------------\n");
}

/* ============================================================
 *  Main
 * ============================================================ */
int main(void)
{
    int running = 1;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n##################################################\n");
    printf("#  Blockchain-Based Library Book Lending Tracker #\n");
    printf("#  Proof-of-work + token rewards + ECDSA        #\n");
    printf("##################################################\n\n");

    chain_init_struct(&g_chain);
    pending_pool_init(&g_pending);

    if (crypto_init(KEY_DIR) != 0) {
        fprintf(stderr, "FATAL: could not initialise cryptography.\n");
        return 1;
    }
    if (registry_load_books(BOOKS_FILE) != 0 ||
        registry_load_members(MEMBERS_FILE) != 0 ||
        registry_load_copies(COPIES_FILE) != 0) {
        fprintf(stderr, "FATAL: a registry file is missing or empty.\n");
        crypto_cleanup();
        return 1;
    }
    if (init_chain() != 0) {
        crypto_cleanup();
        return 1;
    }

    rebuild_copy_states_from_chain();
    rebuild_ledgers_from_chain();

    {
        int n = scan_overdue(0);
        if (n > 0)
            printf("[overdue] %d copy/copies newly overdue; OVERDUE records are pending.\n", n);
    }

    while (running) {
        char line[32];
        int  choice;

        print_menu();
        if (!read_line("Choice > ", line, sizeof line)) {
            printf("\nEnd of input.\n");
            break;
        }
        if (line[0] == '\0') continue;
        choice = atoi(line);

        switch (choice) {
            case 1:  do_borrow(); break;
            case 2:  do_return(); break;
            case 3:  do_view_copies(); break;
            case 4:  chain_print(&g_chain); break;
            case 5:  do_validate(); break;
            case 6:  do_tamper(); break;
            case 7:  registry_print_books(); break;
            case 8:  registry_print_members(); break;
            case 9:  do_audit_book(); break;
            case 10: do_overdue(); break;
            case 11: do_reserve(); break;
            case 12: do_renew(); break;
            case 13: do_pool_view(); break;
            case 14: do_view_fines(); break;
            case 15: do_settle_fine(); break;
            case 16: do_fraud_review(); break;
            case 17: do_approve(); break;
            case 18: do_reject(); break;
            case 19: do_mine(); break;
            case 20: do_switch_model(); break;
            case 21: do_set_difficulty(); break;
            case 22: print_balances(); break;
            case 23: do_member_history(); break;
            case 24: do_add_copy(); break;
            case 25: do_view_copy(); break;
            case 26: do_mark_lost(); break;
            case 27: do_reserve_status(); break;
            case 0:  running = 0; break;
            default: printf("Invalid option. Please choose 0-27.\n");
        }
    }

    persist_or_warn();
    if (g_pending.count > 0)
        printf("WARNING: %d unmined record(s) were discarded on exit. Mine before exiting.\n",
               g_pending.count);

    pending_pool_free(&g_pending);
    chain_free(&g_chain);
    crypto_cleanup();
    printf("Goodbye.\n");
    return 0;
}