# v9 — v8 틱 서버의 Linux(epoll) 이식

> 현재 상태: 완료 (이 버전은 사용자 결정으로 Claude가 구현했다)

## 목표

v8(Windows IOCP)의 20Hz 틱 루프 + 3 × 3 격자 AOI를 배포 타겟인 Linux에서 epoll로 돌린다.

- 같은 게임 로직을 완료 통지(IOCP, "작업이 끝났다")와 준비 통지(epoll, "이제 읽거나 쓸 수 있다") 두 방식의
  네트워크 계층 위에 올려 차이를 비교한다.
- 소켓을 모르는 게임 로직과 플랫폼별 네트워크 계층의 경계를 코드로 분리한다.

프로토콜과 게임 규칙은 v8과 같다. 소켓과 무관한 코드는 v8에서 그대로 가져오고, 네트워크 계층은 v5의 epoll 패턴으로 새로 썼다.

## 구성 파일

- [include/packet.h](include/packet.h), [src/packet.cpp](src/packet.cpp): v8과 같은 패킷 형식
- [include/packet_assembler.h](include/packet_assembler.h), [src/packet_assembler.cpp](src/packet_assembler.cpp): v8과 같은 조립기
- [include/world.h](include/world.h), [src/world.cpp](src/world.cpp): v8과 같은 2D 월드, 이동, 격자 AOI
- [include/tick_scheduler.h](include/tick_scheduler.h), [src/tick_scheduler.cpp](src/tick_scheduler.cpp): v8과 같은 틱 시작 시각 계산
- [include/game_job_queue.h](include/game_job_queue.h), [src/game_job_queue.cpp](src/game_job_queue.cpp): v8과 같은 틱 단위 job 큐
- [include/async_logger.h](include/async_logger.h), [src/async_logger.cpp](src/async_logger.cpp): v8과 같은 비동기 로거
- [include/epoll_world_server.h](include/epoll_world_server.h), [src/epoll_world_server.cpp](src/epoll_world_server.cpp): epoll reactor와 틱 루프(새로 작성)
- [src/main.cpp](src/main.cpp): v5처럼 SIGINT/SIGTERM을 막고 main 스레드가 `sigwait`로 받는다. 표준 입력의 줄바꿈(Enter)도 같은 종료 경로로 보낸다
- `tests/unit_tests.cpp`(로컬 전용, 저장소에 없음): 셀·시야·이동·입력 순번·틱 스케줄러·패킷·조립기·job 큐 단위 테스트 (`ctest`, Linux·Windows 양쪽)
- `tools/logic_bench.cpp`(로컬 전용, 저장소에 없음): 서버 없이 게임 로직(이동·시야 수집·인코딩)만 같은 입력으로 재는 벤치마크(g++·MSVC 비교용)
- `tools/load_bot.cpp`(로컬 전용, 저장소에 없음): epoll 하나로 연결 수천 개를 돌리는 C++ 부하 봇 + 시야 정확성 검증
- `tools/run_load.sh`(로컬 전용, 저장소에 없음): 서버 + 부하 봇 + SIGTERM 종료, 측정 구간의 `[Status]` 출력
- `tools/slow_client_check.sh`(로컬 전용, 저장소에 없음): 읽지 않는 클라이언트로 partial send, 송신 큐 상한, 연결이 남은 종료 확인
- `tools/v9_check.py`(로컬 전용, 저장소에 없음): 실제 서버를 띄워 확인하는 기능 검사(입장, 시야 채팅, 셀 이동, 속도 상한, 입력 만료, 잘못된 입력에서 종료)

## v8에서 바꾼 네트워크 계층

