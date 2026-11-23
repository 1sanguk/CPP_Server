# 작업 계획 (Roadmap)

## 목표
C++ 실전 경험이 적은 상태에서, MMO RPG 서버의 기초 밑단(네트워크 + 멀티스레드)을 직접 구현해보며 C++ 실력을 쌓는다.
한 번에 완성형을 만들지 않고, 버전을 올려가며 여러 개의 "뼈대"를 단계적으로 만들어 각 구조의 장단점을 몸으로 익힌다.

## 타겟 플랫폼
- 클라이언트는 Windows에서 플레이됨. TCP/IP는 프로토콜 레벨이라 클라이언트 OS는 서버 코드에 영향을 주지 않으므로 무시해도 됨.
- 서버 배포 환경은 **Linux로 확정**. 따라서 Linux 완성 트랙(v4~v5)은
  **Linux epoll**을 기준으로 한다.
- 이유: "대부분 MMO 서버가 Linux에 배포된다"는 일반론이 아니라, 실제 이 프로젝트의 배포 타겟이 Linux이기 때문.
- v1~v3는 POSIX/BSD 소켓 API 기반이라 macOS(로컬)에서 구현과 검증을 완료했다.
  Linux에서도 같은 API를 사용할 수 있지만 배포 전 별도 회귀 테스트는 필요하다.
- v4~v5는 Linux 환경이 필요. Windows 컴퓨터에서는 WSL2를 사용해 서버 배포 환경과
  같은 Linux API로 개발한다.
- v6~v8은 Linux 배포 트랙과 별개로 native Windows의 IOCP 모델을 비교 학습한다. v8은 v7의 IOCP 계층 위에
  틱 루프와 AOI를 얹었고, v9에서 같은 게임 로직을 Linux epoll로 이식했다.
  Windows와 Visual Studio 환경에서 Winsock2 및 IOCP API를 직접 사용한다.

## 버전별 계획

### v1 — 싱글 스레드 blocking TCP 서버
- **상태**: 완료
- **목표**: 동시성 없이 소켓 API 자체(`socket`/`bind`/`listen`/`accept`/`recv`/`send`/`close`) 흐름을 익히는 baseline.
- **구현 세부사항**:
  - 접속 1개만 처리 가능 (accept → echo 루프 → 연결 종료 시 다음 accept로 복귀)
  - 받은 메시지를 그대로 돌려주는 echo 서버
  - 연결 종료(recv 0바이트), 에러 처리(errno) 구분
- **테스트/QA**:
  - `nc localhost <port>` 또는 `telnet`으로 수동 접속 후 echo 확인
  - 두 번째 클라이언트가 접속을 시도하면 첫 번째 처리가 끝날 때까지 멈춰있는지 확인 (blocking의 한계를 직접 체감하는 것이 이 버전의 QA 포인트)

### v2 — thread-per-connection
- **상태**: 완료
- **목표**: 접속마다 `std::thread` 하나씩 생성. 기본적인 스레드/락/레이스 컨디션 감각 습득.
- **구현 세부사항**:
  - accept 후 클라이언트 핸들러를 `std::thread`로 분리, `detach` 또는 종료 시 `join`으로 정리
  - 공유 자원(전체 접속자 목록 등)에 `std::mutex` + `std::lock_guard` 적용
  - 접속별 스레드는 `detach()`하고, 활성 client fd 목록은 mutex로 보호
  - `detach()`의 수명 관리 한계는 구조 비교를 위해 의도적으로 유지하고 v3에서 worker 소유와
    graceful shutdown을 학습
- **테스트/QA**:
  - 여러 클라이언트를 동시에 접속시켜 병렬로 echo가 되는지 확인 (nc 여러 개 또는 간단한 부하 스크립트)
  - 다수 클라이언트가 급격히 접속/종료를 반복할 때 스레드 누수(leak) 없는지 확인
  - 완료 결과: 동시 echo와 최대 10개 병렬의 50개 접속·종료 반복 성공, 활성 count 0과
    client fd 정리 확인

