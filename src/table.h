/* 테이블 하나의 스키마 파일과 테이블 파일입니다. record를 파일 끝에
   덧붙이고, iterator로 처음부터 읽습니다. */
#ifndef TABLE_H
#define TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "value.h"

struct schema_field {
    char *name;
    enum value_type type;
};

struct table {
    char *name;
    size_t field_count; /* id를 뺀 스키마의 field 수 */
    struct schema_field *fields;
    int fd;
    uint64_t committed_size;
    int64_t last_id; /* 가장 큰 ID, 빈 테이블이면 0. 다음 ID는 이 값 + 1 */
    bool available;  /* 되돌리기가 실패하면 거짓. 재시작해야 다시 참 */
};

/* 테이블 코드가 파일을 쓰고 자를 때 부르는 함수 표입니다. 실제 값은 pwrite와
   ftruncate이고, 테스트가 바꿔 끼워 쓰기 실패와 되돌리기 실패를 만듭니다. */
struct table_file_functions {
    ssize_t (*write_at)(int fd, const void *bytes, size_t size, off_t offset);
    int (*truncate)(int fd, off_t size);
};

extern struct table_file_functions table_file_functions;

enum table_insert_status {
    TABLE_INSERT_OK,
    TABLE_INSERT_ID_EXHAUSTED,
    TABLE_INSERT_OUT_OF_MEMORY,
    TABLE_INSERT_STORAGE_ERROR, /* 쓰기가 실패했고 파일은 쓰기 전과 같습니다 */
    TABLE_INSERT_UNAVAILABLE,   /* 되돌리기도 실패해 테이블을 멈췄습니다 */
};

/* values는 스키마 순서의 field 값이고, text는 다음 next 호출 전까지만 쓸 수
   있습니다. */
struct record {
    int64_t id;
    struct value *values;
};

/* SCAN과 INDEX가 같은 모양으로 record를 내줍니다. 실행기는 둘을 구분하지
   않습니다. next는 record를 채우면 1, 끝이면 0, 읽기 오류면 -1입니다. */
struct record_iterator {
    int (*next)(struct record_iterator *iterator, struct record *record);
    void (*destroy)(struct record_iterator *iterator);
};

/* data_dir의 <name>.schema와 <name>.tbl을 엽니다. */
struct table *table_create(const char *data_dir, const char *name, char *error,
                           size_t error_size);

void table_destroy(struct table *table);

/* values를 record 하나로 덧붙이고 발급한 ID를 *inserted_id에 씁니다.
   values의 개수와 타입은 부르는 쪽이 스키마와 맞춰 둡니다. 실패하면 ID를
   쓰지 않습니다. */
enum table_insert_status table_insert(struct table *table,
                                      const struct value *values,
                                      int64_t *inserted_id, char *error,
                                      size_t error_size);

/* committed_size까지 처음부터 읽는 SCAN iterator입니다. */
struct record_iterator *table_scan_create(struct table *table);

#endif
