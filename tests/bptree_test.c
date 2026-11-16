#include <stdint.h>
#include <stdlib.h>

#include "acutest.h"
#include "bptree.h"

#define MILLION_KEYS 1000000

/* 오름차순 삽입에서 리프는 반씩 나뉘어 16키씩 남으므로, 2000키면 리프
   루트와 그 위의 내부 루트가 모두 한 번 이상 나뉩니다. */
#define ROOT_SPLIT_KEYS 2000

/* 7의 거듭제곱은 이 수와 서로소이므로 1부터 이 수까지를 한 번씩
   뒤섞인 순서로 지납니다. */
#define SHUFFLED_KEYS 100003
#define SHUFFLE_STEP 7

static size_t allocation_count;

static struct bptree *tree_create_checked(void);
static void insert_reserved(struct bptree *tree, int64_t key, uint64_t value);
static void insert_ascending(struct bptree *tree, int64_t first, int64_t last);
static bool keys_are_found(const struct bptree *tree, int64_t first,
                           int64_t last);
static void *allocate_counted(size_t size);
static void *allocate_failing(size_t size);

static void inserted_key_is_found_with_its_value_test(void)
{
    struct bptree *tree = tree_create_checked();
    uint64_t value = 0;

    insert_reserved(tree, 7, 700);
    TEST_CHECK(bptree_find(tree, 7, &value));
    TEST_CHECK(value == 700);
    bptree_destroy(tree);
}

static void key_not_inserted_is_not_found_test(void)
{
    struct bptree *tree = tree_create_checked();
    uint64_t value = 0;

    TEST_CHECK(!bptree_find(tree, 1, &value));
    insert_ascending(tree, 1, 100);
    TEST_CHECK(!bptree_find(tree, 0, &value));
    TEST_CHECK(!bptree_find(tree, 101, &value));
    TEST_CHECK(!bptree_find(tree, INT64_MIN, &value));
    TEST_CHECK(!bptree_find(tree, INT64_MAX, &value));
    bptree_destroy(tree);
}

static void all_keys_are_found_at_node_size_boundary_test(void)
{
    for (int64_t count = BPTREE_NODE_KEYS; count <= BPTREE_NODE_KEYS + 1;
         count++) {
        struct bptree *tree = tree_create_checked();

        TEST_CASE_("키 %lld개", (long long)count);
        insert_ascending(tree, 1, count);
        TEST_CHECK(keys_are_found(tree, 1, count));
        TEST_CHECK(bptree_is_valid(tree, (size_t)count));
        bptree_destroy(tree);
    }
}

static void keys_and_values_stay_after_root_splits_test(void)
{
    struct bptree *tree = tree_create_checked();
    uint64_t value = 0;

    insert_ascending(tree, 1, ROOT_SPLIT_KEYS);
    TEST_CHECK(keys_are_found(tree, 1, ROOT_SPLIT_KEYS));
    TEST_CHECK(bptree_find(tree, 1, &value));
    TEST_CHECK(value == 10);
    TEST_CHECK(bptree_find(tree, ROOT_SPLIT_KEYS, &value));
    TEST_CHECK(value == 20000);
    TEST_CHECK(bptree_is_valid(tree, ROOT_SPLIT_KEYS));
    bptree_destroy(tree);
}

static void million_ascending_keys_are_found_test(void)
{
    struct bptree *tree = tree_create_checked();
    uint64_t value = 0;

    insert_ascending(tree, 1, MILLION_KEYS);
    TEST_CHECK(keys_are_found(tree, 1, MILLION_KEYS));
    TEST_CHECK(bptree_find(tree, 500000, &value));
    TEST_CHECK(value == 5000000);
    TEST_CHECK(!bptree_find(tree, MILLION_KEYS + 1, &value));
    TEST_CHECK(bptree_is_valid(tree, MILLION_KEYS));
    bptree_destroy(tree);
}

static void shuffled_keys_are_found_in_order_test(void)
{
    struct bptree *tree = tree_create_checked();
    int64_t key = 1;

    for (int64_t i = 0; i < SHUFFLED_KEYS - 1; i++) {
        key = key * SHUFFLE_STEP % SHUFFLED_KEYS;
        insert_reserved(tree, key, (uint64_t)key * 10);
    }
    TEST_CHECK(keys_are_found(tree, 1, SHUFFLED_KEYS - 1));
    TEST_CHECK(bptree_is_valid(tree, SHUFFLED_KEYS - 1));
    bptree_destroy(tree);
}

static void insert_after_reserve_allocates_nothing_test(void)
{
    struct bptree *tree = tree_create_checked();
    size_t insert_allocations = 0;

    bptree_allocate = allocate_counted;
    for (int64_t key = 1; key <= ROOT_SPLIT_KEYS; key++) {
        TEST_ASSERT(bptree_reserve(tree) == 0);
        allocation_count = 0;
        bptree_insert(tree, key, (uint64_t)key * 10);
        insert_allocations += allocation_count;
    }
    bptree_allocate = malloc;
    TEST_CHECK(insert_allocations == 0);
    TEST_CHECK(keys_are_found(tree, 1, ROOT_SPLIT_KEYS));
    bptree_destroy(tree);
}

