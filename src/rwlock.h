/* 쓰기 우선 잠금입니다. 여러 읽기는 함께 들어가고, 쓰기는 혼자
   들어갑니다. 쓰기가 기다리기 시작하면 새 읽기는 그 쓰기가 끝날 때까지
   기다립니다. glibc 전용 설정 없이 mutex 하나와 조건 변수 두 개로 만들어
   macOS와 Linux에서 같게 동작합니다. */
#ifndef RWLOCK_H
#define RWLOCK_H

#include <pthread.h>
#include <stdbool.h>

enum rwlock_mode {
    RWLOCK_READ,
    RWLOCK_WRITE,
};

struct rwlock {
    pthread_mutex_t mutex;
    pthread_cond_t readers_may_enter;
    pthread_cond_t writer_may_enter;
    int active_readers;
    int waiting_writers;
    bool writing;
    /* NULL이 아니면 스레드가 기다리기 직전에 mutex를 쥔 채로 부릅니다.
       테스트가 시간에 기대지 않고 "기다리기 시작했다"를 알려고 씁니다. */
    void (*wait_hook)(void *context, enum rwlock_mode mode);
    void *wait_context;
};

/* 성공하면 0, pthread 초기화가 실패하면 -1입니다. */
int rwlock_init(struct rwlock *lock);
void rwlock_destroy(struct rwlock *lock);

void rwlock_read_lock(struct rwlock *lock);
void rwlock_read_unlock(struct rwlock *lock);
void rwlock_write_lock(struct rwlock *lock);
void rwlock_write_unlock(struct rwlock *lock);

#endif
