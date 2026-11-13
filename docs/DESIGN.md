# Ignacio-C 설계 문서 — 세마포로 동기화한 멀티스레드·멀티프로세스 트레이딩 엔진

KOSPI200 선물을 대상으로 하는 자동매매 엔진을 C11 + POSIX(pthread, named semaphore, shm)로 구현했다.
이 문서는 **어떤 세마포가 왜 필요한지, 초기값이 왜 그 값인지, 교착 상태와 경쟁 조건이 왜 생기지 않는지**를 정리한다.

---

## 1. 전체 구조

```
 ┌───────────── feeder 프로세스 (posix_spawn) ─────────────┐
 │  loop: P(rl_md) → 시세 조회(KIS REST / mock) → 반납 예약   │
 │        → md 링에 tryput (꽉 차면 드롭 카운트)              │
 └──────────────┬───────────────────────────────────────────┘
                │ 공유메모리 SPSC 링 (named sem: md.e / md.f)
                │ 공유 permit (named sem: rl_md), 토큰 준비 신호 (named sem: tk)
 ┌──────────────▼─────────────── 엔진 프로세스 ─────────────────────────────────┐
 │ md_receiver ──fan-out──▶ inbox[0..N-1].md (꽉 차면 드롭)                        │
 │                                                                              │
 │ 전략 스레드 ×N  ◀─ P(items) ─ inbox{ack > ctl > md}                           │
 │    │ trywait(exposure) · trywait(inflight)                                    │
 │    └─ put ─▶ order_q (MPMC 유한 버퍼, cap 64)                                 │
 │                 │                                                            │
 │ 게이트웨이 워커 ×M ◀─ get                                                     │
 │    P(rl_trade) → 브로커 호출 → 반납 예약                                       │
 │    reg_bind{ 등록 + RECEIVED 전달 + park된 체결 회수 }  (registry mtx)          │
 │                 │ (mock) put ─▶ exch_q ─▶ 거래소 스레드: 지연 후 체결           │
 │                 │                          reg_route → book(쓰기락) → inbox.ack │
 │ nav 스레드: P(rl_trade) → 잔고 조회 → book(쓰기락)에 브로커 값 반영·대조         │
 │ clock 스레드: 1초 틱, 장마감 FLATTEN 방송, 상태 출력, 토큰 갱신                  │
 │ refill 스레드: 만기된 permit을 V()  (rl_md, rl_trade)                          │
 │ main 스레드: sigwait (SIGINT/TERM/ALRM/CHLD) = supervisor                     │
 └──────────────────────────────────────────────────────────────────────────────┘
```

기본 설정(mock)에서 스레드는 엔진 18개(전략 8 + 워커 4 + md/nav/clock/refill/거래소 5 + main 1)이고,
프로세스는 2개다. 세마포는 프로세스 간 named 4개 + 프로세스 내부 128개를 쓴다.

### 왜 이렇게 나눴나

| 단위 | 개수 | 나눈 이유 |
|---|---|---|
| feeder **프로세스** | 1 | 외부 네트워크/라이브러리 장애 격리. 죽어도 주문·포지션을 가진 엔진은 살아 있고, supervisor가 재기동한다 |
| 전략 스레드 | N | 전략끼리 격리. 느린 전략이 다른 전략의 시세 처리를 막지 않는다. 전략 내부 상태는 자기 스레드만 만지므로 전략 코드에는 락이 없다 |
| 게이트웨이 워커 | M | 브로커 호출은 I/O 대기(수십 ms)다. M개가 대기를 겹쳐 처리량을 M배로 늘린다(벤치 §7) |
| refill/clock/nav | 1씩 | 시간이 지나면 일어나야 하는 일들. macOS에 `sem_timedwait`가 없어서 시간 기반 V()는 전담 스레드가 한다 |

---

## 2. 세마포 전체 목록