/* N+1번째 삽입이 루트 리프를 나누며 미리 할당한 노드를 다 쓰므로, 그 다음
   미리 할당은 새 노드를 할당해야 합니다. */
static void failed_reserve_leaves_tree_unchanged_test(void)
{
    struct bptree *tree = tree_create_checked();
    int64_t count = BPTREE_NODE_KEYS + 1;

    insert_ascending(tree, 1, count);
    bptree_allocate = allocate_failing;
    TEST_CHECK(bptree_reserve(tree) == -1);
    bptree_allocate = malloc;
    TEST_CHECK(keys_are_found(tree, 1, count));
    TEST_CHECK(bptree_is_valid(tree, (size_t)count));
    insert_reserved(tree, count + 1, (uint64_t)(count + 1) * 10);
    TEST_CHECK(keys_are_found(tree, 1, count + 1));
    bptree_destroy(tree);
}

/* 빈 트리는 루트 리프 하나이고, 키 N+1개는 리프 둘과 새 루트 하나입니다. */
static void stats_count_height_and_nodes_after_root_split_test(void)
{
    struct bptree *tree = tree_create_checked();
    struct bptree_stats empty_stats;
    struct bptree_stats split_stats;

    bptree_get_stats(tree, &empty_stats);
    TEST_CHECK(empty_stats.height == 1);
    TEST_CHECK(empty_stats.node_count == 1);
    TEST_CHECK(empty_stats.node_bytes > 0);
    insert_ascending(tree, 1, BPTREE_NODE_KEYS + 1);
    bptree_get_stats(tree, &split_stats);
    TEST_CHECK(split_stats.height == 2);
    TEST_CHECK(split_stats.node_count == 3);
    TEST_CHECK(split_stats.node_bytes == 3 * empty_stats.node_bytes);
    bptree_destroy(tree);
}

TEST_LIST = {
    {"삽입한 키는 그 값과 함께 찾아진다",
     inserted_key_is_found_with_its_value_test},
    {"넣지 않은 키는 찾아지지 않는다", key_not_inserted_is_not_found_test},
    {"[T40] 키가 노드 크기 N개와 N+1개일 때 모든 키가 찾아진다",
     all_keys_are_found_at_node_size_boundary_test},
    {"[T41] 루트가 나뉜 뒤에도 기존 키와 값이 남는다",
     keys_and_values_stay_after_root_splits_test},
    {"[T42] 오름차순 키 100만 개가 모두 제 값과 함께 찾아지고 트리 불변식이 "
     "지켜진다",
     million_ascending_keys_are_found_test},
    {"뒤섞인 순서로 넣은 키가 모두 찾아지고 리프 연결이 오름차순이다",
     shuffled_keys_are_found_in_order_test},
    {"[T43] 미리 할당이 성공한 뒤의 삽입은 메모리를 할당하지 않는다",
     insert_after_reserve_allocates_nothing_test},
    {"미리 할당이 실패해도 트리는 그대로이고 다음 삽입을 받는다",
     failed_reserve_leaves_tree_unchanged_test},
    {"[T141] 빈 트리는 높이 1, 노드 1개이고, 키 N+1개를 넣으면 높이 2, "
     "노드 3개다",
     stats_count_height_and_nodes_after_root_split_test},
    {NULL, NULL},
};

static struct bptree *tree_create_checked(void)
{
    struct bptree *tree = bptree_create();

    TEST_ASSERT(tree != NULL);
    return tree;
}

static void insert_reserved(struct bptree *tree, int64_t key, uint64_t value)
{
    TEST_ASSERT(bptree_reserve(tree) == 0);
    bptree_insert(tree, key, value);
}

/* 키 k의 값은 k * 10입니다. */
static void insert_ascending(struct bptree *tree, int64_t first, int64_t last)
{
    for (int64_t key = first; key <= last; key++) {
        insert_reserved(tree, key, (uint64_t)key * 10);
    }
}

static bool keys_are_found(const struct bptree *tree, int64_t first,
                           int64_t last)
{
    for (int64_t key = first; key <= last; key++) {
        uint64_t value = 0;

        if (!bptree_find(tree, key, &value) || value != (uint64_t)key * 10) {
            TEST_MSG("키 %lld를 값 %lld로 찾지 못함", (long long)key,
                     (long long)key * 10);
            return false;
        }
    }
    return true;
}

static void *allocate_counted(size_t size)
{
    allocation_count++;
    return malloc(size);
}

static void *allocate_failing(size_t size)
{
    (void)size;
    return NULL;
}
