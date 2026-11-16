/* field 값 하나입니다. 스키마, 파서, 테이블 파일, 실행 결과가 같이 씁니다. */
#ifndef VALUE_H
#define VALUE_H

#include <stdint.h>

enum value_type {
    VALUE_INT,
    VALUE_TEXT,
};

/* VALUE_TEXT의 text는 NUL로 끝나지 않을 수 있으므로 text_len으로 읽습니다. */
struct value {
    enum value_type type;
    int64_t integer;
    const char *text;
    uint32_t text_len;
};

#endif
