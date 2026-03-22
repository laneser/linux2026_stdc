/*
 * cache_exp.c - Compare cache behavior of fast-slow vs single pointer
 *
 * This experiment measures the cache performance difference between two
 * algorithms for finding the middle node of a singly linked list:
 *
 *   1. Fast-slow pointer: fast advances 2 steps, slow advances 1 step
 *      in the same loop. The slow pointer accesses nodes recently visited
 *      by the fast pointer (good temporal locality).
 *
 *   2. Single pointer (two-pass): first pass counts all nodes, second
 *      pass walks to the middle. The second pass accesses nodes whose
 *      cache lines may have been evicted during the first pass.
 *
 * Reference: https://hackmd.io/@sysprog/ry8NwAMvT
 *
 * Usage: ./cache_exp <size> <mode> <algo> <iters>
 *   size:  number of nodes (e.g., 10000, 1000000)
 *   mode:  seq   - sequential links (good spatial locality)
 *          shuf  - shuffled links (poor spatial locality, realistic)
 *   algo:  fast   - fast-slow pointer
 *          single - single pointer (two-pass)
 *          both   - run both and print timing comparison
 *   iters: number of iterations (use 1 for perf stat, more for timing)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct list_node {
    int val;
    struct list_node *next;
};

#ifndef TEST_BUILD
/* volatile on the pointer itself: writes to sink can't be optimized away */
static struct list_node *volatile sink;
#endif

/*
 * Fast-slow pointer: find middle node.
 *
 * Access pattern (list of 9 nodes):
 *   Iteration 1: fast reads [1],[2]; slow reads [1]
 *   Iteration 2: fast reads [3],[4]; slow reads [2]
 *   ...
 * Slow pointer accesses nodes that fast pointer just touched,
 * so those cache lines are likely still warm.
 */
__attribute__((noinline))
struct list_node *middle_fast_slow(struct list_node *head)
{
    struct list_node *slow = head, *fast = head;

    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
    }
    return slow;
}

/*
 * Single pointer (two-pass): find middle node.
 *
 * First pass: traverse entire list to count n nodes.
 * Second pass: traverse first n/2 nodes to reach middle.
 *
 * The second pass accesses nodes whose cache lines may have been
 * evicted by the first pass traversing the second half of the list.
 */
__attribute__((noinline))
struct list_node *middle_single(struct list_node *head)
{
    struct list_node *cur = head;
    int n = 0;

    while (cur) {
        ++n;
        cur = cur->next;
    }

    int k = 0;
    cur = head;
    while (k < n / 2) {
        ++k;
        cur = cur->next;
    }
    return cur;
}

