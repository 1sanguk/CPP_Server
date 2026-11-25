# v10 — io_uring 기반 완료 통지 서버

> 현재 상태: 완료 (이 버전은 사용자 결정으로 Claude가 구현했다)

## 목표

v9의 epoll 네트워크 계층을 io_uring으로 바꾸고, 같은 게임 로직(20Hz 틱 + 3 × 3 격자 AOI) 위에서 세 가지 I/O 방식을 비교한다.

- IOCP(v8, Windows 완료 통지), epoll(v9, Linux 준비 통지), io_uring(v10, Linux 완료 통지)
- io_uring은 IOCP처럼 작업을 맡기고(SQE) 끝나면 결과(CQE)를 받는다. v8에서 익힌 "커널이 버퍼를 쓰는 동안 수명 지키기"와
  "남은 작업을 모두 회수한 뒤 정리"가 Linux에서 다시 나온다.
- v9와 같은 PC, 같은 컴파일러(g++), 같은 WSL2에서 비교한다.

프로토콜과 게임 규칙은 v8·v9와 같다. 틱 스레드 코드는 v9에서 그대로 가져왔다.

## 구성 파일

- [include/packet.h](include/packet.h), [src/packet.cpp](src/packet.cpp): v9와 같은 패킷 형식
- [include/packet_assembler.h](include/packet_assembler.h), [src/packet_assembler.cpp](src/packet_assembler.cpp): v9와 같은 조립기
- [include/world.h](include/world.h), [src/world.cpp](src/world.cpp): v9와 같은 2D 월드, 이동, 격자 AOI
- [include/tick_scheduler.h](include/tick_scheduler.h), [src/tick_scheduler.cpp](src/tick_scheduler.cpp): v9와 같은 틱 시작 시각 계산
- [include/game_job_queue.h](include/game_job_queue.h), [src/game_job_queue.cpp](src/game_job_queue.cpp): v9와 같은 job 큐
- [include/async_logger.h](include/async_logger.h), [src/async_logger.cpp](src/async_logger.cpp): v9와 같은 비동기 로거
- [include/uring_world_server.h](include/uring_world_server.h), [src/uring_world_server.cpp](src/uring_world_server.cpp): io_uring ring 스레드(새로 작성)와 틱 루프(v9와 같음)
- [src/main.cpp](src/main.cpp): v9와 같다(`sigwait`로 SIGINT/SIGTERM, Enter도 같은 종료 경로)
- 시험·측정은 v9의 로컬 도구(`server/v9/tools`, 저장소에 없음)를 `SERVER_BIN`으로 서버만 바꿔 쓴다(프로토콜이 같다).
  `compare_io.py`(로컬 전용)는 v9와 v10을 같은 조건으로 잰다.

## v9에서 바꾼 네트워크 계층

| 항목 | v9 (epoll) | v10 (io_uring) | 이유 |
|---|---|---|---|
| 통지 방식 | 준비 통지(LT). 읽기·쓰기 가능만 알리고 `recv`/`sendmsg`는 직접 | 완료 통지. SQE로 맡기고 CQE로 결과를 받음 | 플랫폼 API |
| 스레드 | reactor 1 + 틱 1 | ring 1 + 틱 1 | 소켓을 만지는 스레드를 하나로(같음) |
| 시스템 호출 | `epoll_wait` + fd마다 `recv` + 세션마다 `sendmsg` | 한 루프의 제출과 대기를 `io_uring_submit_and_wait_timeout` 한 번으로 | 아래 측정에서 약 1/7.6 |
| accept | listen fd가 읽을 수 있으면 `accept4` 반복 | multishot accept: SQE 하나로 접속마다 CQE | |
| 수신 | fd마다 최대 4번 `recv` | `V10_RECV_MODE`: `fixed`(세션 8 KiB 버퍼, IOCP와 같은 모양) / `provided`(공용 버퍼 묶음 + multishot recv) | 유휴 연결 메모리 비교 |
| 송신 | `EAGAIN`이면 `EPOLLOUT` 켜기 | 세션당 sendmsg 하나만 진행, 완료 바이트만큼 큐를 당기고 이어서 제출 | IOCP와 같다 |
| 작업 식별 | epoll 키 = session_id | user_data = 작업 종류(상위 8비트) + session_id | 포인터를 담으면 세션이 먼저 사라졌을 때 해제된 메모리를 가리킨다 |
| 수신 멈춤 | 읽기 전에 수위를 보고 `EPOLLIN`을 뺌 | `fixed`는 recv를 다시 걸지 않음, `provided`는 multishot을 취소. 멈춘 뒤 받은 바이트는 세션 조립기에 두고, 재개할 때도 세션마다 수위를 먼저 봄 | 이미 받은 데이터는 버릴 수 없다(보완 참고) |
| 세션 닫기 | `epoll_ctl(DEL)` 후 바로 `close` | `shutdown` + `cancel_fd`로 진행 중인 작업을 끝내고, 그 세션의 완료가 모두 돌아온 뒤에만 `close` | 커널이 아직 쓰는 버퍼(수신 버퍼, 송신 iovec·msghdr)를 해제하지 않게 |
| 종료 | 남은 송신 한 번 → fd 정리 | 남은 송신 → 모두 취소 → 진행 중 작업 수가 0이 될 때까지 완료 회수 → ring 정리 | v8 `Clean_Up`과 같은 순서 |

