/* ============================================================
 *  File: src/main.c
 *  Extended Library Blockchain Tracker - CLI
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto_utils.h"
#include "registry.h"
#include "blockchain.h"

#define BOOKS_FILE   "books.txt"
#define MEMBERS_FILE "members.txt"
#define CHAIN_FILE   "chain.dat"
#define KEY_DIR      "keys"

#define MAX_ACCOUNTS 256
#define MAX_UTXOS    1024

typedef enum {
    MODEL_UTXO = 1,
    MODEL_ACCOUNT = 2
} TxModel;

typedef struct {
    char member_id[20];
    int  balance;
    unsigned long nonce;
    int  tx_count;
} AccountEntry;

typedef struct {
    char member_id[20];
    int  amount;
    int  spent;
} UtxoEntry;

typedef struct TxLog {
    char sender[20];
    char recipient[20];
    int  amount;
    int  fee;
    unsigned long nonce;
    struct TxLog *next;
} TxLog;

static Chain g_chain;
static PendingPool g_pending;
static TxModel g_model = MODEL_UTXO;
static int g_difficulty = 2;
static AccountEntry g_accounts[MAX_ACCOUNTS];
static int g_account_count = 0;
static UtxoEntry g_utxos[MAX_UTXOS];
static int g_utxo_count = 0;
static TxLog *g_history[MAX_ACCOUNTS];

typedef struct {
    char request_id[24];
    char book_id[20];
    char copy_id[24];
    char member_id[20];
    char action_requested[16];
    int  priority;
    time_t queued_at;
    char status[16];
} PendingRequest;

typedef struct {
    char member_id[20];
    int  amount;
    char reason[64];
    int  settled;
} FineEntry;

typedef struct {
    char request_id[24];
    char member_id[20];
    char status[16];
    char reason[80];
} FraudEntry;

static PendingRequest g_requests[256];
static int g_request_count = 0;
static FineEntry g_fines[256];
static int g_fine_count = 0;
static FraudEntry g_fraud[128];
static int g_fraud_count = 0;

static void add_account_if_missing(const char *member_id);
static AccountEntry *find_account(const char *id);
static void utxo_add_reward(const char *member_id, int amount);
static void account_credit(const char *member_id, int amount);
static void record_history(const char *sender, const char *recipient, int amount, int fee, unsigned long nonce);

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

static int init_chain(void)
{
    int r;
    chain_init_struct(&g_chain);
    pending_pool_init(&g_pending);
    r = chain_load(&g_chain, CHAIN_FILE);

    if (r == 1) {
        if (chain_genesis(&g_chain) != 0) {
            fprintf(stderr, "FATAL: could not create genesis block\n");
            return -1;
        }
        if (chain_save(&g_chain, CHAIN_FILE) != 0) {
            fprintf(stderr, "FATAL: could not persist new chain\n");
            return -1;
        }
        printf("[chain]    created new chain with genesis block -> %s\n", CHAIN_FILE);
    } else if (r != 0) {
        fprintf(stderr, "FATAL: chain file '%s' could not be read\n", CHAIN_FILE);
        return -1;
    } else {
        int bad = -1;
        if (!chain_validate(&g_chain, &bad)) {
            fprintf(stderr, "FATAL: loaded chain failed validation at block #%d\n", bad);
            return -1;
        }
        printf("[chain]    loaded %d block(s) from %s\n", g_chain.count, CHAIN_FILE);
    }
    return 0;
}

static void persist_or_warn(void)
{
    if (chain_save(&g_chain, CHAIN_FILE) != 0)
        printf("WARNING: chain could not be written to disk.\n");
}

static int equals_ignore_case(const char *a, const char *b)
{
    unsigned char ca, cb;
    while (*a && *b) {
        ca = (unsigned char)*a; cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void init_accounts(void)
{
    int i;
    for (i = 0; i < MAX_ACCOUNTS; i++) {
        g_accounts[i].member_id[0] = '\0';
        g_accounts[i].balance = 0;
        g_accounts[i].nonce = 0;
        g_accounts[i].tx_count = 0;
        g_history[i] = NULL;
    }
    g_account_count = 0;
    for (i = 0; i < MAX_UTXOS; i++) {
        g_utxos[i].member_id[0] = '\0';
        g_utxos[i].amount = 0;
        g_utxos[i].spent = 0;
    }
    g_utxo_count = 0;

    for (int j = 0; j < registry_member_count(); j++) {
        const Member *m = registry_member_at(j);
        if (m) add_account_if_missing(m->member_id);
    }
}

static void rebuild_balances_from_chain(void)
{
    int i;

    for (i = 0; i < MAX_ACCOUNTS; i++) {
        g_accounts[i].balance = 0;
        g_accounts[i].nonce = 0;
        g_history[i] = NULL;
    }
    for (i = 0; i < MAX_UTXOS; i++) {
        g_utxos[i].amount = 0;
        g_utxos[i].spent = 0;
        g_utxos[i].member_id[0] = '\0';
    }
    g_utxo_count = 0;

    for (i = 0; i < g_chain.count; i++) {
        const Block *b = &g_chain.blocks[i];
        if (strcmp(b->action, "RETURNED") != 0) continue;
        if (b->token_reward <= 0) continue;

        utxo_add_reward(b->member_id, b->token_reward);
        account_credit(b->member_id, b->token_reward);
        record_history("SYSTEM", b->member_id, b->token_reward, 1, 0);
    }
}

static AccountEntry *find_account(const char *id)
{
    for (int i = 0; i < g_account_count; i++) {
        if (strcmp(g_accounts[i].member_id, id) == 0)
            return &g_accounts[i];
    }
    return NULL;
}

static void add_account_if_missing(const char *member_id)
{
    if (find_account(member_id) != NULL) return;
    if (g_account_count >= MAX_ACCOUNTS) return;
    snprintf(g_accounts[g_account_count].member_id, sizeof(g_accounts[g_account_count].member_id), "%s", member_id);
    g_accounts[g_account_count].balance = 0;
    g_accounts[g_account_count].nonce = 0;
    g_accounts[g_account_count].tx_count = 0;
    g_history[g_account_count] = NULL;
    g_account_count++;
}

static int utxo_balance_for_member(const char *member_id)
{
    int total = 0;
    for (int i = 0; i < g_utxo_count; i++) {
        if (strcmp(g_utxos[i].member_id, member_id) == 0 && !g_utxos[i].spent)
            total += g_utxos[i].amount;
    }
    return total;
}

static void utxo_add_reward(const char *member_id, int amount)
{
    if (g_utxo_count >= MAX_UTXOS) {
        printf("WARNING: UTXO pool is full.\n");
        return;
    }
    snprintf(g_utxos[g_utxo_count].member_id, sizeof(g_utxos[g_utxo_count].member_id), "%s", member_id);
    g_utxos[g_utxo_count].amount = amount;
    g_utxos[g_utxo_count].spent = 0;
    g_utxo_count++;
}

static void account_credit(const char *member_id, int amount)
{
    AccountEntry *acc = find_account(member_id);
    if (!acc) {
        add_account_if_missing(member_id);
        acc = find_account(member_id);
    }
    if (!acc) return;
    acc->balance += amount;
}

static int account_debit(const char *member_id, int amount)
{
    AccountEntry *acc = find_account(member_id);
    if (!acc) return -1;
    if (acc->balance < amount) return -1;
    acc->balance -= amount;
    return 0;
}

static void record_history(const char *sender, const char *recipient, int amount, int fee, unsigned long nonce)
{
    for (int i = 0; i < g_account_count; i++) {
        if (strcmp(g_accounts[i].member_id, sender) == 0 || strcmp(g_accounts[i].member_id, recipient) == 0) {
            TxLog *node = malloc(sizeof(TxLog));
            if (!node) return;
            memset(node, 0, sizeof(TxLog));
            snprintf(node->sender, sizeof(node->sender), "%s", sender);
            snprintf(node->recipient, sizeof(node->recipient), "%s", recipient);
            node->amount = amount;
            node->fee = fee;
            node->nonce = nonce;
            node->next = g_history[i];
            g_history[i] = node;
        }
    }
}

static void apply_confirmed_reward(const Block *b)
{
    if (!b) return;

    utxo_add_reward(b->member_id, b->token_reward);
    account_credit(b->member_id, b->token_reward);

    record_history("SYSTEM", b->member_id, b->token_reward, 1, 0);
}

static void print_member_history(const char *member_id)
{
    TxLog *cur = NULL;
    int found = 0;
    for (int i = 0; i < g_account_count; i++) {
        if (strcmp(g_accounts[i].member_id, member_id) != 0) continue;
        cur = g_history[i];
        break;
    }
    printf("\n--- TRANSACTION HISTORY FOR %s ---\n", member_id);
    if (!cur) {
        printf("No transactions recorded for this member.\n");
        return;
    }
    while (cur) {
        printf("sender=%s recipient=%s amount=%d fee=%d nonce=%lu\n",
               cur->sender, cur->recipient, cur->amount, cur->fee, cur->nonce);
        cur = cur->next;
        found = 1;
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
            if (!m) continue;
            printf("  %s -> %d\n", m->member_id, utxo_balance_for_member(m->member_id));
        }
        printf("UTXO set:\n");
        if (g_utxo_count == 0) {
            printf("No unspent outputs yet.\n");
            return;
        }
        for (int i = 0; i < g_utxo_count; i++) {
            if (!g_utxos[i].spent) {
                printf("  %s -> %d\n", g_utxos[i].member_id, g_utxos[i].amount);
                total += g_utxos[i].amount;
            }
        }
        printf("Total unspent output value: %d\n", total);
        return;
    }

    printf("\n=== ACCOUNT BALANCES ===\n");
    for (int i = 0; i < g_account_count; i++) {
        printf("  %s -> balance=%d, nonce=%lu\n",
               g_accounts[i].member_id, g_accounts[i].balance, g_accounts[i].nonce);
    }
}

static void update_model_balance(const char *member_id, int reward)
{
    if (g_model == MODEL_UTXO) {
        utxo_add_reward(member_id, reward);
    } else {
        account_credit(member_id, reward);
    }
}

static int member_balance(const char *member_id)
{
    if (g_model == MODEL_UTXO) return utxo_balance_for_member(member_id);
    AccountEntry *acc = find_account(member_id);
    return acc ? acc->balance : 0;
}

static void format_today(char out[11])
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (!tm_info) {
        snprintf(out, 11, "1970-01-01");
        return;
    }
    strftime(out, 11, "%Y-%m-%d", tm_info);
}

static int parse_date(const char *text, struct tm *out)
{
    int year, month, day;
    if (!text || strlen(text) < 10) return -1;
    if (sscanf(text, "%d-%d-%d", &year, &month, &day) != 3) return -1;
    memset(out, 0, sizeof *out);
    out->tm_year = year - 1900;
    out->tm_mon = month - 1;
    out->tm_mday = day;
    out->tm_isdst = -1;
    return 0;
}

static time_t date_to_time(const char *text)
{
    struct tm tm_info;
    if (parse_date(text, &tm_info) != 0) return (time_t)-1;
    return mktime(&tm_info);
}

static void date_plus_days(char out[11], int days)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    time_t future;

    if (!tm_info) {
        snprintf(out, 11, "1970-01-01");
        return;
    }

    tm_info->tm_mday += days;
    future = mktime(tm_info);
    tm_info = localtime(&future);
    if (!tm_info) {
        snprintf(out, 11, "1970-01-01");
        return;
    }
    strftime(out, 11, "%Y-%m-%d", tm_info);
}

static Copy *find_available_copy_for_book(const char *book_id)
{
    for (int i = 0; i < registry_copy_count(); i++) {
        Copy *copy = registry_find_copy(registry_copy_at(i)->copy_id);
        if (!copy) continue;
        if (strcmp(copy->book_id, book_id) == 0 && strcmp(copy->state, "AVAILABLE") == 0)
            return copy;
    }
    return NULL;
}

static int member_active_borrow_count(const char *member_id)
{
    int count = 0;
    for (int i = 0; i < registry_copy_count(); i++) {
        const Copy *copy = registry_copy_at(i);
        if (!copy) continue;
        if (strcmp(copy->current_holder_id, member_id) == 0 && strcmp(copy->state, "BORROWED") == 0)
            count++;
    }
    return count;
}

static int member_recently_borrowed_same_book(const char *member_id, const char *book_id)
{
    for (int i = g_chain.count - 1; i >= 0; i--) {
        const Block *blk = &g_chain.blocks[i];
        if (strcmp(blk->member_id, member_id) != 0) continue;
        if (strcmp(blk->book_id, book_id) != 0) continue;
        if (strcmp(blk->action, "RETURNED") == 0) return 1;
        if (strcmp(blk->action, "BORROWED") == 0) return 0;
    }
    return 0;
}

static void append_fine(const char *member_id, int amount, const char *reason)
{
    for (int i = 0; i < g_fine_count; i++) {
        if (strcmp(g_fines[i].member_id, member_id) == 0 && !g_fines[i].settled) {
            g_fines[i].amount += amount;
            snprintf(g_fines[i].reason, sizeof g_fines[i].reason, "%s", reason);
            return;
        }
    }
    if (g_fine_count >= (int)(sizeof g_fines / sizeof g_fines[0])) return;
    snprintf(g_fines[g_fine_count].member_id, sizeof g_fines[g_fine_count].member_id, "%s", member_id);
    g_fines[g_fine_count].amount = amount;
    snprintf(g_fines[g_fine_count].reason, sizeof g_fines[g_fine_count].reason, "%s", reason);
    g_fines[g_fine_count].settled = 0;
    g_fine_count++;
}

static void queue_fraud(const char *request_id, const char *member_id, const char *reason)
{
    if (g_fraud_count >= (int)(sizeof g_fraud / sizeof g_fraud[0])) return;
    snprintf(g_fraud[g_fraud_count].request_id, sizeof g_fraud[g_fraud_count].request_id, "%s", request_id);
    snprintf(g_fraud[g_fraud_count].member_id, sizeof g_fraud[g_fraud_count].member_id, "%s", member_id);
    snprintf(g_fraud[g_fraud_count].status, sizeof g_fraud[g_fraud_count].status, "SUSPICIOUS");
    snprintf(g_fraud[g_fraud_count].reason, sizeof g_fraud[g_fraud_count].reason, "%s", reason);
    g_fraud_count++;
}

static void print_fraud_review(void)
{
    printf("\n--- FRAUD REVIEW ---\n");
    if (g_fraud_count == 0) {
        printf("No suspicious requests.\n");
        return;
    }
    for (int i = 0; i < g_fraud_count; i++) {
        printf("%s | %s | %s | %s\n", g_fraud[i].request_id, g_fraud[i].member_id, g_fraud[i].status, g_fraud[i].reason);
    }
}

static void update_copy_after_borrow(Copy *copy, const char *member_id)
{
    if (!copy) return;
    snprintf(copy->state, sizeof copy->state, "BORROWED");
    snprintf(copy->current_holder_id, sizeof copy->current_holder_id, "%s", member_id);
    date_plus_days(copy->due_date, 14);
}

static void update_copy_after_return(Copy *copy)
{
    if (!copy) return;
    snprintf(copy->state, sizeof copy->state, "AVAILABLE");
    copy->current_holder_id[0] = '\0';
    copy->due_date[0] = '\0';
    copy->renew_count = 0;
}

static void make_request_id(char out[24], const char *prefix, int index)
{
    snprintf(out, 24, "%s%04d", prefix, index + 1);
}

static PendingRequest *append_request(const char *book_id, const char *copy_id,
                                      const char *member_id, const char *action,
                                      int priority, const char *status)
{
    PendingRequest *req;

    if (g_request_count >= (int)(sizeof g_requests / sizeof g_requests[0])) return NULL;
    req = &g_requests[g_request_count];
    memset(req, 0, sizeof *req);
    make_request_id(req->request_id, "RQ", g_request_count);
    snprintf(req->book_id, sizeof req->book_id, "%s", book_id);
    snprintf(req->copy_id, sizeof req->copy_id, "%s", copy_id ? copy_id : "");
    snprintf(req->member_id, sizeof req->member_id, "%s", member_id);
    snprintf(req->action_requested, sizeof req->action_requested, "%s", action);
    req->priority = priority;
    req->queued_at = time(NULL);
    snprintf(req->status, sizeof req->status, "%s", status);
    g_request_count++;
    return req;
}

static PendingRequest *find_request(const char *request_id)
{
    for (int i = 0; i < g_request_count; i++) {
        if (strcmp(g_requests[i].request_id, request_id) == 0)
            return &g_requests[i];
    }
    return NULL;
}

static PendingRequest *next_reservation_for_book(const char *book_id)
{
    PendingRequest *best = NULL;
    for (int i = 0; i < g_request_count; i++) {
        PendingRequest *req = &g_requests[i];
        if (strcmp(req->book_id, book_id) != 0) continue;
        if (strcmp(req->action_requested, "RESERVE") != 0) continue;
        if (strcmp(req->status, "PENDING") != 0) continue;
        if (!best || req->queued_at < best->queued_at) best = req;
    }
    return best;
}

static int compare_requests(const void *a, const void *b)
{
    const PendingRequest *x = (const PendingRequest *)a;
    const PendingRequest *y = (const PendingRequest *)b;
    if (x->priority != y->priority) return x->priority - y->priority;
    if (x->queued_at < y->queued_at) return -1;
    if (x->queued_at > y->queued_at) return 1;
    return strcmp(x->request_id, y->request_id);
}

static void print_pending_requests(void)
{
    printf("\n--- PENDING POOL ---\n");
    if (g_request_count == 0) {
        printf("No requests in the pool.\n");
        return;
    }
    qsort(g_requests, (size_t)g_request_count, sizeof g_requests[0], compare_requests);
    for (int i = 0; i < g_request_count; i++) {
        printf("%s | %s | %s | copy=%s | status=%s | priority=%d\n",
               g_requests[i].request_id, g_requests[i].member_id,
               g_requests[i].action_requested, g_requests[i].copy_id,
               g_requests[i].status, g_requests[i].priority);
    }
}

static void do_borrow(void)
{
    char bid[64], mid[64];
    const Book *b; const Member *m;
    Copy *copy;
    char request_id[24];

    printf("\n--- BORROW BOOK ---\n");
    if (!read_line("Book ID   : ", bid, sizeof bid)) return;
    if (!read_line("Member ID : ", mid, sizeof mid)) return;

    b = registry_find_book(bid);
    m = registry_find_member(mid);
    if (!b || !m) { printf("ERROR: Book or Member not found\n"); return; }

    copy = find_available_copy_for_book(b->book_id);
    if (!copy) {
        printf("ERROR: no available copy exists for this title. Use reserve_book.\n");
        return;
    }

    if (member_active_borrow_count(m->member_id) >= 3) {
        make_request_id(request_id, "RQ", g_request_count);
        queue_fraud(request_id, m->member_id, "excessive concurrent borrows");
        append_request(b->book_id, copy->copy_id, m->member_id, "BORROW", 1, "SUSPICIOUS");
        printf("ERROR: request flagged as suspicious. Use fraud_review.\n");
        return;
    }
    if (member_recently_borrowed_same_book(m->member_id, b->book_id)) {
        make_request_id(request_id, "RQ", g_request_count);
        queue_fraud(request_id, m->member_id, "rapid re-borrow pattern");
        append_request(b->book_id, copy->copy_id, m->member_id, "BORROW", 1, "SUSPICIOUS");
        printf("ERROR: request flagged as suspicious. Use fraud_review.\n");
        return;
    }

    if (chain_add(&g_chain, b->book_id, b->title, m->member_id, m->full_name, "BORROWED") != 0) {
        printf("ERROR: could not record the lending event.\n");
        return;
    }
    update_copy_after_borrow(copy, m->member_id);
    append_request(b->book_id, copy->copy_id, m->member_id, "BORROW", 1, "CONFIRMED");
    persist_or_warn();
    printf("\nSUCCESS: %s (%s) borrowed \"%s\" copy %s.\n", m->full_name, m->member_id, b->title, copy->copy_id);
}

static void do_return(void)
{
    char bid[64], mid[64], cid[64], answer[16];
    const Book *b; const Member *m; const Block *last;
    Copy *copy;
    Block reward_block;
    char tx_string[256];
    char tx_hash[SHA256_HEX_LEN];
    int reward = 0;
    char today[11];
    PendingRequest *next_reservation;
    int was_late;

    printf("\n--- RETURN BOOK ---\n");
    if (!read_line("Book ID   : ", bid, sizeof bid)) return;
    if (!read_line("Copy ID   : ", cid, sizeof cid)) return;
    if (!read_line("Member ID : ", mid, sizeof mid)) return;
    if (!read_line("On time? [y/N]: ", answer, sizeof answer)) return;

    b = registry_find_book(bid); m = registry_find_member(mid);
    if (!b || !m) { printf("ERROR: Book or Member not found\n"); return; }
    copy = registry_find_copy(cid);
    if (!copy || strcmp(copy->current_holder_id, m->member_id) != 0) {
        printf("ERROR: copy not found or not held by this member.\n");
        return;
    }

    last = chain_last_for_book(&g_chain, b->book_id);
    if (!last || (strcmp(last->action, "BORROWED") != 0 && strcmp(last->action, "OVERDUE") != 0)) {
        printf("ERROR: \"%s\" (%s) is not currently on loan.\n", b->title, b->book_id);
        return;
    }

    format_today(today);
    was_late = (copy->due_date[0] != '\0' && strcmp(copy->due_date, today) < 0) ||
               !(equals_ignore_case(answer, "y") || equals_ignore_case(answer, "yes"));
    reward = was_late ? 5 : 10;
    snprintf(tx_string, sizeof tx_string, "%s|%s|%d|%ld", m->member_id, b->book_id, reward, (long)time(NULL));
    sha256_hex(tx_string, strlen(tx_string), tx_hash);

    memset(&reward_block, 0, sizeof reward_block);
    reward_block.index = 0;
    reward_block.timestamp = time(NULL);
    snprintf(reward_block.book_id, sizeof reward_block.book_id, "%s", b->book_id);
    snprintf(reward_block.book_title, sizeof reward_block.book_title, "%s", b->title);
    snprintf(reward_block.member_id, sizeof reward_block.member_id, "%s", m->member_id);
    snprintf(reward_block.member_name, sizeof reward_block.member_name, "%s", m->full_name);
    snprintf(reward_block.action, sizeof reward_block.action, "RETURNED");
    reward_block.token_reward = reward;
    reward_block.nonce = 0;
    snprintf(reward_block.transaction_id, sizeof reward_block.transaction_id, "%s", tx_hash);
    snprintf(reward_block.previous_hash, sizeof reward_block.previous_hash, "%s", g_chain.blocks[g_chain.count - 1].hash);

    if (pending_pool_add(&g_pending, &reward_block) != 0) {
        printf("ERROR: could not add return reward to pending pool.\n");
        return;
    }

    next_reservation = next_reservation_for_book(b->book_id);
    if (next_reservation) {
        snprintf(copy->state, sizeof copy->state, "RESERVED");
        snprintf(copy->current_holder_id, sizeof copy->current_holder_id, "%s", next_reservation->member_id);
        date_plus_days(copy->due_date, 14);
        snprintf(next_reservation->status, sizeof next_reservation->status, "CONFIRMED");
    } else {
        update_copy_after_return(copy);
    }

    if (was_late) {
        append_fine(m->member_id, 5, "late return");
    }

    append_request(b->book_id, copy->copy_id, m->member_id, "RETURN", 1, "CONFIRMED");

    printf("\nSUCCESS: \"%s\" copy %s returned by %s.\n", b->title, copy->copy_id, m->full_name);
    printf("Reward queued in pending pool: %d tokens (tx=%s)\n", reward, tx_hash);
    pending_pool_print(&g_pending);
}

static void do_overdue(void)
{
    char today[11];
    int flagged = 0;

    printf("\n--- CHECK OVERDUE ---\n");
    format_today(today);

    for (int i = 0; i < registry_copy_count(); i++) {
        Copy *copy = registry_find_copy(registry_copy_at(i)->copy_id);
        const Book *book;
        const Member *member;
        if (!copy || strcmp(copy->state, "BORROWED") != 0) continue;
        if (copy->due_date[0] == '\0') continue;
        if (strcmp(copy->due_date, today) >= 0) continue;

        book = registry_find_book(copy->book_id);
        member = registry_find_member(copy->current_holder_id);
        if (!book || !member) continue;

        if (chain_add(&g_chain, book->book_id, book->title, member->member_id, member->full_name, "OVERDUE") == 0) {
            append_fine(member->member_id, 1, "overdue item");
            append_request(book->book_id, copy->copy_id, member->member_id, "OVERDUE", 0, "CONFIRMED");
            flagged++;
        }
    }

    if (flagged > 0) {
        persist_or_warn();
        printf("%d overdue copy/copies flagged.\n", flagged);
    } else {
        printf("No overdue copies found.\n");
    }
}

static void do_reserve_book(void)
{
    char bid[64], mid[64];
    const Book *b;
    const Member *m;

    printf("\n--- RESERVE BOOK ---\n");
    if (!read_line("Book ID   : ", bid, sizeof bid)) return;
    if (!read_line("Member ID : ", mid, sizeof mid)) return;

    b = registry_find_book(bid);
    m = registry_find_member(mid);
    if (!b || !m) { printf("ERROR: Book or Member not found\n"); return; }

    if (g_request_count >= (int)(sizeof g_requests / sizeof g_requests[0])) {
        printf("ERROR: reservation pool full.\n");
        return;
    }

    append_request(b->book_id, "", m->member_id, "RESERVE", 1, "PENDING");
    printf("Reservation queued for %s on \"%s\".\n", m->member_id, b->title);
}

static void do_renew_book(void)
{
    char cid[64], mid[64], bid[64];
    Copy *copy;
    const Book *book;
    int renew_limit = 2;
    PendingRequest *next_reservation;

    printf("\n--- RENEW BOOK ---\n");
    if (!read_line("Copy ID   : ", cid, sizeof cid)) return;
    if (!read_line("Member ID : ", mid, sizeof mid)) return;

    copy = registry_find_copy(cid);
    if (!copy) { printf("ERROR: copy not found.\n"); return; }
    if (strcmp(copy->current_holder_id, mid) != 0) { printf("ERROR: copy not held by this member.\n"); return; }

    if (copy->renew_count >= renew_limit) {
        printf("ERROR: renewal limit reached.\n");
        return;
    }

    next_reservation = next_reservation_for_book(copy->book_id);
    if (next_reservation && strcmp(next_reservation->member_id, mid) != 0) {
        printf("ERROR: reservation exists for another member.\n");
        return;
    }

    copy->renew_count++;
    date_plus_days(copy->due_date, 14);
    book = registry_find_book(copy->book_id);
    if (book) snprintf(bid, sizeof bid, "%s", book->book_id);
    else snprintf(bid, sizeof bid, "%s", copy->book_id);

    chain_add(&g_chain, bid, book ? book->title : "", mid, registry_find_member(mid)->full_name, "RENEWED");
    append_request(copy->book_id, copy->copy_id, mid, "RENEW", 1, "CONFIRMED");
    persist_or_warn();
    printf("Renewed copy %s. New due date: %s\n", copy->copy_id, copy->due_date);
}

static void do_view_fines(void)
{
    printf("\n--- FINES ---\n");
    if (g_fine_count == 0) {
        printf("No fines recorded.\n");
        return;
    }
    for (int i = 0; i < g_fine_count; i++) {
        printf("%s | amount=%d | settled=%s | %s\n",
               g_fines[i].member_id, g_fines[i].amount,
               g_fines[i].settled ? "yes" : "no", g_fines[i].reason);
    }
}

static void do_settle_fine(void)
{
    char mid[64];
    if (!read_line("Member ID: ", mid, sizeof mid)) return;
    for (int i = 0; i < g_fine_count; i++) {
        if (strcmp(g_fines[i].member_id, mid) == 0 && !g_fines[i].settled) {
            g_fines[i].settled = 1;
            printf("Fine settled for %s.\n", mid);
            return;
        }
    }
    printf("No unsettled fine found for %s.\n", mid);
}

static void do_pool_view(void)
{
    print_pending_requests();
}

static void do_fraud_review(void)
{
    print_fraud_review();
}

static void do_approve_suspicious(void)
{
    char rid[64];
    PendingRequest *req;
    if (!read_line("Request ID: ", rid, sizeof rid)) return;
    req = find_request(rid);
    if (!req) { printf("Request not found.\n"); return; }
    snprintf(req->status, sizeof req->status, "CONFIRMED");
    for (int i = 0; i < g_fraud_count; i++) {
        if (strcmp(g_fraud[i].request_id, rid) == 0) {
            snprintf(g_fraud[i].status, sizeof g_fraud[i].status, "APPROVED");
        }
    }
    printf("Request approved.\n");
}

static void do_reject_suspicious(void)
{
    char rid[64];
    PendingRequest *req;
    if (!read_line("Request ID: ", rid, sizeof rid)) return;
    req = find_request(rid);
    if (!req) { printf("Request not found.\n"); return; }
    snprintf(req->status, sizeof req->status, "REJECTED");
    for (int i = 0; i < g_fraud_count; i++) {
        if (strcmp(g_fraud[i].request_id, rid) == 0) {
            snprintf(g_fraud[i].status, sizeof g_fraud[i].status, "REJECTED");
        }
    }
    printf("Request rejected.\n");
}

static void do_view_copies(void)
{
    registry_print_copies();
}

static void do_add_copy(void)
{
    char cid[64], bid[64], cond[32], state[32];
    printf("\n--- ADD COPY ---\n");
    if (!read_line("Copy ID   : ", cid, sizeof cid)) return;
    if (!read_line("Book ID   : ", bid, sizeof bid)) return;
    if (!read_line("Condition : ", cond, sizeof cond)) return;
    if (!read_line("State     : ", state, sizeof state)) return;
    if (registry_add_copy(cid, bid, cond, state) != 0) {
        printf("ERROR: could not add copy.\n");
        return;
    }
    printf("Copy added.\n");
}

static void do_view_copy(void)
{
    char cid[64];
    if (!read_line("\nCopy ID: ", cid, sizeof cid)) return;
    registry_print_copy(cid);
}

static void do_mark_lost(void)
{
    char cid[64], reason[80];
    Copy *copy;
    printf("\n--- MARK LOST ---\n");
    if (!read_line("Copy ID : ", cid, sizeof cid)) return;
    if (!read_line("Reason  : ", reason, sizeof reason)) return;
    copy = registry_find_copy(cid);
    if (!copy) { printf("Copy not found.\n"); return; }
    snprintf(copy->condition, sizeof copy->condition, "LOST");
    snprintf(copy->state, sizeof copy->state, "LOST");
    copy->current_holder_id[0] = '\0';
    copy->due_date[0] = '\0';
    append_request(copy->book_id, copy->copy_id, "LIBRARIAN", "LOST", 0, "CONFIRMED");
    printf("Copy marked lost. Reason: %s\n", reason);
}

static void do_validate(void)
{
    int bad = -1;
    printf("\n--- CHAIN VALIDATION ---\n");
    if (chain_validate(&g_chain, &bad)) {
        printf("CHAIN VALID: all %d block(s) verified.\n", g_chain.count);
    } else {
        printf("CHAIN INVALID: tampering detected at block #%d\n", bad);
    }
}

static void do_audit_book(void)
{
    char bid[64]; const Book *b;
    if (!read_line("\nBook ID: ", bid, sizeof bid)) return;
    b = registry_find_book(bid);
    if (!b) { printf("ERROR: Book or Member not found\n"); return; }
    printf("\nHistory for \"%s\" (%s):\n", b->title, b->book_id);
    int found = 0;
    for (int i = 0; i < g_chain.count; i++) {
        const Block *blk = &g_chain.blocks[i];
        char tbuf[64]; struct tm *tm_info;
        if (strcmp(blk->book_id, b->book_id) != 0) continue;
        tm_info = localtime(&blk->timestamp); strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", tm_info);
        printf("  #%-3d %-9s %-12s %s\n", blk->index, blk->action, blk->member_id, tbuf);
        found = 1;
    }
    if (!found) printf("  (no lending events recorded)\n");
    printf("  Current status: %s\n", chain_book_on_loan(&g_chain, b->book_id) ? "ON LOAN" : "AVAILABLE");
}

static void do_mine_pending(void)
{
    int mined = 0, attempts = 0;
    Block *snapshot = NULL;
    int pending_count;
    printf("\n--- MINE PENDING POOL ---\n");
    if (g_pending.count == 0) {
        printf("No pending rewards to mine.\n");
        return;
    }

    pending_count = g_pending.count;
    snapshot = malloc(sizeof(Block) * (size_t)pending_count);
    if (!snapshot) {
        printf("ERROR: could not snapshot pending pool.\n");
        return;
    }
    memcpy(snapshot, g_pending.items, sizeof(Block) * (size_t)pending_count);

    mined = chain_mine_pending(&g_chain, &g_pending, g_difficulty, &mined, &attempts);
    for (int i = 0; i < mined && i < pending_count; i++) {
        apply_confirmed_reward(&snapshot[i]);
    }

    free(snapshot);

    if (mined > 0) {
        persist_or_warn();
    }

    printf("Mining complete. Confirmed %d block(s). Total attempts: %d\n", mined, attempts);
    printf("Reward confirmations are being applied to member balances.\n");
    print_balances();
}

static void do_switch_model(void)
{
    char sel[16];
    printf("\n--- SWITCH TRANSACTION MODEL ---\n");
    printf("1. UTXO model\n2. Account-based model\n");
    if (!read_line("Choice > ", sel, sizeof sel)) return;
    if (strcmp(sel, "1") == 0) g_model = MODEL_UTXO;
    else if (strcmp(sel, "2") == 0) g_model = MODEL_ACCOUNT;
    else printf("Invalid selection.\n");
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
    if (registry_find_member(mid) == NULL) {
        printf("ERROR: Member not found.\n");
        return;
    }
    print_member_history(mid);
}

static void print_menu(void)
{
    printf("\n==================================================\n");
    printf("  EXTENDED LIBRARY BLOCKCHAIN TRACKER\n");
    printf("==================================================\n");
    printf("  1. Borrow a book\n");
    printf("  2. Return a book (creates pending reward)\n");
    printf("  3. View copies\n");
    printf("  4. View lending records (full ledger)\n");
    printf("  5. Validate chain integrity\n");
    printf("  6. Tamper-detection demonstration\n");
    printf("  7. Show book registry\n");
    printf("  8. Show member registry\n");
    printf("  9. Audit a single book's history\n");
    printf(" 10. Mark overdue copies\n");
    printf(" 11. Reserve a book\n");
    printf(" 12. Renew a book\n");
    printf(" 13. View pending pool\n");
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
    printf("  0. Exit\n");
    printf("--------------------------------------------------\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n##################################################\n");
    printf("#  Blockchain-Based Library Book Lending Tracker #\n");
    printf("#  Proof-of-work + token rewards + mining       #\n");
    printf("##################################################\n\n");

    if (crypto_init(KEY_DIR) != 0) {
        fprintf(stderr, "FATAL: could not initialise cryptography.\n");
        return 1;
    }

    if (registry_load_books(BOOKS_FILE) != 0) {
        fprintf(stderr, "FATAL: book registry unavailable.\n");
        crypto_cleanup();
        return 1;
    }
    if (registry_load_members(MEMBERS_FILE) != 0) {
        fprintf(stderr, "FATAL: member registry unavailable.\n");
        crypto_cleanup();
        return 1;
    }
    if (registry_load_copies("copies.txt") != 0) {
        fprintf(stderr, "FATAL: copy registry unavailable.\n");
        crypto_cleanup();
        return 1;
    }

    if (init_chain() != 0) {
        crypto_cleanup();
        return 1;
    }

    init_accounts();
    rebuild_balances_from_chain();

    for (;;) {
        char line[32]; int choice;
        print_menu();
        if (!read_line("Choice > ", line, sizeof line)) {
            printf("\nEnd of input - exiting.\n");
            break;
        }
        if (line[0] == '\0') continue;
        choice = atoi(line);

        switch (choice) {
            case 1: do_borrow(); break;
            case 2: do_return(); break;
            case 3: do_view_copies(); break;
            case 4: chain_print(&g_chain); break;
            case 5: do_validate(); break;
            case 6: chain_tamper_demo(&g_chain); break;
            case 7: registry_print_books(); break;
            case 8: registry_print_members(); break;
            case 9: do_audit_book(); break;
            case 10: do_overdue(); break;
            case 11: do_reserve_book(); break;
            case 12: do_renew_book(); break;
            case 13: do_pool_view(); break;
            case 14: do_view_fines(); break;
            case 15: do_settle_fine(); break;
            case 16: do_fraud_review(); break;
            case 17: do_approve_suspicious(); break;
            case 18: do_reject_suspicious(); break;
            case 19: do_mine_pending(); break;
            case 20: do_switch_model(); break;
            case 21: do_set_difficulty(); break;
            case 22: print_balances(); break;
            case 23: do_member_history(); break;
            case 24: do_add_copy(); break;
            case 25: do_view_copy(); break;
            case 26: do_mark_lost(); break;
            case 0:
                printf("\nSaving chain and shutting down...\n");
                persist_or_warn();
                pending_pool_free(&g_pending);
                chain_free(&g_chain);
                crypto_cleanup();
                printf("Goodbye.\n");
                return 0;
            default:
                printf("Invalid option. Please choose 0-26.\n");
        }
    }

    persist_or_warn();
    pending_pool_free(&g_pending);
    chain_free(&g_chain);
    crypto_cleanup();
    return 0;
}