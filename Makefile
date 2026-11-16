# make verify: CI와 같은 명령입니다. 빌드, 단위 테스트, 기능 테스트, ID 검사, 코드 규칙 검사를 차례로 실행합니다.

CC       ?= cc
CPPFLAGS := -Isrc -D_POSIX_C_SOURCE=200809L
# Darwin 확장 선언은 Acutest가 사용합니다. Linux 설정은 유지합니다.
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif
# ASan과 UBSan은 메모리 오류와 정의되지 않은 동작을 테스트 중에 바로 멈춰 세웁니다.
# -MMD -MP는 .o마다 읽은 헤더 목록(.d)을 남겨, 헤더만 고쳐도 다시 빌드되게 합니다.
CFLAGS   := -std=c11 -Wall -Wextra -Werror -g -fno-omit-frame-pointer \
            -fsanitize=address,undefined -fno-sanitize-recover=all -MMD -MP \
            -pthread
LDFLAGS  := -fsanitize=address,undefined -pthread

BUILD    := build

# main이 있는 파일입니다. 나머지 src/*.c는 DB 엔진이며, CLI, 서버, 측정 프로그램, 테스트가 함께 링크합니다.
MAIN_SRCS := src/cli.c src/server.c src/bench.c
LIB_SRCS  := $(filter-out $(MAIN_SRCS),$(wildcard src/*.c))
LIB_OBJS  := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)

# tests/acutest.h: https://github.com/mity/acutest (MIT),
# commit 31751b4089c93b46a9fd8a8183a695f772de66de 의 include/acutest.h 그대로입니다.
TEST_SRCS := $(wildcard tests/*_test.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/%)

# 기능 테스트는 실행 파일을 바깥에서 실행합니다. 파일마다 첫 인자로 CLI 경로를 받습니다.
# 서버 기능 테스트는 아래 API 서버 블록이 서버 경로를 넘겨 따로 실행합니다.
FUNCTIONAL_TESTS = $(filter-out $(SERVER_FUNCTIONAL_TESTS),$(wildcard tests/functional/*_test.py))

CLI      := $(BUILD)/mini-dbms

.PHONY: all test functional verify tsan lint format clean bench bench-smoke

all: $(CLI) $(TEST_BINS)

$(CLI): src/cli.c $(LIB_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_OBJS) $(LDFLAGS) -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%_test: tests/%_test.c tests/acutest.h $(LIB_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_OBJS) $(LDFLAGS) -o $@

$(BUILD):
	mkdir -p $@

-include $(wildcard $(BUILD)/*.d)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

# -B는 tests/ 아래에 .pyc를 만들지 않게 합니다(scripts/check-ids.py가 tests/의 모든 파일을 텍스트로 읽습니다).
functional: $(CLI) functional-server
	@set -e; for t in $(FUNCTIONAL_TESTS); do echo "== $$t"; python3 -B $$t $(CLI); done

verify: $(CLI) test functional bench-smoke
	python3 scripts/check-ids.py
	python3 scripts/convention.py lint

# make bench: 100만 record를 생성해 -O2 CLI로 넣고, INDEX 대 SCAN과 노드 크기를
# 잽니다. 측정용 실행 파일은 sanitizer를 끄고 -O2로 따로 만듭니다. 노드 크기마다
# -DBPTREE_NODE_KEYS=N으로 엔진을 다시 컴파일합니다(bench-N).
# bench-smoke는 같은 절차를 작은 record 수로 실행해 make verify가 빠르게 끝나게 합니다.
BENCH_DIR        := $(BUILD)/bench
BENCH_CFLAGS     := -std=c11 -Wall -Wextra -Werror -O2 -pthread
BENCH_NODE_SIZES := 16 32 64 128
BENCH_RECORDS    ?= 1000000
BENCH_SEED       ?= 20261116
BENCH_SMOKE_RECORDS := 2000
BENCH_BINS := $(BENCH_DIR)/mini-dbms $(BENCH_DIR)/bench \
              $(BENCH_NODE_SIZES:%=$(BENCH_DIR)/bench-%)

$(BENCH_DIR)/mini-dbms: src/cli.c $(LIB_SRCS) $(wildcard src/*.h) | $(BENCH_DIR)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) src/cli.c $(LIB_SRCS) -o $@

$(BENCH_DIR)/bench: src/bench.c $(LIB_SRCS) $(wildcard src/*.h) | $(BENCH_DIR)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) src/bench.c $(LIB_SRCS) -o $@

$(BENCH_DIR)/bench-%: src/bench.c $(LIB_SRCS) $(wildcard src/*.h) | $(BENCH_DIR)
	$(CC) $(CPPFLAGS) -DBPTREE_NODE_KEYS=$* $(BENCH_CFLAGS) src/bench.c $(LIB_SRCS) -o $@

$(BENCH_DIR):
	mkdir -p $@

bench: $(BENCH_BINS)
	python3 -B scripts/bench.py $(BENCH_DIR) $(BENCH_RECORDS) $(BENCH_SEED) $(BENCH_NODE_SIZES)

bench-smoke: $(BENCH_BINS)
	@python3 -B scripts/bench.py $(BENCH_DIR) $(BENCH_SMOKE_RECORDS) $(BENCH_SEED) $(BENCH_NODE_SIZES) > /dev/null

# make tsan: ThreadSanitizer 빌드로 동시성 테스트를 돌립니다. TSan은 ASan과 함께
# 켤 수 없어 build/tsan/에 따로 빌드합니다. 경고가 하나라도 나오면 실패로 끝납니다.
TSAN_BUILD  := $(BUILD)/tsan
TSAN_CFLAGS := -std=c11 -Wall -Wextra -Werror -g -O1 -fno-omit-frame-pointer \
               -fsanitize=thread -pthread -MMD -MP
TSAN_LIB_OBJS := $(LIB_SRCS:src/%.c=$(TSAN_BUILD)/%.o)
# 잠금과 작업 대기열의 단위 테스트와, 여러 스레드가 엔진을 함께 부르는 엔진 테스트입니다.
TSAN_TESTS  := $(TSAN_BUILD)/rwlock_test $(TSAN_BUILD)/engine_test \
               $(TSAN_BUILD)/work_queue_test
TSAN_ENGINE_TESTS := T123 T124 T125 T126 T241

# Acutest는 테스트마다 자식 프로세스를 만들고 그 종료 코드를 결과로 읽습니다. TSan의
# 기본 종료 코드 66은 Acutest가 실패로 세지 않으므로, 경고가 실패(1)로 읽히게 바꿉니다.
TSAN_RUN := TSAN_OPTIONS=exitcode=1

tsan: $(TSAN_TESTS)
	$(TSAN_RUN) ./$(TSAN_BUILD)/rwlock_test
	$(TSAN_RUN) ./$(TSAN_BUILD)/engine_test $(TSAN_ENGINE_TESTS)
	$(TSAN_RUN) ./$(TSAN_BUILD)/work_queue_test

$(TSAN_BUILD)/%.o: src/%.c | $(TSAN_BUILD)
	$(CC) $(CPPFLAGS) $(TSAN_CFLAGS) -c $< -o $@

$(TSAN_BUILD)/%_test: tests/%_test.c tests/acutest.h $(TSAN_LIB_OBJS) | $(TSAN_BUILD)
	$(CC) $(CPPFLAGS) $(TSAN_CFLAGS) $< $(TSAN_LIB_OBJS) -fsanitize=thread -pthread -o $@

$(TSAN_BUILD):
	mkdir -p $@

-include $(wildcard $(TSAN_BUILD)/*.d)

# API 서버 (Phase 3) ----------------------------------------------------------
# vendor/mongoose: https://github.com/cesanta/mongoose (GPLv2, 레포 루트 LICENSE),
# tag 7.23 = commit 02bdbb9ed6f0a8f7c42228c6e7cb35d748b60551 의 mongoose.c, mongoose.h 그대로입니다.
# 서버에만 링크하고 엔진과 그 테스트에는 링크하지 않습니다. mongoose.c는 _POSIX_C_SOURCE만으로는
# alloca와 ip_mreq 선언이 숨어서 빌드되지 않으므로, 이 파일에만 _DEFAULT_SOURCE를 줍니다.
MONGOOSE_DIR   := vendor/mongoose
MONGOOSE_OBJ   := $(BUILD)/mongoose.o
MONGOOSE_FLAGS := -D_DEFAULT_SOURCE

SERVER := $(BUILD)/mini-dbms-server
# 서버 기능 테스트입니다. 테스트 대상은 서버이고, 서버 실행 파일 경로를 첫 인자로 받습니다.
SERVER_FUNCTIONAL_TESTS := tests/functional/server_test.py

.PHONY: functional-server

all: $(SERVER)

$(SERVER): src/server.c $(LIB_OBJS) $(MONGOOSE_OBJ) | $(BUILD)
	$(CC) $(CPPFLAGS) -I$(MONGOOSE_DIR) $(CFLAGS) $< $(LIB_OBJS) $(MONGOOSE_OBJ) \
	    $(LDFLAGS) -o $@

# Makefile도 의존합니다. MONGOOSE_FLAGS를 바꾸면 이전 빌드의 mongoose.o를 다시 만듭니다.
$(MONGOOSE_OBJ): $(MONGOOSE_DIR)/mongoose.c $(MONGOOSE_DIR)/mongoose.h Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(MONGOOSE_FLAGS) $(CFLAGS) -c $< -o $@

functional-server: $(SERVER)
	@set -e; for t in $(SERVER_FUNCTIONAL_TESTS); do echo "== $$t"; python3 -B $$t $(SERVER); done

# 서버를 TSan으로도 빌드해 같은 기능 테스트를 돌립니다. 작업 스레드와 이벤트 루프가
# 요청 구조체를 주고받는 경로에 경합이 있으면 테스트가 서버 출력의 경고를 보고 실패합니다.
TSAN_SERVER := $(TSAN_BUILD)/mini-dbms-server

.PHONY: tsan-server

tsan: tsan-server

tsan-server: $(TSAN_SERVER)
	@set -e; for t in $(SERVER_FUNCTIONAL_TESTS); do echo "== $$t (tsan)"; python3 -B $$t $(TSAN_SERVER); done

$(TSAN_SERVER): src/server.c $(TSAN_LIB_OBJS) $(TSAN_BUILD)/mongoose.o | $(TSAN_BUILD)
	$(CC) $(CPPFLAGS) -I$(MONGOOSE_DIR) $(TSAN_CFLAGS) $< $(TSAN_LIB_OBJS) $(TSAN_BUILD)/mongoose.o \
	    -fsanitize=thread -pthread -o $@

$(TSAN_BUILD)/mongoose.o: $(MONGOOSE_DIR)/mongoose.c $(MONGOOSE_DIR)/mongoose.h Makefile | $(TSAN_BUILD)
	$(CC) $(CPPFLAGS) $(MONGOOSE_FLAGS) $(TSAN_CFLAGS) -c $< -o $@

# 코드 규칙(CONVENTION.md의 코드 절)입니다. lint는 위반을 출력하고 실패로 끝나며,
# format은 모양 위반을 고치고 바꾼 줄을 보여 줍니다.
lint:
	@python3 scripts/convention.py lint

format:
	@python3 scripts/convention.py format

clean:
	rm -rf $(BUILD)
