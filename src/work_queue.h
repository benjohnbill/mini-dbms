/* 작업 대기열과 그 대기열을 비우는 작업 스레드들입니다(스레드 풀). 넣는
   쪽은 기다리지 않습니다: 대기열이 가득 차면 넣기가 바로 실패합니다.
   작업 스레드는 작업을 넣은 순서로 꺼내 handler(job, context)를 부릅니다.
   서버를 모르는 모듈이라 엔진 없이 단위 테스트합니다. */
#ifndef WORK_QUEUE_H
#define WORK_QUEUE_H

#include <stdbool.h>
#include <stddef.h>

struct work_queue;

/* 작업 스레드 thread_count개와 칸 capacity개의 대기열을 만듭니다.
   thread_count는 1 이상입니다. capacity가 0이면 모든 작업을 거절합니다.
   실행 중인 작업은 칸을 쓰지 않습니다. 실패하면 NULL입니다. */
struct work_queue *work_queue_create(size_t thread_count, size_t capacity,
                                     void (*handler)(void *job, void *context),
                                     void *context);

/* job을 대기열 끝에 넣습니다. job은 NULL이 아니어야 합니다. 대기열이 가득 찼으면 기다리지 않고 false를
   돌려주고, job은 부른 쪽에 남습니다. */
bool work_queue_submit(struct work_queue *queue, void *job);

/* 아직 시작하지 않은 작업을 넣은 순서로 대기열에서 모두 빼고, 하나마다
   discard(job)을 부릅니다. 실행 중인 작업은 건드리지 않습니다. 뺀 작업의
   수를 돌려줍니다. discard는 대기열의 mutex를 쥔 채로 불리므로 대기열을
   다시 부르면 안 됩니다. */
size_t work_queue_discard(struct work_queue *queue, void (*discard)(void *job));

/* 대기열에 남은 작업을 모두 실행한 뒤 작업 스레드를 끝내고 해제합니다. */
void work_queue_destroy(struct work_queue *queue);

#endif