| 세마포 | 종류 | 초기값 | P 하는 쪽 | V 하는 쪽 | 역할 |
|---|---|---|---|---|---|
| `md.e` / `md.f` | named, 프로세스 간 | 1024 / 0 | feeder / md_receiver | md_receiver / feeder | SPSC 링 빈 칸·찬 칸 |
| `rl_md.permits` | named, 프로세스 간 | 8 | feeder | 엔진 refill | 시세 조회 1초 한도 |
| `tok_ready` | named, 프로세스 간 | 0 | feeder(최초 1회) | 엔진 main | KIS 토큰 준비 신호 |
| `order_q` empty/full/pmtx/cmtx | 내부 | 64/0/1/1 | 전략·워커 | 워커·전략 | 주문 큐 (MPMC) |
| `exch_q` 4개 | 내부 | 256/0/1/1 | 워커·거래소 | 거래소·워커 | 모의 거래소 큐 |
| `inbox[i].{ack,ctl,md}` 각 4개 | 내부 | 64·64·128/0/1/1 | | | 전략 우편함 3개 큐 |
| `inbox[i].items` | 내부 | 0 | 전략 | 모든 생산자 | 세 큐 합계 (§3.2) |
| `rl_trade.permits` | 내부 | 10 | 워커·nav | refill | 주문/잔고 1초 한도 |
| `exposure` | 내부 | max_net=3 | 전략(진입) | 전략(청산 체결/진입 거절) | 계좌 롱 한도, 원자적 예약 (§3.4) |
| `strategy[i].inflight` | 내부 | K=2 | 전략(주문 전) | 전략(최종 ACK 소비) | 미체결 주문 수 한도, ACK 큐 용량 증명 (§4.2) |
| `registry.mtx` | 내부 | 1 | 워커·거래소 | 〃 | 주문번호 표 (§3.3) |
| `book.{turnstile,room_empty,rmtx}` | 내부 | 1/1/1 | | | Readers-Writers 락 (§3.5) |
| `ledger_mtx`, `csv_mtx` | 내부 | 1 | | | 모의 거래소 원장, CSV 한 행 |

**mutex도 이진 세마포로 통일한 이유**: 동기화를 전부 같은 원시 연산(P/V)으로 표현하면 정확성 논증을 하나의 모델로 할 수 있다.
대가도 있다. 세마포에는 소유자 개념이 없어서 잘못된 스레드가 V를 해도 막아 주지 않고, 우선순위 상속이나 robust 모드도 없다.
그래서 이진 세마포는 **짧은 임계구역(memcpy 수준) 안에서만, 다른 대기 없이** 쓰도록 제한했고(§4.1), **프로세스 간에는 이진 세마포를 아예 쓰지 않았다**(§5).

---

## 3. 핵심 동기화 패턴

### 3.1 유한 버퍼 (`bbuf.c`) — 생산자-소비자

```c
put:  P(empty); P(pmtx); write slot[tail]; tail++; V(pmtx); V(full);
get:  P(full);  P(cmtx); read  slot[head]; head++; V(cmtx); V(empty);
```

- **P(empty)는 반드시 P(pmtx)보다 먼저 한다.** 순서를 바꾸면, 버퍼가 꽉 찬 상태에서 생산자가 mutex를 쥔 채 잠든다.
  그러면 다른 생산자는 mutex에서 막히고, 소비자가 칸을 비워도 그 생산자를 깨울 수 없다(교착 상태).
- **생산자 mutex와 소비자 mutex를 분리했다.** 정확성 논증:
  - *읽는 칸은 다 쓰인 칸이다.* 생산자 A가 칸 k를 쓰는 동안 pmtx를 쥐고 있으므로, 다음 생산자 B는 A가 다 쓴 뒤에야 칸 k+1을 받는다.
    B가 A보다 먼저 V(full)을 해서 소비자가 칸 k를 읽게 되더라도, 칸 k의 기록은 이미 끝나 있다
    (A의 기록 → A의 V(pmtx) → B의 P(pmtx) → B의 V(full) → 소비자의 P(full)로 happens-before가 이어진다).
  - *쓰는 칸은 다 읽힌 칸이다.* 읽기는 cmtx 때문에 칸 순서대로 하나씩만 일어나고, V(empty)는 읽기가 끝난 뒤에만 한다.
    V(empty)가 m번 일어났다면 칸 0..m-1의 읽기는 모두 끝났다. n번째 생산자는 empty permit이 cap+m ≥ n+1개일 때만 진행하므로,
    덮어쓰는 칸 n−cap은 이미 읽혔다.
