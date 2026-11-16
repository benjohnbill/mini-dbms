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

# main이 있는 파일입니다. 나머지 src/*.c는 DB 엔진이며, CLI와 테스트가 함께 링크합니다.
MAIN_SRCS := src/cli.c
LIB_SRCS  := $(filter-out $(MAIN_SRCS),$(wildcard src/*.c))
LIB_OBJS  := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)

# tests/acutest.h: https://github.com/mity/acutest (MIT),
# commit 31751b4089c93b46a9fd8a8183a695f772de66de 의 include/acutest.h 그대로입니다.
TEST_SRCS := $(wildcard tests/*_test.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/%)

# 기능 테스트는 실행 파일을 바깥에서 실행합니다. 파일마다 첫 인자로 CLI 경로를 받습니다.
FUNCTIONAL_TESTS := $(wildcard tests/functional/*_test.py)

CLI      := $(BUILD)/mini-dbms

.PHONY: all test functional verify lint format clean

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

verify: $(CLI) test functional
	python3 scripts/check-ids.py
	python3 scripts/convention.py lint

# 코드 규칙(CONVENTION.md의 코드 절)입니다. lint는 위반을 출력하고 실패로 끝나며,
# format은 모양 위반을 고치고 바꾼 줄을 보여 줍니다.
lint:
	@python3 scripts/convention.py lint

format:
	@python3 scripts/convention.py format

clean:
	rm -rf $(BUILD)
