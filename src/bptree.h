/* int64 키를 uint64 값에 연결하는 메모리 B+ 트리입니다. 테이블을 모릅니다.
   엔진에서 키는 record의 id이고, 값은 테이블 파일 안에서 그 record가
   시작하는 offset입니다. 삭제와 중복 키는 없습니다. */
#ifndef BPTREE_H
#define BPTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 노드 하나가 담는 키의 최대 수입니다. 측정(make bench)이 16, 32, 64,
   128을 견주려고 빌드 때 -DBPTREE_NODE_KEYS=N으로 바꿉니다. */
#ifndef BPTREE_NODE_KEYS
#define BPTREE_NODE_KEYS 32
#endif

struct bptree;

/* 빈 트리를 만듭니다. 메모리가 없으면 NULL입니다. */
struct bptree *bptree_create(void);

void bptree_destroy(struct bptree *tree);

/* 다음 삽입 한 번이 쓸 노드를 미리 할당합니다. 성공하면 0, 메모리가 없으면
   -1이고, 실패해도 트리는 그대로입니다. */
int bptree_reserve(struct bptree *tree);

/* 부르기 전에 bptree_reserve가 성공해 있어야 하고, key는 트리에 없어야
   합니다. 그러면 메모리를 할당하지 않고 실패하지 않습니다. */
void bptree_insert(struct bptree *tree, int64_t key, uint64_t value);

/* key가 있으면 값을 *value에 쓰고 true입니다. */
bool bptree_find(const struct bptree *tree, int64_t key, uint64_t *value);

/* 측정용 크기입니다. node_bytes는 트리에 달린 노드의 바이트이고, 미리
   할당해 둔 노드와 트리 구조체 자신은 넣지 않습니다. */
struct bptree_stats {
    size_t height; /* 루트에서 리프까지의 노드 수 */
    size_t node_count;
    size_t node_bytes;
};

void bptree_get_stats(const struct bptree *tree, struct bptree_stats *stats);

/* 테스트 전용입니다. 모든 리프의 깊이가 같고, 리프 연결을 따라 키가
   오름차순이며, 키 수가 expected_key_count와 같으면 true입니다. */
bool bptree_is_valid(const struct bptree *tree, size_t expected_key_count);

/* 테스트 전용입니다. 모듈이 노드를 할당할 때 부르는 함수이고, 기본값은
   malloc입니다. 테스트가 바꿔 할당 횟수를 셉니다. */
extern void *(*bptree_allocate)(size_t size);

#endif
