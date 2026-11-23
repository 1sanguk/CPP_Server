# v8 — 틱 기반 게임 루프 + 격자 AOI

> 현재 상태: 완료

## 목표

v7의 "job이 오는 즉시 처리하고 방 전체에 방송" 구조를 MMO 서버의 기본 형태로 바꾼다.

- 게임 로직을 **고정 주기 틱(20Hz)** 으로 돌려, 한 틱 동안 쌓인 입력을 몰아서 처리하고 결과를 한 번에 보낸다.
- 플레이어에게 2D 좌표를 주고, **격자 AOI**로 근처 플레이어에게만 상태를 보낸다. v7은 인원이 N이면 전송량이 N²으로 늘었다.

v7의 IOCP 네트워크 계층(패킷 조립, 세션 context, 송신 큐, backpressure, 비동기 로거)을 그대로 가져오고,
game logic을 틱 루프와 월드로 바꿨다.

## 구성 파일

- [include/packet.h](include/packet.h), [src/packet.cpp](src/packet.cpp): 패킷 형식, 목록 패킷 분할, 해석 함수
- [include/packet_assembler.h](include/packet_assembler.h), [src/packet_assembler.cpp](src/packet_assembler.cpp): v7과 같은 조립기
- [include/world.h](include/world.h), [src/world.cpp](src/world.cpp): 2D 월드, 이동, 격자 AOI(소켓과 무관)
- [include/tick_scheduler.h](include/tick_scheduler.h), [src/tick_scheduler.cpp](src/tick_scheduler.cpp): 틱 시작 시각 계산(가짜 시각으로 테스트 가능)
- [include/game_job_queue.h](include/game_job_queue.h), [src/game_job_queue.cpp](src/game_job_queue.cpp): 틱 단위로 비우는 job 큐
- [include/async_logger.h](include/async_logger.h), [src/async_logger.cpp](src/async_logger.cpp): v7과 같은 비동기 로거
- [include/iocp_world_server.h](include/iocp_world_server.h), [src/iocp_world_server.cpp](src/iocp_world_server.cpp): IOCP 서버와 틱 루프
- `tests/v8_unit_tests.cpp`(지금은 저장소에 없음): 패킷·셀·AOI·틱 스케줄러·큐 단위 테스트 (`ctest`)
- `tests/v8_smoke_test.py`(지금은 저장소에 없음): 실제 서버를 띄워 확인하는 통합 테스트
- `tools/v8_load_bot.cpp`(지금은 저장소에 없음): 수천 명이 움직이는 C++ 부하 봇 + 시야 정확성 검증
- [../../basicdata/tools/measure_v8_scaling.py](../../basicdata/tools/measure_v8_scaling.py): 인원별 틱 시간, AOI 켬/끔 측정
- [../../.github/workflows/v8-windows.yml](../../.github/workflows/v8-windows.yml): Release·ASan 빌드를 확인하는 CI(테스트를 지운 뒤로는 빌드만)(push 때 자동 실행하지 않고 GitHub Actions에서 수동 실행)
- 위 `tests/`·`tools/`의 테스트와 부하 봇은 커밋하기 전에 저장소에서 지웠다. 검증 결과는 그때 실행한 기록이며,
  아래 `실행과 테스트`의 테스트 명령은 지금은 그대로 동작하지 않는다. CI는 서버 빌드(Release·ASan)만 확인한다.

## 패킷 형식

헤더는 v7과 같다: `[uint16 size][uint16 id][body]`(little-endian, 최대 8 KiB). float는 IEEE 754 비트 그대로 보낸다.
주변 엔티티 좌표(`q`)는 월드 크기 기준 16비트 고정소수점(`좌표 × 65535 / world_size`를 반올림)이다. 월드 1000이면 한 단계가 약 0.015다.

