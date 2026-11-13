# Ignacio-C — 세마포로 동기화한 멀티스레드·멀티프로세스 트레이딩 엔진

KOSPI200 선물 자동매매 엔진을 **C11 + pthread + POSIX named 세마포 + 공유메모리**로 구현한 프로젝트입니다.
스레드와 프로세스 사이의 **모든 동기화를 세마포(P/V)로 표현**했고, 각 세마포가 왜 필요한지, 초기값이 왜 그 값인지, 교착과 경쟁이 왜 생기지 않는지를 [docs/DESIGN.md](docs/DESIGN.md)에 정리했습니다.

이 프로젝트의 주제는 전략 수익이 아니라 **동시성 구조와 그 정확성 검증**입니다.

## 구조 한눈에 보기

- **프로세스 2개**: 시세 수집 feeder ↔ 엔진. 공유메모리 링버퍼와 named 세마포로 연결하고, feeder가 죽으면 엔진이 감지해서 자동으로 재기동합니다.
- **엔진 스레드 18개**: 전략 ×8, 주문 게이트웨이 워커 ×4, 시세 분배, 잔고 대조, 시계, permit 리필, 모의 거래소, supervisor
- **세마포 132개** (프로세스 간 4 + 내부 128): 유한 버퍼, 우선순위 우편함, Readers-Writers 락, 슬라이딩 윈도 레이트 리미터, 리스크 한도 예약, in-flight 한도
- **실제 한국투자증권(KIS) 모의투자 API**로 시세와 주문을 처리할 수 있습니다 (`--mode kis-dry` / `kis-live`).

| 동기화 문제 | 세마포로 푼 방법 | 파일 |
|---|---|---|
| 생산자-소비자 | 유한 버퍼 (MPMC는 생산자·소비자 mutex 분리, 프로세스 간은 SPSC 공유메모리 링) | [src/bbuf.c](src/bbuf.c) |
| 우선순위 메시지 | 큐 3개 + 카운팅 세마포 1개로 만든 우편함 | [src/inbox.c](src/inbox.c) |
| 장부 읽기/쓰기 | writer가 굶지 않는 Readers-Writers 락 (*Little Book of Semaphores* 4.2.6) | [src/rwlock.c](src/rwlock.c) |
| 브로커 초당 한도 | "응답 + 1초 뒤 반납" 카운팅 세마포 → 슬라이딩 1초 한도, 프로세스 간 공유 | [src/ratelimit.c](src/ratelimit.c) |
| 계좌 롱 한도 | 읽고 판단하지 않고 `trywait`로 **원자적 예약** | [src/strategy.c](src/strategy.c) |
| 주문 등록 전 체결 도착 | 주문번호 표에서 park 후 등록 시 해소 | [src/registry.c](src/registry.c) |

macOS에는 이름 없는 세마포(`sem_init`)가 없어서, 모든 세마포를 `sem_open` named 세마포로 만들었습니다. 내부용은 생성 직후 unlink하고, 프로세스 간용은 이름으로 엽니다 ([src/sem.c](src/sem.c)).

## 검증: `make check`

```bash
make check   # 스트레스 테스트 6개 → ThreadSanitizer(테스트 + 엔진 20초) → TSan 음성 대조군
```

1. **스트레스 테스트 6개** ([tests/](tests/)): 유한 버퍼, RW 락, 레이트 리미터, 우편함, 레지스트리, 프로세스 간 IPC
2. **ThreadSanitizer**: 테스트와 엔진 전체(20초)를 TSan 빌드로 돌려 데이터 경쟁이 없음을 확인합니다. TSan은 세마포의 happens-before 관계를 모르기 때문에 주석으로 알려 줍니다 (DESIGN.md §7).
3. **음성 대조군**: 유한 버퍼에서 생산자 mutex 세마포 **하나만 뺀** 빌드를 TSan으로 돌립니다. TSan이 경쟁을 잡아내야 통과입니다. 검사 도구가 실제로 버그를 잡을 수 있다는 증거입니다.

```
SUMMARY: ThreadSanitizer: data race bbuf.c:95 in bbuf_put
음성 대조군 통과: 세마포 하나를 빼면 TSan이 즉시 데이터 경쟁을 보고함
== check 완료: 테스트/TSan/음성대조군 모두 통과
```