- SPSC 모드(프로세스 간 링)에서는 pmtx/cmtx를 쓰지 않는다. 생산자와 소비자가 하나씩이라 필요 없고, 이유는 §5에 더 있다.

### 3.2 우편함 (`inbox.c`) — 큐 3개를 세마포 하나로 기다리기

전략은 ACK, 제어 이벤트, 시세 중 **아무거나 하나라도** 오면 깨어나야 하고, 꺼낼 때는 ACK가 먼저다.
조건변수나 select 없이 이렇게 했다.

```c
생산자: bbuf_put(q_x, ev); V(items);        // 넣고 나서 알린다
소비자: P(items);
        if tryget(ack) → 처리 / else if tryget(ctl) → 처리 / else tryget(md) (반드시 성공)
```

items 값은 세 큐 원소 수의 합 이하다. 생산자는 put을 마친 뒤 V를 하고, 소비자는 전략 스레드 하나뿐이다.
그래서 P(items)가 통과하면 적어도 한 큐에 원소가 있고, 다른 소비자가 그걸 가져갈 수도 없다. 세 tryget이 모두 실패하면
가정이 깨진 것이므로 즉시 abort 한다(실행 중 0회).

ACK를 우선하는 이유: 체결을 반영하기 전에 새 시세로 판단하면, 이미 들어간 포지션을 모른 채 중복 진입할 수 있다.

### 3.3 주문 레지스트리 (`registry.c`) — "등록 전에 체결 도착" 경쟁

체결 통보는 브로커 주문번호(ODNO)만 들고 온다. 그런데 거래소는 주문을 받는 즉시 처리를 시작하므로,
**워커가 ODNO를 받아 표에 등록하기 전에 체결이 먼저 도착할 수 있다.**

- 거래소 쪽: `reg_route`가 주인을 못 찾으면 체결을 버리지 않고 **park** 해 둔다.
- 워커 쪽: `reg_bind`가 **같은 임계구역 안에서** ① 등록 ② RECEIVED를 우편함에 넣기 ③ park 목록 확인을 한 번에 한다.
- 두 동작이 같은 mtx로 직렬화되므로 순서는 둘 중 하나뿐이다. route가 먼저면 park에 남고 bind가 그걸 본다.
  bind가 먼저면 route가 엔트리를 찾는다. **둘 다 상대를 못 보고 지나가는 경우가 없다.**
- ②를 락 안에서 하기 때문에 같은 주문의 FILLED가 RECEIVED보다 먼저 전략에 도착하는 일도 없다.
- 검증: `test_registry`(2만 건, 유실·중복·순서 위반 0, park 경로 실행 확인). 엔진에서도 `--fast-fill` 비율만큼 즉시 체결이 생겨 이 경로를 탄다.

### 3.4 계좌 한도 `exposure` — 읽고 판단하지 말고 예약하라

전략 A와 B가 동시에 장부를 읽고 "net 2 < 한도 3"이라고 판단하면, 둘 다 진입해서 net이 4가 된다.
확인과 실행 사이에 상태가 바뀌는 TOCTOU 문제다. 그래서 진입 직전에 `trywait(exposure)`로 **확인과 차감을 원자적으로** 한다.

- 주문을 못 내면(inflight 부족) 즉시 V(exposure)로 되돌린다.
- 진입이 거절되면 V. 청산이 체결되면 V.
- 종료 시 **permit 보존**을 검사한다: 남은 permit + 전략들이 쥔 permit = 초기값.

### 3.5 포지션 장부 — 쓰기 스레드가 굶지 않는 Readers-Writers (`rwlock.c`)

Downey, *The Little Book of Semaphores* §4.2.6의 해법을 썼다. writer가 회전문(turnstile)을 잠그면 **새로 오는 reader**가 막히므로,
reader가 끊임없이 들어와도 writer(체결 반영)는 이미 들어가 있던 reader들만 기다리면 된다.
`test_rwlock`: reader 8개가 쉬지 않는 중에도 writer 최대 대기 ≈ 0.1~4ms, 상호배제 위반·찢어진 읽기 0.

