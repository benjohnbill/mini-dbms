#include <stdint.h>
#include <stdlib.h>

#include "acutest.h"
#include "bptree.h"

static struct bptree *tree_create_checked(void);
static void insert_reserved(struct bptree *tree, int64_t key, uint64_t value);

static void inserted_key_is_found_with_its_value_test(void)
{
    struct bptree *tree = tree_create_checked();
    uint64_t value = 0;

    insert_reserved(tree, 7, 700);
    TEST_CHECK(bptree_find(tree, 7, &value));
    TEST_CHECK(value == 700);
    bptree_destroy(tree);
}

TEST_LIST = {
    {"삽입한 키는 그 값과 함께 찾아집니다",
     inserted_key_is_found_with_its_value_test},
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