### v3 — 고정 크기 thread pool + 작업 큐
- **상태**: 완료
- **목표**: 스레드 개수를 접속자 수와 분리. 생산자-소비자 패턴, 조건 변수 학습.
- **구현 세부사항**:
  - 고정 worker 스레드 4개 생성
  - 작업 큐: `std::queue` + `std::mutex` + `std::condition_variable`
  - accept 스레드(생산자)가 큐에 작업을 넣고, 워커 스레드(소비자)가 꺼내 처리
  - 최대 128개의 bounded queue와 초과 연결 거부 정책
  - SIGINT/SIGTERM 동기 처리와 worker/monitor join을 포함한 graceful shutdown
  - 30초 주기 worker 상태 monitor와 종료 알림에 즉시 반응하는 `wait_for()`
- **테스트/QA**:
  - 동시 접속 수가 스레드 풀 크기를 초과할 때(예: 접속 100개, 워커 4개) 큐잉이 정상 동작하는지 확인
  - 큐가 비었을 때 워커 스레드가 busy-wait 없이 `condition_variable::wait`로 대기하는지 CPU 사용률로 확인
  - v2와 동일한 부하에서 스레드 생성 오버헤드가 줄었는지 비교 (스레드 개수 고정 확인)
  - 완료 결과: 동시 echo 50개 50/50 성공, 장기 연결 140개 중 worker 4개와 큐 128개
    유지 및 초과 8개 거부, 활성 연결 6개 상태의 `Ctrl+C` 정상 종료 확인

### v4 — epoll 기반 이벤트 루프 (단일 스레드 reactor)
- **상태**: 완료
- **목표**: I/O multiplexing으로 여러 소켓을 하나의 이벤트 루프에서 감시하고,
  readiness 기반 처리 흐름을 체감한다.
- **구현 세부사항**:
  - `epoll_create1`, `epoll_ctl`, `epoll_wait` 사용
  - listen fd, client fd, `eventfd` 기반 stop fd를 하나의 이벤트 루프에서 분기 처리
  - 단일 스레드에서 여러 client fd의 accept/recv/send echo 처리
  - `std::unordered_set<int>`로 활성 client fd 목록 관리
  - SIGINT/SIGTERM을 main 스레드의 `sigwait()`로 받고 `Stop()`이 `eventfd`에 write해
    `epoll_wait()`를 즉시 깨우는 graceful shutdown 구성
  - fd 정리는 `CleanUp()`으로 분리하고 중복 호출 방지 플래그 적용
  - 현재 버전은 level-triggered + blocking client fd/send 방식으로 구현했다.
    non-blocking client fd, `EPOLLOUT`, 송신 버퍼 기반 partial send는 v5 전 준비 또는
    v5 구조에서 확장한다.
- **테스트/QA**:
  - Docker Ubuntu 24.04 기반 `cpp-server-dev` 이미지에서 `v4_server` 빌드 성공
  - Docker 포트 매핑(`-p 9000:9000`) 후 macOS 터미널의 `nc`로 echo 확인
  - interactive `nc` 여러 개를 열어 각 클라이언트가 독립적으로 echo되는지 확인
  - 5개 동시 `nc` 요청에서 5/5 echo 성공
  - `Ctrl+C` 종료 시 `eventfd` 이벤트 수신, event loop 종료, client/listen/stop/epoll fd
    cleanup 로그 확인