| 항목 | v8 (IOCP) | v9 (epoll) | 이유 |
|---|---|---|---|
| 통지 방식 | 완료 통지. 버퍼를 맡기면 다 받은/보낸 뒤 알림 | 준비 통지(LT). 읽기·쓰기 가능만 알리고 `recv`/`sendmsg`는 직접 | 플랫폼 API |
| 스레드 | IOCP worker 16 + 틱 1 | reactor 1 + 틱 1 | 소켓을 만지는 스레드를 하나로 |
| 수신 | `WSARecv` 게시, 완료 시 조립 | fd당 최대 4번 `recv` 후 조립, 남은 것은 다음 `epoll_wait` | LT라 한 연결이 reactor를 독차지하지 않게 |
| 송신 | 틱이 세션 묶음을 worker에게 넘겨 `WSASend`(WSABUF 배열) | 틱이 세션 목록을 넘기고 eventfd로 깨움, reactor가 `sendmsg`(iovec 배열) | 틱 스레드가 송신 호출에 시간을 쓰지 않게(같은 이유) |
| 송신이 막힐 때 | 완료 통지가 이어서 게시 | `EAGAIN`이면 `EPOLLOUT`을 켜고, 다 보내면 끔 | LT에서 켜 두면 쓸 것이 없어도 매번 깨어남 |
| 수신 멈춤 | 다음 `WSARecv`를 게시하지 않음 | 그 fd의 `EPOLLIN`을 뺌 | 같은 TCP 흐름 제어 |
| 세션 식별 | completion key 대신 context의 세션 참조 | epoll 이벤트 키 = session_id | 닫은 fd 번호가 같은 묶음 안에서 재사용돼도 섞이지 않게 |
| 끊긴 연결에 쓰기 | 오류 반환 | `MSG_NOSIGNAL`로 SIGPIPE 대신 `EPIPE` | Linux는 기본 동작이 프로세스 종료 |
| 종료 신호 | Enter 입력 | `sigwait`(SIGINT/SIGTERM) + eventfd. Enter도 SIGTERM으로 바꿔 같은 경로 | v5와 같은 방식, Server Lab(WSL)에서도 끌 수 있게 |

```
reactor 스레드 (Server_Run)              틱 스레드 (Game_Tick_Loop, v8과 같음)
  epoll_wait                              Drain_Until(다음 틱 시각)
   ├ listen fd → accept4 → Enter job ─┐    입력 처리 → 입력 만료 → World::Step
   ├ client fd IN → recv → 조립 ───────┼→ job 큐 →  → 시야 변경 수집 → 세션별 한 틱 버퍼
   ├ client fd OUT → sendmsg(iovec)    │              → 세션 송신 큐에 넣기(send_mutex)
   └ eventfd ← "보낼 세션 목록" ←──────────────────── 틱 끝에 목록을 넘기고 eventfd write
main 스레드: sigwait(SIGINT/SIGTERM) → Stop() → eventfd write
```

- 닫기는 reactor만 한다(`closing` 원자 교환으로 한 번만). 틱 스레드가 닫아야 할 때(송신 큐 4 MiB 초과, 잘못된 패킷)는
  reactor에 닫기를 요청한다. job 큐가 가득 차 넣지 못한 Leave는 reactor가 매 루프 다시 넣는다(잃으면 플레이어가 월드에 남는다).
- 종료 순서: `Stop` → reactor 루프 종료 → job 큐 Close·틱 스레드 join → 남은 송신 한 번 더(best-effort) → fd 정리.
  eventfd는 `Stop()`이 다른 스레드에서 쓸 수 있으므로 생성자에서 만들고 소멸자에서 닫는다.
- 수신 멈춤은 읽기 **전에** 수위를 본다. 한 번의 `epoll_wait` 묶음에서 앞의 fd들이 큐를 수위 위로 올리면 뒤의 fd는 읽지 않는다.
- 입장·퇴장은 틱당 상한(`V9_MAX_ENTER_LEAVE_PER_TICK`)이 있다. 하나가 시야 안의 모두에게 보임/사라짐을 기록하므로, 한 시야 안의
  많은 인원이 한꺼번에 들어오거나 나가면 그 틱이 N²로 길어진다. 넘으면 그 뒤의 job을 패킷까지 순서 그대로 다음 틱으로 미룬다.

## 환경변수

| 이름 | 기본값 | 의미 |
|---|---:|---|
| `V9_LOG_LEVEL` | info | debug / info / error |
| `V9_TICK_MS` | 50 | 틱 주기(ms) |
| `V9_WORLD_SIZE` / `V9_CELL_SIZE` | 1000 / 100 | 월드 한 변과 셀 한 변 |
| `V9_MAX_SPEED` | 60 | 초당 최대 이동 거리 |
| `V9_SEED` | 8 | 시작 위치 난수 seed |
| `V9_STATUS_INTERVAL_MS` | 10000 | `[Status]` 출력 간격 |
| `V9_JOB_HIGH_WATERMARK` / `V9_JOB_LOW_WATERMARK` | 8192 / 1024 | 수신 멈춤 수위 |
| `V9_CHATS_PER_TICK` | 4 | 세션별 틱당 채팅 수 상한 |
| `V9_MAX_ENTER_LEAVE_PER_TICK` | 64 | 틱당 처리할 입장·퇴장 수. 넘으면 그 뒤의 job을 다음 틱으로 미룬다 |
| `V9_INPUT_TIMEOUT_MS` | 500 | 이 시간 동안 `C_Move`가 없으면 그 플레이어를 멈춘다(입력 만료). 0이면 끔 |
| `V9_SPAWN_X` / `V9_SPAWN_Y` / `V9_SPAWN_RADIUS` | 없음 | 셋 다 있으면 무작위 스폰 대신 그 원 안에 스폰(개발용) |

