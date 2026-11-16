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
            -fsanitize=address,undefined -fno-sanitize-recover=all -MMD -MP
LDFLAGS  := -fsanitize=address,undefined

BUILD    := build

# main이 있는 파일입니다. 나머지 src/*.c는 DB 엔진이며, CLI와 테스트가 함께 링크합니다.
MAIN_SRCS := src/cli.c src/bench.c
LIB_SRCS  := $(filter-out $(MAIN_SRCS),$(wildcard src/*.c))
LIB_OBJS  := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)

# tests/acutest.h: https://github.com/mity/acutest (MIT),
# commit 31751b4089c93b46a9fd8a8183a695f772de66de 의 include/acutest.h 그대로입니다.
TEST_SRCS := $(wildcard tests/*_test.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/%)

# 기능 테스트는 실행 파일을 바깥에서 실행합니다. 파일마다 첫 인자로 CLI 경로를 받습니다.
FUNCTIONAL_TESTS := $(wildcard tests/functional/*_test.py)

CLI      := $(BUILD)/mini-dbms

.PHONY: all test functional verify lint format clean bench bench-smoke

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
functional: $(CLI)
	@set -e; for t in $(FUNCTIONAL_TESTS); do echo "== $$t"; python3 -B $$t $(CLI); done

verify: $(CLI) test functional bench-smoke
	python3 scripts/check-ids.py
	python3 scripts/convention.py lint

# make bench: 100만 record를 생성해 -O2 CLI로 넣고, INDEX 대 SCAN과 노드 크기를
# 잽니다. 측정용 실행 파일은 sanitizer를 끄고 -O2로 따로 만듭니다. 노드 크기마다
# -DBPTREE_NODE_KEYS=N으로 엔진을 다시 컴파일합니다(bench-N).
# bench-smoke는 같은 절차를 작은 record 수로 실행해 make verify가 빠르게 끝나게 합니다.
BENCH_DIR        := $(BUILD)/bench
BENCH_CFLAGS     := -std=c11 -Wall -Wextra -Werror -O2
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

# 코드 규칙(CONVENTION.md의 코드 절)입니다. lint는 위반을 출력하고 실패로 끝나며,
# format은 모양 위반을 고치고 바꾼 줄을 보여 줍니다.
lint:
	@python3 scripts/convention.py lint

format:
	@python3 scripts/convention.py format

clean:
	rm -rf $(BUILD)
