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
- v6~v7은 Linux 배포 트랙과 별개로 native Windows의 IOCP 모델을 비교 학습한다.
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
- **목표**: 실제 상용 MMO 서버에 가까운 구조 (reactor가 I/O만 담당, 워커 스레드가 게임 로직 처리).
- **구현 세부사항**:
  - reactor(epoll 루프)는 I/O(수신/송신)만 담당, 파싱된 요청을 job queue로 워커 스레드 풀에 전달
  - v3의 thread pool 구조 재사용, v4의 이벤트 루프와 결합
  - 이후 실제 게임 로직(예: 좌표 이동, 채팅 브로드캐스트) 붙이기 실험 가능
- **테스트/QA**:
  - 종합 부하 테스트: 동시 접속 수 + 초당 메시지 처리량(TPS) 측정
  - I/O 스레드와 워커 스레드 간 job queue 병목 여부 확인 (큐 길이 모니터링)
  - 장시간 실행 시 메모리 누수 여부 확인 (`valgrind` 또는 `AddressSanitizer`)

### v6 — Windows IOCP 기반 비동기 I/O 서버

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

### v7 — IOCP + 워커/game logic queue 결합

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

각 버전은 `server/` 아래 `v1`, `v2` 등 버전별 하위 폴더(`include`/`src`/`CMakeLists.txt`)로 분리하고, 최상위 `server/CMakeLists.txt`가 `add_subdirectory()`로 각 버전을 불러온다. 버전마다 독립된 실행 파일(`v1_server`, `v2_server`, ...)로 빌드되어 언제든 원하는 버전을 골라 실행할 수 있다.

## 공통 테스트/QA 도구
- **동시성 버그 검출**: 필요할 때 별도 빌드에 ThreadSanitizer(`-fsanitize=thread`)를 적용.
  AddressSanitizer(`-fsanitize=address`)도 이후 버전에서 별도 검증용으로 사용
- **메모리 누수 검출**: `valgrind`(Linux) 또는 Xcode Instruments(macOS)
- **수동 접속 테스트**: `nc`, `telnet`
- **부하 테스트**: 간단한 멀티 커넥션 클라이언트를 직접 C++로 작성해 버전마다 재사용 (동시 접속 수, 메시지 처리량을 버전별로 비교하는 벤치마크 역할도 겸함)
- **단위 테스트**: 버전이 올라가며 로직이 복잡해지는 v3~v7에서 GoogleTest 도입 검토
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

## 빌드 도구 / 에디터
- cmake + Homebrew 설치 완료. `server/` 아래에서 `cmake -S . -B build && cmake --build build`로 빌드.
- 개발은 맥북 + Windows 두 컴퓨터에서 진행할 예정. Visual Studio 2022/2026은 Windows 전용이라 맥에서는 사용 불가 ("Visual Studio for Mac"은 단종되었고 애초에 C++용도 아니었음) → 맥에서는 **VS Code**(CMake 확장)를 사용한다.
- Windows에서는 v4~v5를 WSL2에서 개발하고, v6~v7은 native Windows와 Visual Studio에서
  IOCP를 직접 구현한다.

## 현재 상태
- [x] v1 — 싱글 스레드 blocking TCP 서버
- [x] v2 — thread-per-connection
- [x] v3 — 고정 크기 thread pool + 작업 큐
- [x] v4 — epoll 기반 이벤트 루프
- [ ] v5 — 이벤트 루프 + 워커 스레드 풀 결합
- [ ] v6 — Windows IOCP 기반 비동기 I/O 서버
- [ ] v7 — IOCP + 워커/game logic queue 결합