```
ring 스레드 (Server_Run)                          틱 스레드 (Game_Tick_Loop, v9와 같음)
  io_uring_submit_and_wait_timeout (제출 + 대기 한 번)    Drain_Until(다음 틱 시각)
   ├ CQE Accept(multishot) → 세션 생성 → Enter job ─┐   입력 처리 → 입력 만료 → World::Step
   ├ CQE Recv → 조립 → Push_Batch ─────────────────┼→ job 큐 → 시야 수집 → 세션별 한 틱 버퍼
   ├ CQE Send → 큐 전진 → 남았으면 sendmsg 다시 제출  │           → 세션 송신 큐에 넣기
   └ CQE Wake(eventfd read) ← "보낼 세션 목록" ←────────────────── 틱 끝에 목록을 넘기고 eventfd write
main 스레드: sigwait(SIGINT/SIGTERM) 또는 Enter → Stop() → eventfd write
```

- ring은 `IORING_SETUP_SINGLE_ISSUER`(이 ring에는 ring 스레드만 제출) + `DEFER_TASKRUN`(완료 처리를 기다릴 때 몰아서) +
  `CQSIZE`(완료 큐를 제출 큐의 4배)로 만든다. 플래그를 지원하지 않는 커널이면 빼고 다시 만든다.
- 제출 큐가 가득 차면 쌓인 것을 먼저 제출하고 다시 얻는다. 진행 중 작업 수(`pending_ops`)는 SQE마다 하나씩 늘고, 마지막 CQE
  (`IORING_CQE_F_MORE`가 꺼진 것)에서 하나씩 준다. multishot은 그 전에 여러 번 온다.
- `provided` 방식은 공용 버퍼(기본 4,096 × 1 KiB)를 커널에 등록한다. 커널이 데이터를 받을 때 하나를 골라 채우고 CQE에 버퍼 번호를
  담아 주면, 조립기에 복사한 뒤 바로 묶음에 돌려준다. 돌려주지 않으면 버퍼가 바닥나 recv가 `-ENOBUFS`로 끝난다(그때는 다시 건다).

## 환경변수

v9와 같은 뜻의 변수는 접두사만 `V10_`다: `V10_TICK_MS`(50), `V10_WORLD_SIZE`(1000), `V10_CELL_SIZE`(100), `V10_MAX_SPEED`(60),
`V10_SEED`(8), `V10_INPUT_TIMEOUT_MS`(500), `V10_CHATS_PER_TICK`(4), `V10_MAX_ENTER_LEAVE_PER_TICK`(64),
`V10_JOB_HIGH_WATERMARK`/`LOW`(8192/1024), `V10_SPAWN_X/Y/RADIUS`, `V10_STATUS_INTERVAL_MS`(10000), `V10_LOG_LEVEL`(info).