### v5 — 이벤트 루프 + 워커 스레드 풀 결합
- **상태**: 완료
- **목표**: 실제 상용 MMO 서버에 가까운 구조 (reactor가 I/O만 담당, 워커 스레드가 게임 로직 처리).
- **구현 세부사항**:
  - reactor(epoll 루프)는 I/O(수신/송신)만 담당, 파싱된 요청을 job queue로 워커 스레드 풀에 전달
  - v3의 thread pool 구조 재사용, v4의 이벤트 루프와 결합
  - client fd를 non-blocking으로 설정하고 `EPOLLOUT` + 세션별 송신 버퍼(`send_buffers`)를
    도입해, 워커는 버퍼에 데이터를 쌓고 `EPOLLOUT` 등록만 하며 실제 `send()`는 reactor가
    전담하도록 분리
  - `Send_All()`을 `bool` 대신 `SendState`(Completed/Partial/Failed) enum으로 재설계해
    non-blocking 소켓의 `EAGAIN`과 진짜 에러를 구분
  - 같은 커넥션의 job이 여러 워커에 흩어져 처리 순서가 뒤바뀌는 걸 막기 위해, 워커별로
    큐/뮤텍스/조건변수를 따로 두고 `client_fd % 워커 수`로 항상 같은 워커에 라우팅하는
    sticky routing 적용
  - 여러 스레드(reactor/워커/monitor)가 동시에 로그를 찍어도 한 줄이 섞이거나 깨지지
    않도록 뮤텍스로 보호된 `Logging()` 단일 출력 지점으로 통일
  - `Clean_Up()`이 종료 시 각 client fd를 닫기 전에 남은 세션 송신 버퍼를 `Send_All()`로
    한 번 더 flush 시도(best-effort)하도록 해서, 종료 타이밍이 마지막 메시지 전송과
    겹쳐도 데이터가 유실되지 않게 함
  - 이후 실제 게임 로직(예: 좌표 이동, 채팅 브로드캐스트) 붙이기 실험 가능
- **테스트/QA**:
  - `v5_server` 빌드(`-Wall -Wextra` 경고 없음), 단일/동시 접속 echo, `SIGTERM` graceful
    shutdown 확인
  - 종합 부하 테스트: v4와 동일 시나리오(50 clients × 20 msg)에서 1000/1000 성공,
    2000 clients × 20 msg(총 40,000 echo) 순간 부하에서 40000/40000 성공, 300 clients를
    30초 이상에 걸쳐 늘리는 지속 부하에서 3000/3000 성공
  - I/O 스레드와 워커 스레드 간 job queue 병목 여부: `top -H`로 스레드별 CPU를 측정한 결과
    job queue 자체는 병목이 아니었고(`Monitor_Workers` 로그가 대부분 `Queued: 0`), reactor
    스레드 하나가 모든 I/O를 처리하는 구조가 실제 병목이라는 걸 확인
  - 장시간 실행 메모리 누수: `valgrind`가 없는 환경이라 AddressSanitizer + LeakSanitizer로
    대체. 120개 연결·1,800개 echo 규모의 반복 연결/해제 부하 후 정상 종료까지 확인했고
    메모리 누수·메모리 안전성 에러는 발견되지 않음
  - 완료 결과: 자세한 회고와 발견된 버그 목록은 [basicdata/feedback.md](feedback.md)의
    v5 항목 참고

### v6 — Windows IOCP 기반 비동기 I/O 서버

- **상태**: 완료
- **목표**: Windows의 completion-based 비동기 I/O 모델을 익히고 Linux `epoll`의
  readiness 모델과 차이를 비교한다.
- **실행 환경**: native Windows + Visual Studio. WSL2에서는 IOCP를 사용할 수 없다.
- **구현 세부사항**:
  - Winsock2 초기화와 overlapped socket 생성
  - `CreateIoCompletionPort()`로 completion port 생성 및 socket 연결
  - `AcceptEx()` 또는 초기 단계의 accept 흐름으로 client 연결 등록
  - `WSARecv()`/`WSASend()`에 `OVERLAPPED`와 수명 관리 가능한 I/O context 사용
  - `GetQueuedCompletionStatus()`로 완료된 I/O 결과 처리
  - 연결 종료, 오류, partial send와 pending I/O 정리
- **테스트/QA**:
  - 다중 client echo 및 반복 접속·종료 확인
  - pending I/O가 남은 상태에서 연결과 서버를 종료해 context 수명 오류가 없는지 확인
  - v4의 `epoll_wait()`와 IOCP 완료 통지 흐름을 같은 시나리오로 비교
  - Visual Studio AddressSanitizer 및 Application Verifier 등 Windows 도구 적용 검토