### 3.6 레이트 리미터 (`ratelimit.c`) — 슬라이딩 1초 한도를 카운팅 세마포로

- permit C개. 요청 전 P. **응답을 받은 시각 + 1초(+50ms)** 이후에 refill 스레드가 V.
- **보장**: 같은 permit을 쓰는 연속된 두 요청 r₁, r₂에 대해 `start(r₂) ≥ end(r₁) + 1s ≥ arrive(r₁) + 1s` 이다.
  그래서 서버가 보는 어떤 1초 구간에도 permit 하나당 요청은 최대 1건, 전체로는 최대 C건이다.
- 흔한 "1초마다 C개 채우기"는 경계에서 2C건이 몰릴 수 있다(`test_ratelimit`가 반례를 출력한다).
- **예산 분할**: 시세용(rl_md 8/s)과 주문용(rl_trade 10/s)을 나눴다(합 18 < 서버 20). 하나의 예산을 공유하면
  시세 폴링이 permit을 다 써서 주문이 굶는다(우선순위 역전과 같은 모양).
- 반납 예약 슬롯은 mutex 없이 CAS로 다룬다. 이유는 §5.

---

## 4. 교착 상태가 없다는 논증

### 4.1 mutex(이진 세마포)는 "쥔 채로 기다리지" 않는다

| 이진 세마포 | 쥐고 있는 동안 하는 일 |
|---|---|
| bbuf pmtx/cmtx | memcpy + 인덱스 증가 |
| registry.mtx | 해시표 조작 + `inbox_put_ack` (= **tryput**, 막히지 않음, §4.2) |
| rwlock 내부, ledger_mtx, csv_mtx | 필드 갱신 / fprintf 한 줄 |

어떤 스레드도 이진 세마포를 쥔 채 다른 세마포를 블로킹으로 기다리지 않는다. Coffman 조건 중 "점유 대기(hold and wait)"가
mutex에 대해 성립하지 않으므로 mutex 사이의 교착은 불가능하다.

### 4.2 ACK 큐는 절대 꽉 차지 않는다 (용량 논증)

전략은 주문을 내기 전에 `trywait(inflight)`를 하고(초기값 K=2), **그 주문의 최종 ACK(FILLED/REJECTED)를 소비한 뒤에야** V 한다.
주문 하나는 ACK를 최대 2개 만든다. 따라서 어느 순간에도 아직 소비되지 않은 ACK ≤ 2K = 4 < ACK_CAP(64)이다.
그래서 `inbox_put_ack`는 tryput으로 구현했고, 실패하면 불변식 위반으로 abort 한다(실행 중 0회, 결과 요약에 출력).
이 성질은 전략 상태기계가 올바르다는 가정에 기대지 않는다. 세마포 inflight만으로 성립한다.

### 4.3 블로킹 대기의 wait-for 그래프에는 사이클이 없다

```
전략 ──(order_q 빈칸)──▶ 워커 ──(exch_q 빈칸)──▶ 거래소 ──▶ (블로킹 대기 없음)
                          └──(rl_trade permit)──▶ refill ──▶ (시간만 기다림)
md_receiver ──(md 링)──▶ feeder ──▶ (블로킹 put 없음: tryput)
                                └──(rl_md permit)──▶ refill
```

화살표 끝의 스레드들은 화살표 시작 쪽을 블로킹으로 기다리지 않는다(워커→전략은 tryput, 거래소→전략은 tryput).
그러므로 사이클이 없고, 교착 상태가 없다.

### 4.4 종료 순서

`engine.c` 머리 주석: feeder → md_receiver → nav/clock → 전략 → 워커 → 거래소 → refill.
각 단계에서 기다리는 쪽이 의존하는 상대는 아직 살아 있다. 예를 들어 워커가 rl_trade에서 기다려도 refill이 마지막까지
살아 있으므로 풀린다. poison 메시지는 소비자 수만큼 넣는다.