`[Status]`의 `phase_avg_us`는 v8과 같은 단계별 평균 시간이고, v9는 `wakes`(eventfd로 깨어난 수), `sendmsg_calls`,
`partial_sends`(EAGAIN으로 `EPOLLOUT`을 기다린 수), `send_overflows`, `recv_pauses`, `jobs_deferred_now`/`jobs_deferred`
(입장·퇴장 상한으로 미룬 job)를 함께 출력한다.

## 실행과 테스트

Linux 또는 WSL2에서 실행한다. WSL의 `/tmp`는 WSL이 꺼지면 지워지므로 빌드 폴더는 홈에 둔다.

`tests/`와 `tools/`(단위 테스트, 기능 검사, 부하 봇, 벤치마크, 측정 스크립트)는 저장소에 올리지 않고 로컬에만 둔다(`.gitignore`).
CMake는 이 폴더가 있을 때만 해당 대상을 만들므로, 저장소만 받은 상태에서는 `v9_server`와 `v9_core`만 빌드된다.

```bash
cmake -S server -B ~/v9build -DCMAKE_BUILD_TYPE=Release
cmake --build ~/v9build --target v9_server v9_load_bot v9_unit_tests v9_logic_bench -j
ctest --test-dir ~/v9build -R v9_unit_tests                   # 단위 테스트
~/v9build/v9/v9_logic_bench 2000 600                         # 게임 로직만 벤치마크
~/v9build/v9/v9_server                                       # 포트 9000, Enter·Ctrl+C·SIGTERM으로 종료
python3 server/v9/tools/v9_check.py ~/v9build/v9/v9_server   # 기능 검사(서버를 직접 띄우고 끈다)
bash server/v9/tools/run_load.sh ~/v9build 2000 30           # 서버 + 봇 2,000명 30초 + SIGTERM 종료
bash server/v9/tools/slow_client_check.sh ~/v9build 1000 70  # 읽지 않는 클라이언트
```

ASan + LeakSanitizer는 `-DV9_ENABLE_ASAN=ON`으로 따로 빌드(예: `~/v9asan`)해서 같은 스크립트를 돌린다.
`run_load.sh`는 봇이 움직이는 동안의 스레드별 CPU 사용률(`v9-reactor`, `v9-tick`)도 출력하고,
`SERVER_CPUS=0-7 BOT_CPUS=8-15`를 앞에 붙이면 서버와 봇을 서로 다른 코어에 고정한다.

Windows에서는 `cmake -S server -B build/server -A x64`로 같은 단위 테스트와 벤치마크(`v9_unit_tests`, `v9_logic_bench`)를
MSVC로 빌드할 수 있다. 서버는 Server Lab에서 v9를 고르면 WSL2 안에서 빌드해 띄운다.
CI는 [v9-linux.yml](../../.github/workflows/v9-linux.yml)(수동 실행): 테스트·도구가 저장소에 없으므로 Linux Release·ASan 서버 빌드와
Windows MSVC 코어 빌드만 확인한다.
수신 멈춤은 기본 수위에서 잘 일어나지 않으므로 `V9_JOB_HIGH_WATERMARK=200 V9_JOB_LOW_WATERMARK=50`을 앞에 붙여 확인한다.

부하 봇만 따로: `v9_load_bot [clients=500] [seconds=20] [port=9000] [host=127.0.0.1] [move_ms=100]`.
봇은 마지막에 모두 멈춘 뒤, 받은 spawn/despawn으로 재구성한 주변 목록이 마지막 위치의 격자 규칙과 같은지 검사한다.
검증은 서버에 봇만 접속해 있다고 가정한다.

## 검증 결과 (2026-11-22, WSL2 Ubuntu 24.04, g++ 13, 16코어 PC 한 대에서 서버와 봇 함께 실행)

- 기능 검사(`v9_check.py`, 2026-11-23 추가): 서로 spawn, 시야 채팅, 3칸 밖 이동 뒤 despawn·채팅 미전달, 속도 상한(요청 1000 → 60),
  입력 만료(500ms), 헤더 size < 4·길이가 틀린 `C_Move`·알 수 없는 id에서 그 연결만 종료, 종료 코드 0. Release·ASan 통과
