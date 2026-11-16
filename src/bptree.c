/* 메모리 B+ 트리입니다. 키는 리프에만 값과 함께 있고, 내부 노드는 길을
   고르는 키만 가집니다. 리프끼리 next로 이어집니다. */
#include "bptree.h"

#include <stdlib.h>
#include <string.h>

/* 분할 직전에 노드가 잠깐 담는 키의 수입니다. 넘친 노드를 반씩 나눕니다. */
#define NODE_KEYS_OVERFLOW (BPTREE_NODE_KEYS + 1)

struct node {
    bool is_leaf;
    size_t key_count;
    int64_t keys[NODE_KEYS_OVERFLOW];
    union {
        struct node *children[NODE_KEYS_OVERFLOW + 1]; /* 내부 노드 */
        struct {
            uint64_t values[NODE_KEYS_OVERFLOW];
            struct node *next; /* 리프: 오른쪽 리프 */
        } leaf;
    };
};

/* 분할이 부모에 올리는 것입니다. right가 NULL이면 분할이 없었습니다. */
struct split {
    int64_t separator; /* right의 가장 작은 키 */
    struct node *right;
};

struct bptree {
    struct node *root;
    size_t height;      /* 루트에서 리프까지의 노드 수 */
    struct node *spare; /* 미리 할당한 노드 목록, leaf.next로 잇습니다 */
    size_t spare_count;
};

void *(*bptree_allocate)(size_t size) = malloc;

static struct node *node_allocate(bool is_leaf);
static struct node *spare_take(struct bptree *tree, bool is_leaf);
static struct split node_insert(struct bptree *tree, struct node *node,
                                int64_t key, uint64_t value);
static struct split leaf_insert(struct bptree *tree, struct node *leaf,
                                int64_t key, uint64_t value);
static struct split inner_insert(struct bptree *tree, struct node *inner,
                                 int64_t key, uint64_t value);
static size_t keys_lower_bound(const struct node *node, int64_t key);
static size_t inner_child_index(const struct node *inner, int64_t key);
static void node_destroy(struct node *node);
static const struct node *leftmost_leaf_find(const struct node *node,
                                             size_t *depth);
static bool leaf_depths_are_equal(const struct node *node, size_t depth,
                                  size_t expected_depth);
static size_t node_count_get(const struct node *node);

struct bptree *bptree_create(void)
{
    struct bptree *tree = bptree_allocate(sizeof(*tree));

    if (tree == NULL) {
        return NULL;
    }
    tree->root = node_allocate(true);
    if (tree->root == NULL) {
        free(tree);
        return NULL;
    }
    tree->height = 1;
    tree->spare = NULL;
    tree->spare_count = 0;

    return tree;
}

void bptree_destroy(struct bptree *tree)
{
    if (tree == NULL) {
        return;
    }
    node_destroy(tree->root);
    while (tree->spare != NULL) {
        struct node *next = tree->spare->leaf.next;

        free(tree->spare);
        tree->spare = next;
    }
    free(tree);
}

/* 삽입 한 번은 리프부터 루트까지 층마다 노드 하나를 나누고, 루트가 나뉘면
   새 루트 하나를 더 씁니다. 그래서 height + 1개를 채워 둡니다. */
int bptree_reserve(struct bptree *tree)
{
    while (tree->spare_count < tree->height + 1) {
        struct node *node = node_allocate(true);

        if (node == NULL) {
            return -1;
        }
        node->leaf.next = tree->spare;
        tree->spare = node;
        tree->spare_count++;
    }

    return 0;
}

void bptree_insert(struct bptree *tree, int64_t key, uint64_t value)
{
    struct split split = node_insert(tree, tree->root, key, value);
    struct node *new_root;

    if (split.right == NULL) {
        return;
    }
    new_root = spare_take(tree, false);
    new_root->key_count = 1;
    new_root->keys[0] = split.separator;
    new_root->children[0] = tree->root;
    new_root->children[1] = split.right;
    tree->root = new_root;
    tree->height++;
}

bool bptree_find(const struct bptree *tree, int64_t key, uint64_t *value)
{
    const struct node *node = tree->root;
    size_t index;

    while (!node->is_leaf) {
        node = node->children[inner_child_index(node, key)];
    }
    index = keys_lower_bound(node, key);
    if (index == node->key_count || node->keys[index] != key) {
        return false;
    }
    *value = node->leaf.values[index];

    return true;
}

bool bptree_is_valid(const struct bptree *tree, size_t expected_key_count)
{
    size_t depth = 0;
    const struct node *leaf = leftmost_leaf_find(tree->root, &depth);
    size_t key_count = 0;
    bool has_previous = false;
    int64_t previous = 0;

    if (depth != tree->height ||
        !leaf_depths_are_equal(tree->root, 1, tree->height)) {
        return false;
    }
    for (; leaf != NULL; leaf = leaf->leaf.next) {
        for (size_t i = 0; i < leaf->key_count; i++) {
            if (has_previous && leaf->keys[i] <= previous) {
                return false;
            }
            previous = leaf->keys[i];
            has_previous = true;
            key_count++;
        }
    }

    return key_count == expected_key_count;
}

void bptree_get_stats(const struct bptree *tree, struct bptree_stats *stats)
{
    stats->height = tree->height;
    stats->node_count = node_count_get(tree->root);
    stats->node_bytes = stats->node_count * sizeof(struct node);
}

static struct node *node_allocate(bool is_leaf)
{
    struct node *node = bptree_allocate(sizeof(*node));