| id | 이름 | 방향 | body |
|---:|---|---|---|
| 1 | `C_Chat` | C→S | UTF-8 텍스트. 시야 안의 플레이어와 자신에게만 전달 |
| 2 | `C_Move` | C→S | `uint32 input_seq, float vx, float vy`(초당 이동량). 서버가 속도 상한으로 줄인다 |
| 102 | `S_Chat` | S→C | `uint32 sender_id` + 텍스트 |
| 103 | `S_Notice` | S→C | 텍스트 |
| 104 | `S_EnterWorld` | S→C | `uint32 my_id, float x, float y, float world_size, float cell_size` |
| 105 | `S_Spawn` | S→C | `uint16 count` + `[uint32 id, uint16 qx, uint16 qy]` × count |
| 106 | `S_Despawn` | S→C | `uint16 count` + `[uint32 id]` × count |
| 107 | `S_MoveBatch` | S→C | `uint16 count` + `[uint32 id, uint16 qx, uint16 qy]` × count (자기 자신 제외) |
| 108 | `S_SelfState` | S→C | `uint32 last_input_seq, float x, float y`. 내가 움직였거나 새 입력을 반영한 틱에만 |

목록이 8 KiB를 넘으면 같은 id의 패킷 여러 개로 나눈다. 한 틱 안에서는 `self → despawn → spawn → move` 순서로 보내므로
클라이언트는 받은 순서대로 적용하면 된다. `S_SelfState`의 `last_input_seq`는 "몇 번 입력까지 반영한 위치인지"라서,
클라이언트가 입력을 보내자마자 예측 이동한 뒤 아직 반영되지 않은 입력만 다시 적용해 보정할 수 있다.

## 틱 루프

```
[completion worker]  Recv → 조립 → 한 Recv의 패킷을 Push_Batch(잠금 1회) ─┐
                                                                         ▼
[tick thread]  Drain_Until(다음 틱 시각) ─ 1) 입력 처리 ─ 2) World::Step(dt) ─ 3) Flush
                                                                                │
               세션별 버퍼 하나로 인코딩 → 송신 큐 적재 → 게시가 필요한 세션 목록을
               worker 수만큼 묶어 PostQueuedCompletionStatus ─────────────────────┘
                                                                         ▼
[completion worker]  묶음 안 세션들의 WSASend 게시(scatter-gather, 복사 없음)
```

- **대기**: job이 와도 깨어나지 않고 다음 틱 시각까지 잔다(`wait_until`). 종료(Close)만 기다림을 끊는다.
- **늦은 틱 정책: 건너뛰기**. 시작 시각을 여러 번 놓쳤으면 따라잡으려 연달아 돌리지 않고, 한 틱의 `dt`를
  놓친 주기만큼 늘린다. 이미 느린 서버에 일을 더 몰아주지 않기 위해서다. 늦은 틱과 건너뛴 틱 수는 `[Status]`에 찍는다.
- **입력 합치기**: 이동 입력은 속도 상태를 덮어쓰므로 같은 틱에 여러 번 와도 마지막 값(과 그 순번)만 남는다.
- **채팅 속도 제한**: 세션마다 틱당 채팅 수(`V8_CHATS_PER_TICK`, 기본 4)를 넘으면 버리고 `chats_dropped`로 센다.
  틱 끝에 한 번에 돌려주는 구조라, 제한이 없으면 한 틱에 몰아 보낸 채팅이 발신자 자신의 송신 큐 상한을 넘긴다.
- **입력 만료** (Unity 클라이언트 M1a에서 추가): 서버는 마지막 속도를 계속 적용하므로, 모바일 앱이 정지 패킷을 못 보내고
  멈추거나 끊기면 캐릭터가 계속 걷는다. `V8_INPUT_TIMEOUT_MS`(기본 500ms) 동안 `C_Move`가 없으면 틱 루프가 속도를 0으로 만든다
  (`Expire_Idle_Inputs`, `[Status]`의 `inputs_expired`). 응답 순번은 마지막 입력 순번을 그대로 둔다.
  그래서 클라이언트는 움직이는 동안 50~100ms마다 현재 속도를 다시 보낸다(Server Lab GUI는 100ms).

## 격자 AOI

