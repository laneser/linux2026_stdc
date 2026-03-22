/*
 * test_cache_exp.c - Unity tests for cache_exp functions
 *
 * Verifies correctness of:
 *   - middle_fast_slow(): finds the correct middle node
 *   - middle_single(): finds the correct middle node
 *   - build_list(): produces valid linked lists (sequential and shuffled)
 *   - Both algorithms agree on the same result for all list sizes
 */

#include "unity.h"
#include <stdlib.h>
#include <string.h>

/* --- Import functions from cache_exp.c --- */

struct list_node {
    int val;
    struct list_node *next;
};

extern struct list_node *middle_fast_slow(struct list_node *head);
extern struct list_node *middle_single(struct list_node *head);
extern struct list_node *build_list(int n, int do_shuffle,
                                    struct list_node **pool_out);
extern void free_list(void *pool, int n, int was_shuffled);

/* --- Helpers --- */

/* Build a simple sequential list from a stack-allocated array */
static struct list_node *make_list(struct list_node *buf, int n)
{
    for (int i = 0; i < n - 1; i++) {
        buf[i].val = i;
        buf[i].next = &buf[i + 1];
    }
    buf[n - 1].val = n - 1;
    buf[n - 1].next = NULL;
    return &buf[0];
}

/* Count the number of nodes in a list */
static int list_length(struct list_node *head)
{
    int n = 0;
    while (head) {
        n++;
        head = head->next;
    }
    return n;
}

/* Walk to the k-th node (0-indexed) */
static struct list_node *list_nth(struct list_node *head, int k)
{
    for (int i = 0; i < k; i++)
        head = head->next;
    return head;
}

void setUp(void) {}
void tearDown(void) {}

/* --- Tests: middle_fast_slow --- */

void test_fast_slow_single_node(void)
{
    struct list_node buf[1];
    struct list_node *head = make_list(buf, 1);
    TEST_ASSERT_EQUAL_PTR(head, middle_fast_slow(head));
}

void test_fast_slow_two_nodes(void)
{
    struct list_node buf[2];
    struct list_node *head = make_list(buf, 2);
    /* For 2 nodes [0]->[1], middle is node 0 (slow doesn't advance
     * because fast->next->next would be NULL) */
    TEST_ASSERT_EQUAL_PTR(&buf[1], middle_fast_slow(head));
}

void test_fast_slow_odd(void)
{
    struct list_node buf[9];
    struct list_node *head = make_list(buf, 9);
    /* 9 nodes: middle is index 4 (0-indexed) */
    struct list_node *mid = middle_fast_slow(head);
    TEST_ASSERT_EQUAL_PTR(list_nth(head, 4), mid);
}

void test_fast_slow_even(void)
{
    struct list_node buf[10];
    struct list_node *head = make_list(buf, 10);
    /* 10 nodes: fast stops at node 9 (last), slow is at index 5 */
    struct list_node *mid = middle_fast_slow(head);
    TEST_ASSERT_EQUAL_PTR(list_nth(head, 5), mid);
}

void test_fast_slow_large(void)
{
    struct list_node *pool;
    struct list_node *head = build_list(10000, 0, &pool);
    struct list_node *mid = middle_fast_slow(head);
    TEST_ASSERT_EQUAL_PTR(list_nth(head, 5000), mid);
    free_list(pool, 10000, 0);
}

/* --- Tests: middle_single --- */

void test_single_single_node(void)
{
    struct list_node buf[1];
    struct list_node *head = make_list(buf, 1);
    TEST_ASSERT_EQUAL_PTR(head, middle_single(head));
}

void test_single_two_nodes(void)
{
    struct list_node buf[2];
    struct list_node *head = make_list(buf, 2);
    TEST_ASSERT_EQUAL_PTR(&buf[1], middle_single(head));
}

void test_single_odd(void)
{
    struct list_node buf[9];
    struct list_node *head = make_list(buf, 9);
    struct list_node *mid = middle_single(head);
    TEST_ASSERT_EQUAL_PTR(list_nth(head, 4), mid);
}

void test_single_even(void)
{
    struct list_node buf[10];
    struct list_node *head = make_list(buf, 10);
    struct list_node *mid = middle_single(head);
    TEST_ASSERT_EQUAL_PTR(list_nth(head, 5), mid);
}

void test_single_large(void)
{
    struct list_node *pool;
    struct list_node *head = build_list(10000, 0, &pool);
    struct list_node *mid = middle_single(head);
    TEST_ASSERT_EQUAL_PTR(list_nth(head, 5000), mid);
    free_list(pool, 10000, 0);
}

/* --- Tests: both algorithms agree --- */