| 이름 | 기본값 | 의미 |
|---|---:|---|
| `V10_RECV_MODE` | provided | `fixed`(세션 8 KiB 버퍼) 또는 `provided`(공용 버퍼 묶음 + multishot recv) |
| `V10_RING_ENTRIES` | 4096 | 제출 큐 크기(완료 큐는 4배) |
| `V10_BUF_COUNT` / `V10_BUF_SIZE` | 4096 / 1024 | provided 버퍼 수(2의 거듭제곱으로 올림)와 크기 |
| `V10_DEFER_TASKRUN` | 1 | 0이면 `IORING_SETUP_DEFER_TASKRUN`을 쓰지 않는다(비교용) |
| `V10_SQPOLL` / `V10_SQPOLL_IDLE_MS` | 0 / 1000 | 1이면 SQPOLL(커널 스레드가 제출 큐를 읽음, `DEFER_TASKRUN` 대신) |
| `V10_FIXED_FILES` | 0 | 1이면 고정 파일 표에 fd를 등록하고 칸 번호로 제출 |

`[Status]`는 v9와 같은 틱 지표에 더해 `uring_enters`(제출·대기 호출), `sqes`, `cqes`, `pending_ops`, `cq_overflow`,
`sends`, `partial_sends`, `cancels`, `no_buffers`(`-ENOBUFS`), `job_queue_window_peak`(지난 출력 이후 job 큐 최대)를 출력한다.

## 실행과 테스트

Linux 또는 WSL2에서 실행하고, liburing이 필요하다(`sudo apt install liburing-dev`). CMake는 liburing이 없으면 v10만 건너뛴다.

```bash
cmake -S server -B ~/v9build -DCMAKE_BUILD_TYPE=Release
cmake --build ~/v9build --target v10_server -j
~/v9build/v10/v10_server                                     # 포트 9000, Enter·Ctrl+C·SIGTERM으로 종료
V10_RECV_MODE=fixed ~/v9build/v10/v10_server                 # 고정 버퍼 수신
```

v9의 로컬 도구로 시험한다(저장소에 없음):

```bash
python3 server/v9/tools/v9_check.py ~/v9build/v10/v10_server                              # 기능 검사
SERVER_BIN=~/v9build/v10/v10_server bash server/v9/tools/run_load.sh ~/v9build 2000 30      # 봇 2,000명
SERVER_BIN=~/v9build/v10/v10_server bash server/v9/tools/slow_client_check.sh ~/v9build 1000 70
python3 server/v9/tools/compare_io.py ~/v9build 2000 30 2                                 # v9 ↔ v10 비교
```

ASan + LeakSanitizer는 `-DV10_ENABLE_ASAN=ON`으로 따로 빌드한다. Server Lab에서 v10을 고르면 v9처럼 WSL2 안에서 빌드해 띄운다.
CI는 [v10-linux.yml](../../.github/workflows/v10-linux.yml)(수동 실행): liburing을 설치하고 Release·ASan 빌드를 확인한다.

## 검증 결과 (2026-11-25, WSL2 Ubuntu 24.04, 커널 6.6, liburing 2.5, g++ 13, 16코어 PC 한 대에서 서버와 봇 함께 실행)

- 기능 검사(`fixed`·`provided` 각 6회): 서로 spawn, 시야 채팅, 3칸 밖 이동 뒤 despawn·채팅 미전달, 속도 상한, 입력 만료,
  잘못된 입력 3종에서 그 연결만 종료, Enter·SIGTERM 종료 코드 0. Release·ASan 통과
- 봇 2,000명(두 방식): 전원 입장, 끊김·패킷 오류 0, 시야 불일치 0, 늦은 틱 0
- 읽지 않는 클라이언트 2개 + 봇 1,000명(셀 하나, ASan, 두 방식): 송신 큐 4 MiB를 넘은 두 연결만 취소·종료, 동시 입장·퇴장은 미뤄 처리,
  ASan 보고 0, 종료 때 진행 중 작업 수 0까지 회수