---

## 5. 프로세스 간 동기화와 장애 격리

- feeder는 fork가 아니라 **posix_spawn(=fork+exec)** 으로 띄운다. 엔진은 이미 멀티스레드라서, fork 직후 자식에게는
  다른 스레드가 쥐고 있던 락이 그대로 잠긴 채 복제될 수 있다. exec로 새 이미지를 시작하면 이 위험이 없다.
- 그래서 자식은 핸들을 상속받지 않고, **이름으로** `shm_open`, `sem_open`을 다시 연다. named 세마포가 필요한 이유가 이것이다.
- **프로세스 간에는 이진 세마포(mutex)를 쓰지 않는다.** 이진 세마포를 쥔 프로세스가 `kill -9` 당하면 그 세마포는 영원히 0으로
  남고(robust mutex 같은 장치가 세마포에는 없다), 상대 프로세스까지 멈춘다. 그래서 다음처럼 했다.
  - 링은 SPSC라 mutex가 필요 없다. 카운팅 세마포 empty/full은 생산자가 중간에 죽어도 "칸 하나가 줄어드는" 정도로만 손상된다.
  - 레이트 리미터 반납 슬롯, 모의 서버 카운터, 통계는 lock-free 원자 연산(CAS)으로 처리한다. 시작할 때 `atomic_is_lock_free`를
    확인하는데, lock-free 원자 변수는 주소에 의존하지 않아서 프로세스 간 공유메모리에서도 동작한다.
  - 토큰은 이중 버퍼 + 버전 번호로 처리한다(쓰는 쪽은 엔진 하나뿐).
- **supervisor**: main 스레드가 `sigwait`로 SIGCHLD를 받으면 feeder가 쥐고 있던 permit을 회수하고(`rl_reclaim_held`)
  새로 띄운다. 실측: `kill -9`를 두 번 했을 때 각각 약 4ms 안에 재기동했고, 시세와 매매가 이어졌으며 불변식이 전부 통과했다.
- 엔진이 죽으면 feeder는 `getppid()` 변화를 보고 스스로 종료한다(고아 방지).

---

## 6. 시그널과 스레드

- 스레드를 만들기 **전에** main이 SIGINT/TERM/ALRM/CHLD를 막는다(`pthread_sigmask`). 새 스레드는 마스크를 물려받는다.
  그래서 시그널은 sigwait 중인 main에서만 동기적으로 처리되고, 세마포를 쥔 임의의 스레드가 비동기 핸들러에 끌려가는 일이 없다.
- posix_spawn에 `SETSIGMASK`/`SETSIGDEF`를 줘서 자식에게는 막힌 마스크를 물려주지 않는다.
- feeder는 SIGINT를 무시하고(Ctrl+C는 엔진만 받음) SIGTERM 핸들러를 `SA_RESTART` 없이 단다.
  그래서 `sem_wait` 중에 EINTR로 깨어나 stop 플래그를 확인할 수 있다(macOS에는 `sem_timedwait`가 없다).

---

## 7. 검증 결과 (Apple M4, macOS)

