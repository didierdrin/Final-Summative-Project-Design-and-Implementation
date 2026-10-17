/* ============================================================
 *  File: include/registry.h
 * ============================================================ */
#ifndef REGISTRY_H
#define REGISTRY_H

#define MAX_BOOKS   256
#define MAX_MEMBERS 256
#define MAX_COPIES  512

typedef enum {
    COPY_NEW = 0,
    COPY_GOOD,
    COPY_WORN,
    COPY_LOST
} CopyCondition;

typedef enum {
    COPY_AVAILABLE = 0,
    COPY_BORROWED,
    COPY_RESERVED,
    COPY_LOST_STATE
} CopyState;

/* ---- Book registry ---------------------------------------- */
typedef struct {
    char book_id[20];
    char title[80];
    char author[50];
} Book;

/* ---- Member registry -------------------------------------- */
typedef struct {
    char member_id[20];
    char full_name[50];
    char course_code[10];
} Member;

typedef struct {
    char copy_id[24];
    char book_id[20];
    char condition[8];
    char state[12];
    char current_holder_id[20];
    char due_date[11];
    int  renew_count;
} Copy;

int  registry_load_books(const char *path);
int  registry_load_members(const char *path);
int  registry_load_copies(const char *path);
int  registry_add_copy(const char *copy_id, const char *book_id,
                       const char *condition, const char *state);

const Book   *registry_find_book(const char *book_id);
const Book   *registry_book_at(int index);
const Member *registry_find_member(const char *member_id);
const Member *registry_member_at(int index);
Copy         *registry_find_copy(const char *copy_id);
const Copy   *registry_copy_at(int index);

int  registry_book_count(void);
int  registry_member_count(void);
int  registry_copy_count(void);

void registry_print_books(void);
void registry_print_members(void);
void registry_print_copies(void);
void registry_print_copy(const char *copy_id);

#endif /* REGISTRY_H */