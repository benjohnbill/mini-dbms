/* 측정 프로그램(src/bench.c)과 테스트만 쓰는 엔진 설정입니다. CLI와 API
   서버는 이 파일을 포함하지 않습니다. */
#ifndef ENGINE_BENCH_H
#define ENGINE_BENCH_H

#include <stdbool.h>

/* 참이면 planner가 WHERE id = 정수에도 SCAN을 고릅니다. 같은 ID를 INDEX와
   SCAN으로 찾아 속도를 견주려고 둡니다. 기본값은 거짓이고, 거짓일 때
   planner 규칙은 그대로입니다. 프로세스 전체에 걸리므로 다른 스레드가
   execute하는 동안 바꾸지 않습니다. */
extern bool engine_bench_force_scan;

#endif