- 봇 500 / 1,000 / 2,000명(초당 10회 이동 입력): 전원 입장, 끊김·패킷 오류 0, 시야 불일치 0
- 2,000명 틱: 평균 5.2~6.3ms, 최대 9.5ms, 늦은 틱 0, 틱당 송신 2.3~2.5 MB.
  2026-11-23 다시 잰 값은 평균 5.8~7.2ms, 최대 7.3~9.1ms(첫 구간만 20ms), 늦은 틱 0
- 읽지 않는 클라이언트 2개(셀 하나, 봇 1,000명): partial send 뒤 `EPOLLOUT` 대기, 송신 큐 4 MiB를 넘자 그 둘만 닫힘.
  나머지 봇은 `S_Despawn`을 받고 시야 일치
- 수신 멈춤/재개(수위 200/50, 2,000명): 멈춤 242,174회, 끝에 멈춘 세션 0, 끊김·시야 불일치 0
- SIGTERM 종료: 접속이 남은 상태에서도 정리 후 종료 코드 0
- ASan + LeakSanitizer: 봇 500명, 느린 클라이언트 시나리오 모두 경고 없음
- v8과의 비교는 [benchmark.md](../../basicdata/benchmark.md)에 있다. 틱당 송신량은 같고 틱 시간은 약 1/2.5~1/3이다.
  게임 로직만 재 보면(`logic_bench`) 컴파일러·표준 라이브러리 차이는 약 1.6배(약 2ms)이고, 나머지는 서버 구조와 실행 환경 쪽이다(아래 보완).

## 트러블슈팅

자세한 기록은 [troubleshooting.md](../../basicdata/troubleshooting.md)의 `[v9]`에 있다.

- **WSL에서 빌드 결과가 사라짐**: `/tmp/v9build`에 빌드했는데 다음 명령에서 실행 파일이 없었다. WSL은 쓰지 않으면 꺼지고
  그때 `/tmp`가 비워진다. 빌드 폴더를 홈으로 옮겼고, 측정 스크립트는 서버 로그에서 필요한 줄을 바로 출력하게 했다.
- **느린 클라이언트 확인에서 봇이 FAIL**: 셀 하나(전체 방송)에서는 읽지 않는 클라이언트도 시야에 들어오는데, 봇은 그 존재를
  모르므로 "2명 더 보인다"고 판단했다. 송신 큐 상한으로 둘이 닫힌 뒤에는 일치했다(서버 문제가 아니라 확인 방법의 한계).

## 보완 (2026-11-23)

완료 뒤 남겼던 점들을 고쳤다. 자세한 내용은 [feedback.md](../../basicdata/feedback.md)의 v9 항목에 있다.

- 단위 테스트 13개(g++·MSVC 모두 통과)와 기능 검사(`v9_check.py`)를 추가했다.
- 게임 로직 벤치마크로 v8↔v9 차이를 나눴다. 로직만은 g++ 3.4~4.0ms ↔ MSVC 5.7~6.5ms(약 1.6배)이고, 서버 틱 차이(약 9ms) 중
  나머지 약 7ms는 서버 구조와 실행 환경(v8은 IOCP worker 16개와 봇이 같은 PC에서 CPU를 함께 씀) 쪽이다.
- 수신 멈춤을 읽기 전에 판단한다. 수위 1,000/250에서 job 큐 최대 1,388(보완 전 수위 200에서 2,239).
- reactor CPU: 2,000명에서 46~52%(코어 하나 기준), 틱 14~15%. reactor는 하나로 유지한다.
- 서버·봇 코어 분리: 틱 6.4~7.6ms(분리 안 함 6.7~8.7ms)로 큰 차이가 없다.
- 동시 입장·퇴장 상한: 셀 하나 1,000명에서 최대 틱 70.8ms → 29ms(Release), 늦은 틱 0.
- eventfd 수명 경쟁과 닫기 요청 반복을 고쳤다.
- CI(`v9-linux.yml`, 빌드 확인)와 Server Lab(WSL2) 지원을 추가했다.

## 남은 점

- 테스트·측정 도구는 로컬 전용이라 CI는 빌드만 확인한다. 위 검증 결과는 로컬에서 실행한 기록이다.
- 서버와 봇을 다른 컴퓨터에서 돌리는 측정은 하지 못했다(컴퓨터가 하나).
- 입장·퇴장으로 미룬 job은 job 큐 수위에 들어가지 않는다. 틱당 상한보다 빠른 접속·종료가 계속되면 미룬 job이 쌓인다.
- Server Lab 메인 창에서 v9를 직접 골라 시작하는 것은 확인하지 않았다(같은 명령을 따로 실행해 확인했다).
