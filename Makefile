# Ignacio-C: 세마포 기반 멀티스레드/멀티프로세스 트레이딩 엔진
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -pthread -Iinclude
LDLIBS  := -lcurl -pthread -lm
UNAME   := $(shell uname)
ifeq ($(UNAME),Darwin)
  CFLAGS += -D_DARWIN_C_SOURCE
else
  CFLAGS += -D_GNU_SOURCE
  LDLIBS += -lrt
endif

CORE := src/common.c src/sem.c src/bbuf.c src/rwlock.c src/ratelimit.c src/mock_server.c \
        src/registry.c src/book.c src/inbox.c
APP  := $(CORE) src/kis.c src/strategy.c src/exchange.c src/engine.c src/feeder.c src/main.c
TESTS := test_bbuf test_rwlock test_ratelimit test_inbox test_registry test_ipc

.PHONY: all check test tsan tsan-negative bench demo demo-norl clean

all: bin/ignacio

bin/ignacio: $(APP) include/*.h | bin
	$(CC) $(CFLAGS) -o $@ $(APP) $(LDLIBS)

bin/%: tests/%.c $(CORE) include/*.h | bin
	$(CC) $(CFLAGS) -o $@ $< $(CORE) $(LDLIBS)

bin/bench_bbuf: bench/bench_bbuf.c $(CORE) include/*.h | bin
	$(CC) $(CFLAGS) -o $@ $< $(CORE) $(LDLIBS)

bin:
	mkdir -p bin

# 한 번에 검증: 스트레스 테스트 → TSan(테스트 + 엔진 20초) → TSan 음성 대조군
check: test tsan tsan-negative
	@echo "== check 완료: 테스트/TSan/음성대조군 모두 통과"

test: $(addprefix bin/,$(TESTS))
	@for t in $(TESTS); do echo "== $$t"; ./bin/$$t || exit 1; done
	@echo "모든 테스트 통과"

# ThreadSanitizer: 데이터 경쟁 검출 (프로세스 간 테스트 test_ipc는 TSan이 fork/exec를 추적하지 못해 제외)
TSAN_TESTS := test_bbuf test_rwlock test_ratelimit test_inbox test_registry
tsan: | bin
	@for t in $(TSAN_TESTS); do \
	  $(CC) $(CFLAGS) -O1 -fsanitize=thread -o bin/$$t.tsan tests/$$t.c $(CORE) $(LDLIBS) || exit 1; \
	  echo "== $$t (TSan)"; TSAN_OPTIONS=halt_on_error=1 ./bin/$$t.tsan || exit 1; done
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o bin/ignacio.tsan $(APP) $(LDLIBS)
	TSAN_OPTIONS=halt_on_error=1 ./bin/ignacio.tsan --duration 20 --out results/tsan
	@echo "TSan: 데이터 경쟁 없음"

# 음성 대조군: bbuf에서 생산자 mutex 세마포를 제거한 빌드. TSan이 경쟁을 "잡아내면" 성공.
tsan-negative: | bin
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -DIG_BUG_NO_PMTX -o bin/test_bbuf.neg tests/test_bbuf.c $(CORE) $(LDLIBS)
	@if TSAN_OPTIONS=halt_on_error=1 ./bin/test_bbuf.neg >/dev/null 2>bin/neg.log; then \
	  echo "음성 대조군 실패: mutex를 뺐는데 TSan이 못 잡음"; exit 1; \
	else grep -m1 "SUMMARY" bin/neg.log; echo "음성 대조군 통과: 세마포 하나를 빼면 TSan이 즉시 데이터 경쟁을 보고함"; fi

bench: bin/bench_bbuf
	./bin/bench_bbuf

demo: bin/ignacio
	./bin/ignacio --mode mock --duration 60

demo-norl: bin/ignacio
	./bin/ignacio --mode mock --duration 60 --no-ratelimit

clean:
	rm -rf bin