- **완료 결과 (2026-08-17)**:
  - `AcceptEx()` context 16개를 선게시하고 완료된 client socket을 IOCP에 등록
  - 세션과 작업별 context를 분리하고, Recv/Send 동시 진행과 세션별 송신 순서를 보장
  - `CONTAINING_RECORD`로 `OVERLAPPED`의 구조체 내 위치에 의존하지 않고 작업 context 복원
  - 종료 시 socket을 닫아 pending I/O를 취소하고 5초 주기로 잔여 수를 진단하며 모두 회수
  - 일반 빌드 200 clients × 10 msg에서 2000/2000 echo 및 pending receive 상태 종료 성공
  - MSVC AddressSanitizer 빌드 50 clients × 20 msg에서 1000/1000 echo와 정상 종료,
    메모리 안전성 오류 없음
  - 강제 부분 전송 빌드에서 느린 수신 client의 4 MiB echo 일치 및 continuation 4,032회 확인

### v7 — IOCP + 워커/game logic queue 결합

- **상태**: 완료
- **목표**: IOCP worker가 네트워크 완료를 처리하고 실제 게임 로직은 별도 job queue에서
  실행하도록 역할을 분리한다.
- **구현 세부사항**:
  - IOCP 완료 처리와 패킷 조립/파싱 책임 분리
  - 파싱된 요청을 game logic job queue로 전달
  - 고정 game worker pool과 `condition_variable` 또는 Windows 동기화 도구 비교
  - 세션별 수신/송신 버퍼의 소유권과 동시 처리 순서 보장
  - graceful shutdown 시 pending I/O 취소, IOCP worker와 game worker join
- **테스트/QA**:
  - 채팅 또는 좌표 이동 같은 간단한 상태 변경 로직 연결
  - 동일 세션의 패킷 처리 순서와 여러 세션의 병렬 처리 확인
  - I/O completion queue와 game job queue 길이 및 병목 측정
  - v5 Linux 구조와 동일 부하에서 처리량, 지연시간, 스레드 수, 메모리 비교

- **완료 결과 (2026-11-19)**:
  - `[uint16 size][uint16 id][body]` 길이 헤더 패킷과 세션별 `PacketAssembler`로 TCP 스트림을 패킷 단위로 조립
  - completion worker는 조립한 패킷을 bounded `GameJobQueue`(65,536)에 넣고, 단일 game logic 스레드가
    방 상태를 소유하며 입장/퇴장 알림과 채팅을 방송
  - Enter를 첫 Recv 게시 전에 넣어 세션별 처리 순서를 보장하고, 브로드캐스트 버퍼는 공유 포인터로 재사용
  - 종료 시 job 큐 Close → logic 스레드 join → v6와 같은 pending I/O 취소·drain → IOCP worker join
  - 50 clients × 20 msg(50,000건 전달)와 200 clients × 10 msg(400,000건 전달)에서 발신자별 순서 유지,
    1바이트 분할 패킷, 비정상 헤더 종료, 느린 클라이언트 송신 큐 상한, ASan 빌드 종료 시나리오 통과
  - GUI 클라이언트에 v7 패킷 모드를 추가하고 카탈로그에서 v7을 실행 가능으로 변경
  - v5와 같은 부하 스크립트(`basicdata/tools/compare_v5_v7.py`)로 지연시간, 처리량, 스레드 수, 메모리를
    비교했다(결과는 benchmark.md). 연속 소패킷 처리량과 연결당 메모리는 feedback.md에 `[미적용]`으로 남겼다

### v8 — 틱 기반 게임 루프 + 격자 AOI

- **상태**: 완료
- **목표**: v7의 "job이 오는 즉시 처리하고 방 전체에 방송" 구조를 MMO 서버의 기본 형태로 바꾼다.
  - 게임 로직을 고정 주기 틱으로 돌려, 한 틱 동안 쌓인 입력을 몰아서 처리하고 결과를 한 번에 보낸다.
  - 플레이어에게 좌표를 주고, 근처(AOI, Area of Interest)에 있는 플레이어에게만 상태를 보낸다. v7은 인원이
    N이면 전송량이 N²으로 늘었다.
- **실행 환경**: native Windows. v7의 IOCP 네트워크 계층(패킷 조립, 세션 context, 묶어 보내기, backpressure,
  비동기 로거)을 재사용하고 game logic 부분을 새로 만든다. Linux(epoll) 이식은 이후 버전에서 검토한다.
