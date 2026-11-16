/* mini-dbms CLI의 진입점입니다. SQL 파일 경로 하나를 받습니다.
   SQL 처리는 아직 없습니다. 처리 코드는 이 파일 밖(DB 엔진)에 두고 여기서 부릅니다. */
#include <stdio.h>

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <sql-file>\n", argv[0]);
        return 2;
    }

    return 0;
}