/* Fisher-Yates shuffle for pointer array */
static void shuffle_ptrs(struct list_node **arr, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        struct list_node *tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/*
 * Build a linked list of n nodes.
 *
 *   sequential: dense packing (calloc), linked in order 0 -> 1 -> ... -> n-1.
 *     Good spatial locality; prefetcher can predict access pattern.
 *
 *   shuffled: each node individually malloc'd with random-sized spacers
 *     between allocations, then spacers freed.  Nodes end up scattered
 *     across the heap — different cache lines, different pages — just
 *     like real-world linked lists.  Linked in shuffled order.
 *
 * Returns the head node.
 * *pool_out is set to opaque handle; caller must use free_list() to release.
 */
struct list_node *build_list(int n, int do_shuffle,
                             struct list_node **pool_out)
{
    if (!do_shuffle) {
        struct list_node *pool = calloc(n, sizeof(struct list_node));

        if (!pool) {
            perror("calloc");
            exit(1);
        }
        *pool_out = pool;
        for (int i = 0; i < n - 1; i++) {
            pool[i].val = i;
            pool[i].next = &pool[i + 1];
        }
        pool[n - 1].val = n - 1;
        pool[n - 1].next = NULL;
        return &pool[0];
    }

    /* Scatter allocation: malloc each node individually with random
     * spacers so nodes land on different cache lines and pages. */
    struct list_node **ptrs = malloc(n * sizeof(struct list_node *));

    if (!ptrs) {
        perror("malloc ptrs");
        exit(1);
    }

    /* Allocate all spacers and nodes together.  Spacers must stay alive
     * during the entire allocation phase; otherwise malloc reuses the
     * just-freed space and nodes end up adjacent. */
    void **spacers = malloc(n * sizeof(void *));

    if (!spacers) {
        perror("malloc spacers");
        exit(1);
    }
    for (int i = 0; i < n; i++) {
        spacers[i] = malloc(64 + (rand() % 4096));
        ptrs[i] = malloc(sizeof(struct list_node));
        if (!ptrs[i]) {
            perror("malloc node");
            exit(1);
        }
    }
    /* Free spacers now; nodes remain at their scattered addresses */
    for (int i = 0; i < n; i++)
        free(spacers[i]);
    free(spacers);

    /* Assign vals by allocation order, then shuffle */
    for (int i = 0; i < n; i++)
        ptrs[i]->val = i;

    shuffle_ptrs(ptrs, n);

    /* Link in shuffled order; vals are now in random sequence */
    for (int i = 0; i < n - 1; i++)
        ptrs[i]->next = ptrs[i + 1];
    ptrs[n - 1]->next = NULL;

    struct list_node *head = ptrs[0];

    /* Store ptrs array as opaque handle for free_list() */
    *pool_out = (struct list_node *)ptrs;
    return head;
}

/*
 * Free a list built by build_list().
 * For sequential: pool is a single calloc block.
 * For shuffled: pool is a pointer array; each node was malloc'd individually.
 */
void free_list(void *pool, int n, int was_shuffled)
{
    if (!was_shuffled) {
        free(pool);
        return;
    }
    struct list_node **ptrs = (struct list_node **)pool;

    for (int i = 0; i < n; i++)
        free(ptrs[i]);
    free(ptrs);
}

#ifndef TEST_BUILD

static double timespec_diff_sec(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) +
           (end->tv_nsec - start->tv_nsec) / 1e9;
}

static void run_algo(struct list_node *head,
                     struct list_node *(*fn)(struct list_node *), int iters)
{
    for (int i = 0; i < iters; i++)
        sink = fn(head);
}

int main(int argc, char *argv[])
{
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s <size> <seq|shuf> <fast|single> <iters>\n"
                "\n"
                "Examples:\n"
                "  %s 100000 shuf fast 100\n"
                "  perf stat -e cache-references,cache-misses,cycles,instructions \\\n"
                "    %s 1000000 shuf fast 100\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }

    int size = atoi(argv[1]);
    int do_shuffle = (strcmp(argv[2], "shuf") == 0);
    const char *algo = argv[3];
    int iters = atoi(argv[4]);

    if (size <= 0 || iters <= 0) {
        fprintf(stderr, "Error: size and iters must be positive\n");
        return 1;
    }

    srand(42); /* fixed seed for reproducibility */

    fprintf(stderr, "Building list: %d nodes, %s...\n",
            size, do_shuffle ? "shuffled" : "sequential");

    struct list_node *pool;
    struct list_node *head = build_list(size, do_shuffle, &pool);

    fprintf(stderr, "Node size: %zu bytes, L1 cache line: %ld bytes, mode: %s\n",
            sizeof(struct list_node),
            sysconf(_SC_LEVEL1_DCACHE_LINESIZE),
            do_shuffle ? "scattered (individual malloc)" : "dense (calloc)");

    struct list_node *(*fn)(struct list_node *);

    if (strcmp(algo, "fast") == 0)
        fn = middle_fast_slow;
    else if (strcmp(algo, "single") == 0)
        fn = middle_single;
    else {
        fprintf(stderr, "Unknown algo: %s (use 'fast' or 'single')\n", algo);
        free_list(pool, size, do_shuffle);
        return 1;
    }

    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    run_algo(head, fn, iters);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double sec = timespec_diff_sec(&t0, &t1);

    printf("%s: %.4f sec (%d iters, %.2f ns/iter)\n",
           algo, sec, iters, sec / iters * 1e9);

    free_list(pool, size, do_shuffle);
    return 0;
}

#endif /* TEST_BUILD */