- 월드(기본 1000 × 1000)를 셀(기본 100)로 나누고, 셀마다 플레이어 slot 목록을 둔다.
- 두 플레이어는 셀 좌표 차이가 가로·세로 모두 1 이하일 때 서로 보인다(3 × 3). 이 관계는 대칭이다.
- 셀을 넘을 때만 시야를 다시 계산한다. 예전 3 × 3에만 있던 셀의 플레이어와는 서로 despawn,
  새 3 × 3에만 있는 셀의 플레이어와는 서로 spawn을 기록한다.
- 관찰자별로 `대상 → +1/-1`을 누적하므로, 같은 틱에 들어왔다 나간 대상은 0이 되어 아무것도 보내지 않는다.
- 틱 끝 수집(`Collect_Updates`)은 먼저 셀마다 "이번 틱에 움직인 플레이어" 목록을 한 번 만들고, 관찰자는 주변
  9개 셀의 목록을 이어 붙인다. 관찰자별 결과 vector는 틱마다 비우기만 하고 다시 쓴다.
- `V8_CELL_SIZE`를 월드 크기 이상으로 주면 셀이 하나라 AOI를 끈 것(전체 방송)과 같다. 비교 측정에 쓴다.

## v7에서 바꾼 네트워크 계층

| 항목 | v7 | v8 | 이유 |
|---|---|---|---|
| job 적재 | 패킷마다 `Push` | Recv 한 번의 패킷을 `Push_Batch` | 패킷당 잠금 비용 (v7 피드백) |
| 수신 버퍼 | 세션마다 64 KiB | 8 KiB (최대 패킷 크기) | 연결당 메모리 (v7 피드백) |
| 송신 | 64 KiB 버퍼에 복사 후 `WSASend` | `WSABUF` 배열로 큐 버퍼를 직접 가리킴, `in_flight`로 수명 유지 | 복사 제거 + 세션 송신 버퍼 제거 |
| 송신 게시 | logic 스레드가 직접 `WSASend` | 틱이 세션 목록을 묶어 worker에게 넘김 | 아래 트러블슈팅 |
| pending I/O 추적 | 전역 set + mutex를 `WSASend`/`WSARecv` 동안 잡음 | Accept만 set, 개수는 원자 카운터 | 전역 잠금 경합 |

## 환경변수

| 이름 | 기본값 | 의미 |
|---|---:|---|
| `V8_LOG_LEVEL` | info | debug / info / error |
| `V8_TICK_MS` | 50 | 틱 주기(ms) |
| `V8_WORLD_SIZE` / `V8_CELL_SIZE` | 1000 / 100 | 월드 한 변과 셀 한 변 |
| `V8_MAX_SPEED` | 60 | 초당 최대 이동 거리 |
| `V8_SEED` | 8 | 시작 위치 난수 seed |
| `V8_STATUS_INTERVAL_MS` | 10000 | `[Status]` 출력 간격 |
| `V8_JOB_HIGH_WATERMARK` / `V8_JOB_LOW_WATERMARK` | 8192 / 1024 | Recv 일시정지 수위 |
| `V8_CHATS_PER_TICK` | 4 | 세션별 틱당 채팅 수 상한 |
| `V8_INPUT_TIMEOUT_MS` | 500 | 이 시간 동안 `C_Move`가 없으면 그 플레이어를 멈춘다(입력 만료). 0이면 끔. 틱 수로 올림 |
| `V8_SPAWN_X` / `V8_SPAWN_Y` / `V8_SPAWN_RADIUS` | 없음 | 셋 다 있으면 무작위 스폰 대신 그 원 안에 스폰(개발용). 없으면 무작위 |

`[Status]`의 `phase_avg_us=input/step/collect/encode/send`는 틱 한 번의 단계별 평균 시간이다.

## 실행과 테스트

```powershell
cmake -S server -B build/server -A x64
cmake --build build/server --config Release --target v8_server v8_unit_tests v8_load_bot
ctest --test-dir build/server -C Release -R v8_unit_tests
python server/v8/tests/v8_smoke_test.py <절대경로>/v8_server.exe --bot <절대경로>/v8_load_bot.exe
python basicdata/tools/measure_v8_scaling.py <v8_server.exe> <v8_load_bot.exe> --rounds 2
```