void test_agree_various_sizes(void)
{
    int sizes[] = {1, 2, 3, 4, 5, 7, 10, 15, 16, 100, 255, 256, 1000, 9999};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < nsizes; i++) {
        struct list_node *pool;
        struct list_node *head = build_list(sizes[i], 0, &pool);
        struct list_node *m1 = middle_fast_slow(head);
        struct list_node *m2 = middle_single(head);

        char msg[64];
        snprintf(msg, sizeof(msg), "size=%d", sizes[i]);
        TEST_ASSERT_EQUAL_PTR_MESSAGE(m1, m2, msg);
        free_list(pool, sizes[i], 0);
    }
}

void test_agree_shuffled(void)
{
    srand(42);
    int sizes[] = {10, 100, 1000, 5000};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < nsizes; i++) {
        struct list_node *pool;
        struct list_node *head = build_list(sizes[i], 1, &pool);

        /* Verify list length is correct */
        TEST_ASSERT_EQUAL_INT(sizes[i], list_length(head));

        /* Verify both algorithms agree */
        struct list_node *m1 = middle_fast_slow(head);
        struct list_node *m2 = middle_single(head);

        char msg[64];
        snprintf(msg, sizeof(msg), "shuffled size=%d", sizes[i]);
        TEST_ASSERT_EQUAL_PTR_MESSAGE(m1, m2, msg);
        free_list(pool, sizes[i], 1);
    }
}

/* --- Tests: build_list correctness --- */

void test_build_sequential_length(void)
{
    struct list_node *pool;
    struct list_node *head = build_list(500, 0, &pool);
    TEST_ASSERT_EQUAL_INT(500, list_length(head));
    free_list(pool, 500, 0);
}

void test_build_sequential_order(void)
{
    struct list_node *pool;
    struct list_node *head = build_list(100, 0, &pool);

    /* Sequential list should have val 0, 1, 2, ... */
    struct list_node *cur = head;
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_NOT_NULL(cur);
        TEST_ASSERT_EQUAL_INT(i, cur->val);
        cur = cur->next;
    }
    TEST_ASSERT_NULL(cur);
    free_list(pool, 100, 0);
}

void test_build_shuffled_length(void)
{
    srand(42);
    struct list_node *pool;
    struct list_node *head = build_list(500, 1, &pool);
    TEST_ASSERT_EQUAL_INT(500, list_length(head));
    free_list(pool, 500, 1);
}

void test_build_shuffled_all_values_present(void)
{
    srand(42);
    int n = 200;
    struct list_node *pool;
    struct list_node *head = build_list(n, 1, &pool);

    /* Every value 0..n-1 should appear exactly once */
    int *seen = calloc(n, sizeof(int));
    struct list_node *cur = head;
    while (cur) {
        TEST_ASSERT_TRUE_MESSAGE(cur->val >= 0 && cur->val < n,
                                 "val out of range");
        seen[cur->val]++;
        cur = cur->next;
    }
    for (int i = 0; i < n; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "val %d seen %d times", i, seen[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen[i], msg);
    }
    free(seen);
    free_list(pool, n, 1);
}

void test_build_shuffled_not_sequential(void)
{
    srand(42);
    int n = 100;
    struct list_node *pool;
    struct list_node *head = build_list(n, 1, &pool);

    /* The shuffled list should NOT have all values in order 0,1,2,... */
    int sequential_count = 0;
    struct list_node *cur = head;
    int expected = 0;
    while (cur) {
        if (cur->val == expected)
            sequential_count++;
        expected++;
        cur = cur->next;
    }
    /* It's astronomically unlikely that a shuffled list is fully sequential */
    TEST_ASSERT_TRUE_MESSAGE(sequential_count < n,
                             "shuffled list appears fully sequential");
    free_list(pool, n, 1);
}

/* --- Main --- */

int main(void)
{
    UNITY_BEGIN();

    /* middle_fast_slow */
    RUN_TEST(test_fast_slow_single_node);
    RUN_TEST(test_fast_slow_two_nodes);
    RUN_TEST(test_fast_slow_odd);
    RUN_TEST(test_fast_slow_even);
    RUN_TEST(test_fast_slow_large);

    /* middle_single */
    RUN_TEST(test_single_single_node);
    RUN_TEST(test_single_two_nodes);
    RUN_TEST(test_single_odd);
    RUN_TEST(test_single_even);
    RUN_TEST(test_single_large);

    /* Agreement */
    RUN_TEST(test_agree_various_sizes);
    RUN_TEST(test_agree_shuffled);

    /* build_list */
    RUN_TEST(test_build_sequential_length);
    RUN_TEST(test_build_sequential_order);
    RUN_TEST(test_build_shuffled_length);
    RUN_TEST(test_build_shuffled_all_values_present);
    RUN_TEST(test_build_shuffled_not_sequential);

    return UNITY_END();
}