- **구현 세부사항**:
  - **틱 루프**
    - logic 스레드가 고정 주기(예: 20Hz, 50ms)로 깨어나 "입력 처리 → 월드 갱신 → 상태 전송" 순서로 한 틱을 실행
    - job 큐는 틱 시작 시 한꺼번에 꺼낸다(drain). 큐 잠금이 패킷마다가 아니라 틱마다 한 번이 된다
    - 다음 틱까지 남은 시간은 `condition_variable::wait_until()` 등으로 기다리고, 종료 요청에는 즉시 반응
    - 틱 처리가 주기보다 오래 걸릴 때의 정책(밀린 틱을 따라잡기 vs 건너뛰기)을 정하고, 늦은 틱 수를 기록
  - **월드와 이동**
    - 2D 평면(예: 1000 × 1000)에서 플레이어마다 좌표와 이동 상태를 가진다
    - 클라이언트는 이동 요청만 보내고, 실제 위치는 서버가 틱마다 계산한다(서버 권위). 속도 상한을 넘는 요청은 보정
    - 같은 틱에 같은 플레이어가 보낸 이동 요청이 여러 개면 마지막 요청만 반영(입력 합치기)
  - **격자 AOI**
    - 월드를 시야 반경 크기의 셀로 나누고, 각 셀이 안에 있는 플레이어 목록을 가진다
    - 플레이어의 시야는 자기 셀과 주변 8개 셀(3 × 3)
    - 셀을 넘어갈 때 새로 보이게 된 대상과 안 보이게 된 대상을 계산해 생성/삭제 알림을 보낸다
    - 틱마다 이번 틱에 움직인 플레이어의 위치만 주변 셀의 플레이어에게 보낸다
  - **패킷과 전송**
    - 추가 패킷: `C_Move`(이동 요청), `S_EnterWorld`(내 id와 시작 좌표), `S_Spawn`/`S_Despawn`(시야 진입/이탈),
      `S_MoveBatch`(한 틱의 주변 이동을 묶은 패킷). 채팅은 시야 범위 채팅으로 바꾼다
    - 세션별로 한 틱의 업데이트를 패킷 하나로 묶어 보낸다(v7 묶어 보내기의 확장)
  - **v7 보완점 함께 처리** (feedback.md의 v7 `[미적용]`)
    - 한 번의 Recv에서 나온 패킷들을 job 큐에 한꺼번에 넣어 패킷당 고정 비용을 줄인다
    - 세션마다 고정한 64 KiB 수신·송신 context의 연결당 메모리를 줄이는 방법을 정한다
  - **상태 지표**: `[Status]`에 틱 처리 시간(평균/최대), 늦은 틱 수, 셀당 최대 인원, 틱당 전송 바이트를 추가
- **테스트/QA**:
  - 단위 테스트: 좌표 → 셀 계산, 셀 경계를 넘을 때의 시야 진입/이탈 집합, 가짜 시계로 틱 스케줄러 검증
  - C++ 부하 봇 작성: 수천 명이 무작위로 이동하는 클라이언트(Python 부하 클라이언트의 처리량 한계 해결,
    아래 공통 테스트/QA 도구 참고)
  - 시야 정확성: 봇이 받은 생성/삭제 알림으로 재구성한 주변 목록이 서버가 계산한 시야와 일치하는지 확인
  - AOI를 끈 설정(전체 방송)과 켠 설정을 같은 인원으로 비교해 틱당 전송량 차이를 측정
  - 인원 증가(예: 500 → 1,000 → 2,000명)에 따른 틱 처리 시간과 늦은 틱 비율 측정.
    목표 예: 2,000명 20Hz에서 틱 처리 시간이 주기(50ms)를 넘지 않음
  - graceful shutdown(틱 루프 종료 → v7과 같은 I/O 정리)과 AddressSanitizer 빌드 확인
  - GUI에서 v8 클라이언트 위치를 2D 미니맵으로 표시하는 방법 검토