| 항목 | 방법 | 결과 |
|---|---|---|
| 유한 버퍼 | `test_bbuf` P×C 최대 16×4, 용량 1까지 | 유실 0 · 중복 0 · 단일 소비자 FIFO 유지 · permit 보존 |
| RW 락 | `test_rwlock` reader 8 + writer 2, 2초 | 상호배제 위반 0 · 찢어진 읽기 0 · reader 동시 진입 최대 8 · writer 최대 대기 수 ms |
| 레이트 리미터 | `test_ratelimit` 스레드 16, 슬라이딩 구간 전수 검사 | 어떤 구간도 한도 초과 없음 · 처리량은 이론 상한의 약 90% · permit 보존 |
| 우편함 | `test_inbox` 생산자 5 | ACK/CTL 유실 0 · 시세는 받은 수 + 버린 수 = 보낸 수 |
| 레지스트리 경쟁 | `test_registry` 2만 건 | 유실·중복·순서 위반 0, park 경로 실행 확인 |
| 프로세스 간 | `test_ipc` | 20만 건 순서 보존(약 150만 msg/s), 프로세스 3개가 공유하는 permit 한도 준수 |
| 데이터 경쟁 | `make tsan` (테스트 5개 + 엔진 20초) | ThreadSanitizer 보고 0 |
| TSan 음성 대조군 | `make tsan-negative` (생산자 mutex 제거 빌드) | TSan이 즉시 data race 보고 → 검사기가 실제로 동작함 |
| 엔진 60초 | `make demo` | 주문 91, 체결 90, EGW00201 0, 불변식 8개 통과 |
| 레이트 리미터 OFF | `make demo-norl` | 주문 303 중 거절 153(EGW00201 151), 서버 거절 합계 278 → 재시도 폭주 |
| 장애 복구 | feeder `kill -9` ×2 | 각각 약 4ms 안에 재기동, 매매 지속, 불변식 통과 |
| 워커 확장성 | `make bench` (호출당 20ms) | 1→16 워커: 15.2배 (이론 16배) |

**TSan 주석에 대해**: macOS의 TSan 런타임은 `pthread_mutex`는 가로채지만 `sem_wait`/`sem_post`는 가로채지 않는다
(`nm`으로 확인). 그러면 세마포로 정상 보호된 코드도 경쟁으로 보고된다. 그래서 `sem.c`에서 TSan 빌드일 때만
post 직전 `__tsan_release`, wait 성공 직후 `__tsan_acquire`를 넣었다. POSIX가 보장하는 의미를 그대로 알려주는 것뿐이고,
세마포를 하나라도 빼면 TSan이 잡는다는 것을 음성 대조군으로 확인했다.

---

## 8. macOS에서 실제로 부딪힌 것들

| 문제 | 대응 |
|---|---|
| `sem_init` 미지원 (ENOSYS) | 전부 `sem_open`. 내부용은 만든 직후 `sem_unlink` → 익명처럼 동작 |
| `sem_timedwait`, `sem_getvalue` 없음 | 시간 기반 V()는 refill 스레드, 중단은 시그널(EINTR) + stop 플래그, 값 확인은 trywait로 세기(종료 후 검증 전용) |
| named 세마포가 fd를 하나씩 씀 | 시작 시 `RLIMIT_NOFILE` 상향 |
| SIGCHLD를 막아 둬도 sigwait에 안 옴 | 기본 동작이 "무시"인 시그널은 커널이 즉시 버린다 → 빈 핸들러 등록 (장애 복구 테스트에서 발견) |
| TSan이 세마포를 모름 | §7 |
| 세마포 이름 길이 31자 제한 | `/ig<pid>.md.e` 형식 |

---

## 9. 한계 (정직하게)

- **kis-live 체결**: KIS 실시간 체결통보(WebSocket H0IFCNI0)는 연동하지 않았다. 접수에 성공하면 주문가로 체결된 것으로 간주한다.
  실제 체결은 잔고 조회로 확인해야 하는데, KIS 잔고의 종목별 배열 파싱은 미구현이라
  순포지션 대조는 mock 모드에서만 한다.
- **처리량 상한**: 큐 하나의 처리량이 약 40~70만 ops/s에서 포화한다. macOS의 named 세마포는 매 연산이 커널 호출이기 때문이다.
  이 시스템의 병목은 브로커 왕복(수십 ms)과 초당 한도라서 문제되지 않지만, μs 단위 HFT 구조는 아니다.
- **feeder가 put 도중 죽으면** 링 칸 하나가 영구히 줄 수 있다(1024칸 중 1칸). 데이터가 손상되지는 않는다.
- 모의 시세의 손익은 랜덤워크 결과라 의미가 없다. 이 프로젝트의 주제는 전략 성과가 아니라 동시성 구조다.
- Linux 빌드는 Makefile에 분기(`-D_GNU_SOURCE -lrt`)를 넣었지만 직접 돌려보지는 않았다.
- 종목코드는 만기마다 바꿔야 한다(2026-11 기준 12월물 `A01612`로 실시세 조회 확인).