엔진은 실행이 끝날 때마다 **동기화 불변식**을 스스로 검사합니다.

```
[동기화 불변식 검증]
  OK   exposure permit 보존: 남은 3 + 보유 0 = 초기값 3
  OK   전략별 inflight permit 전부 반납 (각 2)
  OK   ACK 큐 overflow = 0 (용량 논증: ACK_CAP 64 ≥ 2×inflight 2)
  OK   체결 유실 없음: 장부 반영 90 = 전략 전달 90
  OK   registry 경쟁 경로: 등록 전 도착한 체결 1건 park → 미해결 0건
  OK   거래소 원장 순포지션 0 = 내부 장부 0
  OK   장마감 전량청산 후 순포지션 = 0
  OK   레이트리미터 ON → 서버 EGW00201 = 0
```

## 실행 결과 (`make demo`, Apple M4, 모의 시장 60초)

```
[주문] 전송 91 | 접수 90 | 체결 90 | 거절 1 (그중 EGW00201 0)
[지연시간]
  시세: feeder→전략 (IPC 포함)  p50=88us   p99=195us
  주문: 전략→워커 큐 대기        p50=33us   p99=76us
  시세→접수 ACK (end-to-end)    p50=27.5ms p99=41.4ms   ← 대부분 브로커 왕복 시간
```

**레이트 리미터의 효과**: 같은 조건에서 레이트 리미터를 끄면(`make demo-norl`) 주문 303건 중 153건이 거절됩니다 (초당 한도 초과 EGW00201 151건, 서버 거절 합계 278건). 거절당한 전략이 다시 주문을 내면서 요청이 불어나는 **재시도 폭주**가 그대로 재현됩니다.

**장애 복구**: 실행 중 다른 터미널에서 feeder를 강제 종료해도 엔진이 "종료 감지 → 재기동" 후 매매를 이어 갑니다.

```bash
kill -9 $(pgrep -f "ignacio --role=feeder")
```

## 빌드와 실행

필요한 것: C11 컴파일러, libcurl. macOS(Apple Silicon)에서 개발·검증했습니다.

```bash
make            # bin/ignacio
make demo       # 모의 시장 60초 (레이트 리미터 ON)
make demo-norl  # 같은 조건, 레이트 리미터 OFF
make check      # 테스트 + TSan + 음성 대조군
make bench      # 유한 버퍼 처리량, 게이트웨이 워커 확장성
./bin/ignacio --help
```

mock 모드는 외부 연결 없이 돌아갑니다. KIS 모의투자 API를 쓰려면 `.env.example`을 `.env`로 복사해 키와 계좌번호를 채우세요.

```bash
cp .env.example .env
./bin/ignacio --mode kis-dry  --inst A01612   # 실시세 + 모의 체결 (주문은 나가지 않음)
./bin/ignacio --mode kis-live --inst A01612   # 모의투자 계좌로 실제 주문 (장중에만)
```

## 한계

- **kis-live 체결 확인**: KIS 실시간 체결통보(WebSocket)는 연동하지 않았습니다. 주문 접수에 성공하면 주문가로 체결된 것으로 간주합니다. 그래서 순포지션 대조는 mock 모드에서만 합니다.
- **처리량 상한**: macOS named 세마포는 연산마다 커널 호출이라, 큐 하나의 처리량이 초당 약 40~70만 건에서 포화합니다. 이 시스템의 병목은 브로커 왕복(수십 ms)과 초당 한도라서 문제되지 않지만, μs 단위 HFT 구조는 아닙니다.
- **Linux**: Makefile에 Linux 분기(`-D_GNU_SOURCE -lrt`)는 있지만 직접 빌드해 보지는 않았습니다.
- **종목코드**: KOSPI200 선물 종목코드는 만기마다 바꿔야 합니다.
- 모의 시장의 손익은 랜덤워크 결과라 의미가 없습니다.

전체 설계 논증, 세마포별 초기값 근거, 교착 부재 증명, 개발 중 발견한 함정은 [docs/DESIGN.md](docs/DESIGN.md)에 있습니다. 그림과 함께 설명한 해설서는 [docs/explainer.html](docs/explainer.html)입니다 (내려받아 브라우저로 여세요).