- **구현 결과** (2026-11-20, 자세한 내용은 [server/v8/README.md](../server/v8/README.md)):
  - 20Hz 틱 루프(놓친 틱은 건너뛰고 dt로 보정), 1000 × 1000 월드, 셀 100의 3 × 3 격자 AOI, 서버 권위 이동과 속도 상한
  - `C_Move`, `S_EnterWorld`, `S_Spawn`/`S_Despawn`, `S_MoveBatch`, 시야 채팅. 세션마다 한 틱 분량을 버퍼 하나로 전송
  - v7 보완: Recv 한 번의 패킷을 잠금 1회로 적재(`Push_Batch`), 수신 버퍼 8 KiB + `WSABUF` scatter-gather 송신으로
    유휴 연결 1,000개 메모리 132.5 MB → 17.5 MB
  - 단위 테스트 18개, 스모크 테스트, C++ 부하 봇 시야 검증(최대 2,000명)이 Release·ASan에서 통과
  - 2,000명 20Hz에서 틱 평균 14~16ms, 최대 35~48ms(목표 50ms 이하). 같은 500명에서 AOI를 켜면 틱당 송신량이
    2.9 MB → 241 KB. 처음에는 틱 스레드가 `WSASend`를 직접 불러 2,000명 틱 평균이 86ms였고, 송신 게시를
    worker에게 묶음으로 넘겨 해결했다(README의 트러블슈팅)
  - GUI 카탈로그에 v8 추가(미니맵, 이동 버튼, 입력 순번 표시). CI 워크플로
    (`.github/workflows/v8-windows.yml`)는 push 때 자동 실행하지 않고 필요할 때 GitHub Actions에서 수동 실행
  - 완료 후 보완(2026-11-20): 세션별 틱당 채팅 제한, 시야 변경 수집 버퍼 재사용, 주변 좌표 16비트 압축(틱당 송신
    2,000명 3.7 → 2.5 MB), 입력 순번과 `S_SelfState`, GUI 미니맵·이동 버튼. 자세한 내용은 feedback.md

### v9 — v8 틱 서버의 Linux(epoll) 이식

- **상태**: 완료 (2026-11-22). 이 버전은 예외로 Claude가 구현하고 설명했다(사용자 결정).
- **목표**: v8의 틱 루프 + 격자 AOI 서버를 배포 타겟인 Linux에서 epoll로 돌린다.
  - 같은 게임 로직을 완료 통지(IOCP, "작업이 끝났다")와 준비 통지(epoll, "이제 읽거나 쓸 수 있다") 두 방식의
    네트워크 계층 위에 올려 차이를 비교한다.
  - 소켓을 모르는 게임 로직과 플랫폼별 네트워크 계층의 경계를 코드로 분리한다.
- **실행 환경**: Linux(Windows에서는 WSL2). v4·v5와 같은 POSIX + epoll + eventfd.
- **재사용**
  - v8에서 그대로: `World`, `TickScheduler`, `packet`, `PacketAssembler`, `GameJobQueue`, `AsyncLogger`
    (모두 Winsock/IOCP를 쓰지 않는다)
  - v5에서: epoll 이벤트 루프, non-blocking fd, `EPOLLOUT` + 세션별 송신 버퍼, `SendState`(Completed/Partial/Failed),
    `eventfd`로 루프 깨우기, `SIGTERM` graceful shutdown
- **구현 세부사항**:
  - **스레드 구성**: reactor 스레드(epoll, accept·recv·send) + 틱 스레드(v8과 같음). v5의 워커 풀 대신 v8의 틱 스레드가
    게임 로직을 맡는다
  - **수신**: 읽을 수 있으면 `EAGAIN`까지 반복해서 읽고(edge-triggered면 필수), 조립된 패킷을 `Push_Batch`로 job 큐에 넣는다.
    job 큐가 높은 수위를 넘으면 그 fd의 `EPOLLIN`을 빼서 수신을 멈추고, 낮은 수위 아래로 내려가면 다시 켠다(v8 `Pause_Recv`의 epoll판)
  - **송신**: 틱 스레드는 세션 송신 큐에 한 틱 분량을 넣고, 실제 `send`/`writev`는 reactor가 한다.
    틱 끝에 "보낼 것이 생긴 세션 목록"을 넘기고 `eventfd`로 reactor를 깨운다(v8에서 worker에게 송신 게시를 넘긴 것과 같은 이유).
    한 번에 다 못 보내면 `EPOLLOUT`을 켜고, 다 보내면 끈다
  - **세션 수명**: fd 번호는 재사용되므로 v8처럼 증가하는 `session_id`를 쓰고, 틱 스레드와 reactor가 함께 보는 세션은
    `shared_ptr`로 수명을 붙잡는다. 닫기는 reactor가 한 번만 하고, 틱 스레드에는 Leave job으로 알린다
  - **종료 순서**: `Stop` → 틱 루프 종료(job 큐 Close, join) → reactor 루프 종료(eventfd) → 남은 송신 best-effort → fd 정리
  - **빌드**: 최상위 `server/CMakeLists.txt`의 Linux 분기에 `add_subdirectory(v9)`. 공통 코드는 v8 파일을 복사해 v9 폴더에 둔다
    (버전별 폴더 규칙 유지)