    if (node == NULL) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->is_leaf = is_leaf;

    return node;
}

/* bptree_reserve가 채운 목록에서 꺼내므로 할당하지 않고 실패하지 않습니다. */
static struct node *spare_take(struct bptree *tree, bool is_leaf)
{
    struct node *node = tree->spare;

    tree->spare = node->leaf.next;
    tree->spare_count--;
    memset(node, 0, sizeof(*node));
    node->is_leaf = is_leaf;

    return node;
}

static struct split node_insert(struct bptree *tree, struct node *node,
                                int64_t key, uint64_t value)
{
    if (node->is_leaf) {
        return leaf_insert(tree, node, key, value);
    }
    return inner_insert(tree, node, key, value);
}

/* 키를 제자리에 끼우고, 넘치면 뒤쪽 절반을 새 리프로 옮깁니다. */
static struct split leaf_insert(struct bptree *tree, struct node *leaf,
                                int64_t key, uint64_t value)
{
    size_t index = keys_lower_bound(leaf, key);
    size_t moved = leaf->key_count - index;
    struct split split = {0, NULL};
    struct node *right;
    size_t left_count;

    memmove(&leaf->keys[index + 1], &leaf->keys[index],
            moved * sizeof(leaf->keys[0]));
    memmove(&leaf->leaf.values[index + 1], &leaf->leaf.values[index],
            moved * sizeof(leaf->leaf.values[0]));
    leaf->keys[index] = key;
    leaf->leaf.values[index] = value;
    leaf->key_count++;
    if (leaf->key_count <= BPTREE_NODE_KEYS) {
        return split;
    }

    right = spare_take(tree, true);
    left_count = leaf->key_count / 2;
    right->key_count = leaf->key_count - left_count;
    memcpy(right->keys, &leaf->keys[left_count],
           right->key_count * sizeof(right->keys[0]));
    memcpy(right->leaf.values, &leaf->leaf.values[left_count],
           right->key_count * sizeof(right->leaf.values[0]));
    leaf->key_count = left_count;
    right->leaf.next = leaf->leaf.next;
    leaf->leaf.next = right;
    split.separator = right->keys[0];
    split.right = right;

    return split;
}

/* 자식이 나뉘면 올라온 키와 자식을 끼우고, 넘치면 가운데 키를 부모로 올리고
   뒤쪽 절반을 새 내부 노드로 옮깁니다. */
static struct split inner_insert(struct bptree *tree, struct node *inner,
                                 int64_t key, uint64_t value)
{
    size_t index = inner_child_index(inner, key);
    struct split child_split =
        node_insert(tree, inner->children[index], key, value);
    struct split split = {0, NULL};
    size_t moved = inner->key_count - index;
    struct node *right;
    size_t left_count;

    if (child_split.right == NULL) {
        return split;
    }
    memmove(&inner->keys[index + 1], &inner->keys[index],
            moved * sizeof(inner->keys[0]));
    memmove(&inner->children[index + 2], &inner->children[index + 1],
            moved * sizeof(inner->children[0]));
    inner->keys[index] = child_split.separator;
    inner->children[index + 1] = child_split.right;
    inner->key_count++;
    if (inner->key_count <= BPTREE_NODE_KEYS) {
        return split;
    }

    right = spare_take(tree, false);
    left_count = inner->key_count / 2;
    right->key_count = inner->key_count - left_count - 1;
    memcpy(right->keys, &inner->keys[left_count + 1],
           right->key_count * sizeof(right->keys[0]));
    memcpy(right->children, &inner->children[left_count + 1],
           (right->key_count + 1) * sizeof(right->children[0]));
    split.separator = inner->keys[left_count];
    split.right = right;
    inner->key_count = left_count;

    return split;
}

/* key 이상인 첫 키의 자리입니다. 모든 키가 key보다 작으면 key_count입니다. */
static size_t keys_lower_bound(const struct node *node, int64_t key)
{
    size_t low = 0;
    size_t high = node->key_count;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (node->keys[middle] < key) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low;
}

/* keys[i]는 children[i + 1]의 가장 작은 키이므로, key와 같은 키는 오른쪽
   자식으로 갑니다. */
static size_t inner_child_index(const struct node *inner, int64_t key)
{
    size_t index = keys_lower_bound(inner, key);

    if (index < inner->key_count && inner->keys[index] == key) {
        index++;
    }

    return index;
}

static void node_destroy(struct node *node)
{
    if (!node->is_leaf) {
        for (size_t i = 0; i <= node->key_count; i++) {
            node_destroy(node->children[i]);
        }
    }
    free(node);
}

static const struct node *leftmost_leaf_find(const struct node *node,
                                             size_t *depth)
{
    *depth = 1;
    while (!node->is_leaf) {
        node = node->children[0];
        (*depth)++;
    }

    return node;
}

static bool leaf_depths_are_equal(const struct node *node, size_t depth,
                                  size_t expected_depth)
{
    if (node->is_leaf) {
        return depth == expected_depth;
    }
    for (size_t i = 0; i <= node->key_count; i++) {
        if (!leaf_depths_are_equal(node->children[i], depth + 1,
                                   expected_depth)) {
            return false;
        }
    }

    return true;
}

/* node와 그 아래 모든 노드의 수입니다. */
static size_t node_count_get(const struct node *node)
{
    size_t count = 1;

    if (!node->is_leaf) {
        for (size_t i = 0; i <= node->key_count; i++) {
            count += node_count_get(node->children[i]);
        }
    }

    return count;
}