- 수신 멈춤(수위 1,000/250, 2,000명): 보낸 이동 입력 모두 처리, 끝에 멈춘 세션 0. job 큐 최대 2,111(fixed)·2,135(provided)
- Server Lab GUI 테스트(WSL2로 띄운 v10에 Windows 클라이언트 화면): 채팅·주변 인원·돌아다니기, Enter 종료 코드 0
- v9와 같은 조건 비교(2,000명, 각 2회, `compare_io.py`):

| 구성 | 틱 평균 | I/O 스레드 CPU | 초당 I/O 시스템 호출 | 유휴 연결 1,000개 메모리 |
|---|---:|---:|---:|---:|
| v9 epoll | 5.95~6.31ms | 43.0~45.5% | 약 69,000 | +2.4 MB (연결당 2.4 KB) |
| v10 fixed | 5.77~6.02ms | 38.7~40.9% | 약 9,000 | +10.4 MB (연결당 10.6 KB) |
| v10 provided | 5.86~6.16ms | 38.4~39.8% | 약 9,200 | +2.5 MB (연결당 2.6 KB, 시작 때 공용 버퍼 4 MiB) |

- 틱 시간은 같다(게임 로직이 같다). 바뀐 것은 I/O 스레드다. 시스템 호출이 약 1/7.6, I/O 스레드 CPU가 약 10~15% 줄었다.
- `fixed`는 세션마다 8 KiB 버퍼를 들고 있어 유휴 연결이 비싸다. `provided`는 연결 수와 상관없이 공용 버퍼만 쓰므로 v9 수준이다.

## 트러블슈팅

자세한 기록은 [troubleshooting.md](../../basicdata/troubleshooting.md)의 `[v10]`에 있다.

- **기능 검사가 가끔 "속도 상한"에서 실패**: v10만의 문제인 줄 알았는데 v9에서도 8번 중 1번 실패했다. 시험이 `time.time()`(벽시계)로
  5초를 쟀는데, WSL2가 벽시계를 Windows와 맞추며 조정해 실제로는 6초 동안 이동했다. 단조 시계(`time.monotonic`)로 바꿨다.
- **liburing을 찾지 못함**: WSL에 `pkg-config`가 없어 CMake가 liburing을 못 찾았다. 헤더와 라이브러리를 직접 찾도록 바꿨다.

## 보완 (2026-11-25)

완료 뒤 남겼던 점들을 처리했다. 자세한 내용은 [feedback.md](../../basicdata/feedback.md)의 v10 항목에 있다.

- **수신 멈춤 넘침**: 넘침이 연결 수에 비례했다(수위 200, 2,000명에서 약 1,100). 원인은 재개 때 멈춘 세션 수천 개가 쌓아 둔 패킷을 수위
  확인 없이 한꺼번에 넣은 것이었다. 재개할 때도 세션마다 수위를 먼저 보게 하자 216~232(연결 수와 무관, v9 242~311보다 낮음),
  수위 1,000/250에서는 정확히 1,000이 됐다. 기본 수위 처리량은 그대로다.
- **provided 버퍼 바닥남**: 같은 4 MiB를 4,096 × 1 KiB로 쪼개 0회(예전 1,024 × 4 KiB는 0~197회).
- **SQPOLL·고정 파일**: 옵션으로 넣고 쟀다. 고정 파일은 차이가 없고, SQPOLL은 ring 스레드 CPU를 37% → 5%로 줄이지만 커널 스레드가 코어
  하나를 99% 써서 전체 CPU는 약 3배다. 이 부하에서는 이득이 없어 기본값은 끈다. 등록 버퍼는 송신 버퍼 구조상 맞지 않아 하지 않았다.
- **코어 분리**: 서버와 봇을 다른 코어에 고정해도 결과가 같았다(I/O 스레드 CPU v9 39.9%, v10 36.7~37.8%).
- 고정 파일을 켜면 닫은 연결이 바로 끊기지 않던 문제를 찾아 고쳤다(닫을 때 항상 `shutdown`).

## 남은 점

- 서버와 봇을 다른 컴퓨터에서 돌리는 측정은 하지 못했다(컴퓨터가 하나).
- 등록 버퍼는 송신 경로를 고정 메모리 풀로 바꿀 때 함께 검토한다.
- 테스트·측정 도구는 로컬 전용이라 CI는 빌드만 확인한다.