부하 봇만 따로: `v8_load_bot --clients 1000 --threads 8 --seconds 15 --move-hz 2`.
봇은 마지막에 모두 멈춘 뒤, 받은 spawn/despawn으로 재구성한 주변 목록과 위치가 격자 규칙의 기대값과 같은지,
각자 보낸 마지막 입력 순번까지 `S_SelfState`로 응답받았는지(`ack_mismatch`) 검사한다.
검증은 서버에 봇만 접속해 있다고 가정한다.

## 검증 결과 (2026-11-20, native Windows 11, MSVC C++20, 16코어 PC 한 대에서 서버와 봇 함께 실행)

- 단위 테스트 20개 통과(Release, ASan). 무작위 이동 300틱 후 재구성한 시야 = 서버 시야, 16비트 좌표 복원 오차,
  입력 순번 응답(정지 입력도 응답, 같은 틱 여러 입력은 마지막 순번) 확인 포함
- 스모크 테스트 통과(Release, ASan): 서로 spawn, 서버 권위 이동, 시야 채팅, 퇴장 despawn, 속도 상한,
  비정상 헤더·길이가 틀린 `C_Move`·알 수 없는 id에서 연결 종료, 봇 300~500명 시야 검증, 종료 코드 0
  - Recv backpressure(수위 64/16): 이동 입력 20,000개를 몰아 보내면 수신을 30번 멈췄다 재개하고,
    거부 0개로 20,000개 모두 처리
  - 느린 클라이언트: 응답을 읽지 않는 연결만 송신 상한(4 MiB)에서 끊기고(overflow 1회), 서버는 정상 종료
  - 채팅 속도 제한: 채팅 1,000개를 한꺼번에 보내도 발신자는 끊기지 않고 996개가 버려짐(`chats_dropped=996`)
  - 입력 순번: 이동 후 정지 입력(#2)까지 반영했다는 `S_SelfState` 응답, 봇 300~500명 `ack_mismatch=0`
- GUI(MMO Server Lab)에서 v8 실험방 실행 확인: 3명 접속 시 각자 `ENTER WORLD`와 위치 표시. 기본 설정(셀 100)에서는
  서로 멀어 채팅이 발신자에게만 돌아왔고, `V8_CELL_SIZE=1000`으로 띄우면 `SPAWN: 2명 → 주변 2명`과 함께
  Client 1의 채팅을 Client 2가 받았으며 공통 채팅에는 1번만 표시. 방 종료 시 서버 종료 코드 0
  - 미니맵과 이동 버튼: `→`를 2초 누른 뒤 `■`로 멈추자 위치가 873.4 → 993.4(초당 60 × 2초)가 되고 `반영된 입력 #2`
    표시. 셀 100에서는 10 × 10 격자와 내 시야(가장자리라 2 × 3칸), 시야 안 플레이어만 그려짐
- 인원별 측정(봇이 초당 2번 무작위 방향 입력, 15초, 2회). 틱 시간은 서버 `[Status]` 1초 구간 값의 평균/최대

| 인원 | AOI | 틱 평균 | 틱 최대 | 늦은 틱 | 틱당 송신 | 평균 주변 인원 | 시야 검증 |
|---:|---|---:|---:|---:|---:|---:|---|
| 500 | 켬 | 1.5~1.8ms | 6.8~7.9ms | 0 / 300 | 168 KB | 38 | 통과 |
| 1,000 | 켬 | 5.6~6.3ms | 14.3~16.0ms | 0 / 300 | 643 KB | 78 | 통과 |
| 2,000 | 켬 | 14.8~15.6ms | 31.5~31.9ms | 0 / 300 | 2.5 MB | 155 | 통과 |
| 500 | 끔 | 3.5~3.9ms | 18.2~20.1ms | 0 / 300 | 1.9 MB | 499 | 통과 |
| 1,000 | 끔 | 11.6~12.0ms | 26.6~26.9ms | 2~3 / 298 | 7.6 MB | 999 | 통과 |

  - 위 표는 보완(16비트 좌표, 수집 버퍼 재사용, 자기 상태 분리, 채팅 제한) 후 값이다. 보완 전에는 2,000명이
    틱 평균 14.1~16.2ms, 최대 35.3~48.1ms, 틱당 3.7 MB였고, AOI 끔 1,000명은 평균 17.5~18.2ms, 틱당 11.5 MB였다.
    2,000명 단계별 시간은 수집 7~8 → 3.5ms, 인코딩은 2.6ms 안팎으로 비슷하다(16비트 변환을 처음에 `std::round`와
    나눗셈으로 했더니 6ms까지 늘어, 배율을 목록마다 한 번만 계산하도록 바꿨다).
  - 같은 500명에서 AOI를 켜면 틱당 송신량이 약 1/12이 된다. 인원이 2배가 되면 AOI 켬은
    약 4배(밀도도 함께 오르기 때문), AOI 끔은 N²으로 늘어난다.
  - 목표(2,000명 20Hz에서 틱 처리 50ms 이하)를 만족했다. 다만 이 월드 크기에서 2,000명이면 셀당 최대 33명,
    주변 155명으로 꽤 빽빽한 설정이다.
- 유휴 연결 메모리(작업 집합): 0 / 100 / 500 / 1,000개에서 v7 5.5 / 18.3 / 69.2 / 132.5 MB,
  v8 5.6 / 6.8 / 11.8 / 17.5 MB. 연결당 약 127 KB → 약 12 KB.

## 트러블슈팅: 첫 측정에서 틱이 주기를 넘었다

처음 구현은 2,000명에서 틱 평균 86ms(늦은 틱 172/177)였다. `[Status]`에 단계별 시간을 추가해 보니
대부분이 송신 단계였다(1,000명 기준 36ms 중 32ms).

1. `WSASend`를 부르는 동안 전역 `context_mutex`를 잡고 있었다 → 목록 등록 순간만 잡도록 줄임. 효과 작음(35 → 30ms)
2. 로컬 연결의 `WSASend`는 호출 안에서 상대 버퍼까지 복사해 세션당 약 30µs가 걸린다. 틱 스레드 혼자 1,000번 부르면
   그것만으로 30ms → 세션마다 `PostQueuedCompletionStatus`로 worker에게 넘김. 1,000명 35 → 14ms
3. 그래도 2,000명에서는 송신 단계가 15~30ms였다. 세션마다 넘기는 호출 자체와 전역 pending 목록 잠금 경합이 남았다
   → pending 추적을 원자 카운터로 바꾸고, 목록 패킷 인코딩을 바이트별 `push_back`에서 한 번에 크기를 잡고 쓰는 방식으로 변경
   (인코딩 8 → 2.6ms)
4. 게시가 필요한 세션을 worker 수만큼의 묶음으로 나눠 묶음당 한 번만 넘김 → 1,000명 5ms, 2,000명 13~16ms

v7에서는 "송신을 worker에게 넘기기"가 오히려 30% 느렸다. 그때는 채팅 메시지마다 넘겼기 때문이다.
v8은 틱마다 묶음 몇 개만 넘기므로 넘기는 비용보다 `WSASend`를 여러 코어에 나누는 이득이 훨씬 크다.

## 남은 점

- 측정은 서버와 봇을 같은 PC에서 돌렸다. 봇의 수신·해석이 CPU를 함께 쓰므로 서버 단독 수치는 더 좋을 수 있다.
- 2,000명 틱에서 이제 가장 큰 단계는 송신 묶음 전달과 수집·인코딩(각 3~4ms)이다. 모두 틱 스레드 하나에서
  돈다. 관찰자를 나눠 worker에서 인코딩하면 더 줄일 수 있지만, 지금 부하에서는 주기(50ms)의 1/3 안쪽이다.
- 멀리 있는 대상의 갱신 주기를 늦추는(거리별 갱신) 방식은 넣지 않았다. 관찰자마다 "마지막으로 보낸 위치"를
  기억해야 해서 메모리와 복잡도가 커진다.
- 클라이언트 예측·보정은 서버가 순번을 돌려주는 데까지만 했다. GUI는 서버 위치를 그대로 그린다.