- **테스트/QA**:
  - ~~v8 단위 테스트를 Linux에서 그대로 통과~~ → v8 테스트 폴더를 지운 뒤라 대신 부하 봇의 시야 검증으로 확인
  - C++ 부하 봇(`tools/load_bot.cpp`, epoll)으로 접속·이동·시야 진입/이탈 확인(프로토콜은 v8과 같다)
  - 2,000명 20Hz에서 틱 처리 시간과 송신량을 v8(Windows IOCP) 결과와 비교해 benchmark.md에 기록
  - 수신 멈춤/재개, partial send, 느린 클라이언트(송신 큐 상한) 시나리오
  - `SIGTERM` graceful shutdown, ASan + LeakSanitizer
- **구현 결과** (2026-11-22, 자세한 내용은 [server/v9/README.md](../server/v9/README.md)):
  - reactor 1 + 틱 1 + 로거 1 + main(sigwait). epoll LT, 이벤트 키는 fd 대신 session_id(재사용된 fd와 섞이지 않게),
    `sendmsg` + iovec 묶어 보내기, `MSG_NOSIGNAL`, `accept4(SOCK_NONBLOCK)`
  - 봇 500 / 1,000 / 2,000명(초당 10회 이동 입력) 모두 시야 불일치 0. 2,000명 틱 평균 5.2~6.3ms, 최대 9.5ms, 늦은 틱 0
  - 읽지 않는 클라이언트: partial send → `EPOLLOUT` 대기 → 송신 큐 4 MiB 초과 시 그 연결만 종료. SIGTERM 종료, ASan + LSan 경고 없음
  - 수신 멈춤/재개: 기본 수위(8,192)에서는 job 큐 최대가 2,383이라 일어나지 않아, 수위를 200/50으로 낮춰 2,000명을 돌렸다.
    멈춤 242,174회, 끝에 멈춘 세션 0, 끊김·시야 불일치 0, 늦은 틱 0
  - 다시 검토(2026-11-23): 기능 검사(`tools/v9_check.py`)를 추가해 Release·ASan 통과. 리뷰에서 찾은 eventfd 수명 경쟁과
    닫기 요청 반복을 고친 뒤 전체 시험을 다시 통과했다(자세한 내용은 feedback.md)
  - 보완(2026-11-23): feedback.md의 v9 `[미적용]`을 모두 처리했다. 단위 테스트 13개(g++·MSVC), 게임 로직 벤치마크(v8↔v9 차이 분석),
    수신 멈춤을 읽기 전에 판단, 입장·퇴장 틱당 상한(동시 입장 70.8 → 29ms), 스레드별 CPU 측정(reactor 46~52%),
    서버·봇 코어 분리 측정, CI(`v9-linux.yml`), Server Lab에서 WSL2로 v9 실행

각 버전은 `server/` 아래 `v1`, `v2` 등 버전별 하위 폴더(`include`/`src`/`CMakeLists.txt`)로 분리하고, 최상위 `server/CMakeLists.txt`가 `add_subdirectory()`로 각 버전을 불러온다. 버전마다 독립된 실행 파일(`v1_server`, `v2_server`, ...)로 빌드되어 언제든 원하는 버전을 골라 실행할 수 있다.

## 공통 테스트/QA 도구
- **동시성 버그 검출**: 필요할 때 별도 빌드에 ThreadSanitizer(`-fsanitize=thread`)를 적용.
  AddressSanitizer(`-fsanitize=address`)도 이후 버전에서 별도 검증용으로 사용
- **메모리 누수 검출**: `valgrind`(Linux) 또는 Xcode Instruments(macOS)
- **수동 접속 테스트**: `nc`, `telnet`
- **부하 테스트**: 간단한 멀티 커넥션 클라이언트를 직접 C++로 작성해 버전마다 재사용 (동시 접속 수, 메시지 처리량을 버전별로 비교하는 벤치마크 역할도 겸함)
- **단위 테스트**: 버전이 올라가며 로직이 복잡해지는 v3~v8에서 GoogleTest 도입 검토
  (큐 동작, 파서 등 순수 로직 단위 위주)

## 벤치마크
버전을 감으로 비교하지 않고 숫자로 비교하기 위한 기록. 버전이 완료될 때마다 실측치를 채운다.
→ [basicdata/benchmark.md](benchmark.md)

## 버그 재현 / 트러블슈팅 기록
단순히 "안전하게 짜서 안 터졌다"가 아니라, 실제로 문제를 만들어보고 왜 안전한지 이해했다는 걸 남기기 위한 기록.
→ [basicdata/troubleshooting.md](troubleshooting.md)

## 깨달은 점
버그를 고치거나 개념을 짚으면서 실제로 이해하게 된 포인트 정리.
→ [basicdata/learnings.md](learnings.md)

## 피드백
기획한대로 구현한 서버를 AI에게 피드백 받아 어떤 점이 부족했는지 확인하기 위한 기록.
→ [basicdata/feedback.md](feedback.md)

## 게임 (마을 꾸미기) / Unity 클라이언트
서버 위에 실제 모바일 게임을 올린다. 모두가 한 마을에 살며 자기 집을 자유 배치로 꾸미고 이웃집에 놀러 가는 소셜 게임.
Android 먼저, Unity 2022.3, 프로젝트는 저장소 밖 `D:\unity\Project\CppServerClient`.
단계: M1a 폰 접속 / M1b 여러 명(v8 + 입력 만료) → M2a~c 내 집 열기·꾸미기·남기기(v9: 로그인·SQLite·DB 스레드·배치 판정)
→ M3 놀러 가기(v10: 공간 이동·방문·편집 방송) → M4 마을(v11: 부지·세션 복구) → M5 생활(채널·경제·Linux 이식은 별도).
→ 게임은 별도 저장소 `cpp_server_unity`(v8을 가져와 v9부터 확장)에서 진행한다. 기획 문서도 그쪽 `docs/`로 옮겼다
  (`game_plan.md`, `unity_client_plan.md`).

## 빌드 도구 / 에디터
- cmake + Homebrew 설치 완료. `server/` 아래에서 `cmake -S . -B build && cmake --build build`로 빌드.
- 개발은 맥북 + Windows 두 컴퓨터에서 진행할 예정. Visual Studio 2022/2026은 Windows 전용이라 맥에서는 사용 불가 ("Visual Studio for Mac"은 단종되었고 애초에 C++용도 아니었음) → 맥에서는 **VS Code**(CMake 확장)를 사용한다.
- Windows에서는 v4~v5를 WSL2에서 개발하고, v6~v8은 native Windows와 Visual Studio에서
  IOCP를 직접 구현한다.

## 현재 상태
- [x] v1 — 싱글 스레드 blocking TCP 서버
- [x] v2 — thread-per-connection
- [x] v3 — 고정 크기 thread pool + 작업 큐
- [x] v4 — epoll 기반 이벤트 루프
- [x] v5 — 이벤트 루프 + 워커 스레드 풀 결합
- [x] v6 — Windows IOCP 기반 비동기 I/O 서버
- [x] v7 — IOCP + 워커/game logic queue 결합
- [x] v8 — 틱 기반 게임 루프 + 격자 AOI
- [x] v9 — v8 틱 서버의 Linux(epoll) 이식
