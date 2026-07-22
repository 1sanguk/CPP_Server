# 작업 계획 (Roadmap)

## 목표
C++ 실전 경험이 적은 상태에서, MMO RPG 서버의 기초 밑단(네트워크 + 멀티스레드)을 직접 구현해보며 C++ 실력을 쌓는다.
한 번에 완성형을 만들지 않고, 버전을 올려가며 여러 개의 "뼈대"를 단계적으로 만들어 각 구조의 장단점을 몸으로 익힌다.

## 타겟 플랫폼
- 클라이언트는 Windows에서 플레이됨. TCP/IP는 프로토콜 레벨이라 클라이언트 OS는 서버 코드에 영향을 주지 않으므로 무시해도 됨.
- 서버 배포 환경은 **Linux로 확정**. 따라서 이벤트 루프 단계(v4~)는 **Linux epoll**을 기준으로 한다.
- 이유: "대부분 MMO 서버가 Linux에 배포된다"는 일반론이 아니라, 실제 이 프로젝트의 배포 타겟이 Linux이기 때문.
- v1~v3는 BSD 소켓 API 기반이라 macOS(로컬)에서도, Linux에서도 동일하게 동작하므로 별도 환경 없이 바로 진행 가능. v1은 맥북에서 완료.
- v4~는 Linux 환경이 필요. Windows 컴퓨터로 개발을 옮길 때는 WSL2를 사용하기로 함(Winsock 등 크로스플랫폼 소켓 코드를 따로 짤 필요 없이, 서버 배포 환경(Linux)과 100% 동일한 조건으로 개발 가능) — 아직 미착수.

## 버전별 계획

### v1 — 싱글 스레드 blocking TCP 서버
- **목표**: 동시성 없이 소켓 API 자체(`socket`/`bind`/`listen`/`accept`/`recv`/`send`/`close`) 흐름을 익히는 baseline.
- **구현 세부사항**:
  - 접속 1개만 처리 가능 (accept → echo 루프 → 연결 종료 시 다음 accept로 복귀)
  - 받은 메시지를 그대로 돌려주는 echo 서버
  - 연결 종료(recv 0바이트), 에러 처리(errno) 구분
- **테스트/QA**:
  - `nc localhost <port>` 또는 `telnet`으로 수동 접속 후 echo 확인
  - 두 번째 클라이언트가 접속을 시도하면 첫 번째 처리가 끝날 때까지 멈춰있는지 확인 (blocking의 한계를 직접 체감하는 것이 이 버전의 QA 포인트)

### v2 — thread-per-connection
- **목표**: 접속마다 `std::thread` 하나씩 생성. 기본적인 스레드/락/레이스 컨디션 감각 습득.
- **구현 세부사항**:
  - accept 후 클라이언트 핸들러를 `std::thread`로 분리, `detach` 또는 종료 시 `join`으로 정리
  - 공유 자원(전체 접속자 목록 등)에 `std::mutex` + `std::lock_guard` 적용
  - 서버 종료 시 모든 스레드를 안전하게 정리하는 graceful shutdown
- **테스트/QA**:
  - 여러 클라이언트를 동시에 접속시켜 병렬로 echo가 되는지 확인 (nc 여러 개 또는 간단한 부하 스크립트)
  - `mutex` 없이 공유 자원에 접근하는 코드를 일부러 넣어보고 `-fsanitize=thread`(ThreadSanitizer)로 race condition이 잡히는지 확인 → 이후 mutex 적용 코드와 비교
  - 다수 클라이언트가 급격히 접속/종료를 반복할 때 스레드 누수(leak) 없는지 확인

### v3 — 고정 크기 thread pool + 작업 큐
- **목표**: 스레드 개수를 접속자 수와 분리. 생산자-소비자 패턴, 조건 변수 학습.
- **구현 세부사항**:
  - 고정 개수 워커 스레드 풀 생성 (예: `std::thread::hardware_concurrency()` 기준)
  - 작업 큐: `std::queue` + `std::mutex` + `std::condition_variable`
  - accept 스레드(생산자)가 큐에 작업을 넣고, 워커 스레드(소비자)가 꺼내 처리
- **테스트/QA**:
  - 동시 접속 수가 스레드 풀 크기를 초과할 때(예: 접속 100개, 워커 4개) 큐잉이 정상 동작하는지 확인
  - 큐가 비었을 때 워커 스레드가 busy-wait 없이 `condition_variable::wait`로 대기하는지 CPU 사용률로 확인
  - v2와 동일한 부하에서 스레드 생성 오버헤드가 줄었는지 비교 (스레드 개수 고정 확인)

### v4 — epoll 기반 이벤트 루프 (단일 스레드 reactor)
- **목표**: I/O multiplexing으로 다수의 소켓을 논블로킹으로 처리, C10K 문제 체감.
- **구현 세부사항**:
  - `epoll_create`, `epoll_ctl`, `epoll_wait` 사용
  - 소켓을 `O_NONBLOCK`으로 설정, level-triggered로 시작 후 필요 시 edge-triggered 실험
  - 단일 스레드에서 수백~수천 개 연결을 이벤트 루프 하나로 처리
- **테스트/QA**:
  - 수백 개 이상의 동시 접속을 열어 단일 스레드로도 정상 응답하는지 부하 테스트 (예: 간단한 C++ 부하 클라이언트 또는 `wrk`류 도구 응용)
  - level-triggered/edge-triggered 각각에서 이벤트 누락 없이 처리되는지 확인
  - v2/v3 대비 동일 접속 수에서 스레드 수와 메모리 사용량 비교

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

각 버전은 `server/` 아래 `v1`, `v2` 등 버전별 하위 폴더(`include`/`src`/`CMakeLists.txt`)로 분리하고, 최상위 `server/CMakeLists.txt`가 `add_subdirectory()`로 각 버전을 불러온다. 버전마다 독립된 실행 파일(`v1_server`, `v2_server`, ...)로 빌드되어 언제든 원하는 버전을 골라 실행할 수 있다.

## 공통 테스트/QA 도구
- **동시성 버그 검출**: ThreadSanitizer(`-fsanitize=thread`), AddressSanitizer(`-fsanitize=address`) — v2부터 컴파일 옵션에 상시 포함
- **메모리 누수 검출**: `valgrind`(Linux) 또는 Xcode Instruments(macOS)
- **수동 접속 테스트**: `nc`, `telnet`
- **부하 테스트**: 간단한 멀티 커넥션 클라이언트를 직접 C++로 작성해 버전마다 재사용 (동시 접속 수, 메시지 처리량을 버전별로 비교하는 벤치마크 역할도 겸함)
- **단위 테스트**: 버전이 올라가며 로직이 복잡해지는 v3~v5부터 GoogleTest 도입 검토 (큐 동작, 파서 등 순수 로직 단위 위주)

## 벤치마크
버전을 감으로 비교하지 않고 숫자로 비교하기 위한 기록. 버전이 완료될 때마다 실측치를 채운다.
→ [basicdata/benchmark.md](benchmark.md)

## 버그 재현 / 트러블슈팅 기록
단순히 "안전하게 짜서 안 터졌다"가 아니라, 실제로 문제를 만들어보고 왜 안전한지 이해했다는 걸 남기기 위한 기록.
→ [basicdata/troubleshooting.md](troubleshooting.md)

## 깨달은 점
버그를 고치거나 개념을 짚으면서 실제로 이해하게 된 포인트 정리.
→ [basicdata/learnings.md](learnings.md)

## 빌드 도구 / 에디터
- cmake + Homebrew 설치 완료. `server/` 아래에서 `cmake -S . -B build && cmake --build build`로 빌드.
- 개발은 맥북 + Windows 두 컴퓨터에서 진행할 예정. Visual Studio 2022/2026은 Windows 전용이라 맥에서는 사용 불가 ("Visual Studio for Mac"은 단종되었고 애초에 C++용도 아니었음) → 맥에서는 **VS Code**(CMake 확장)를 사용하고, cmake 기반이라 나중에 Windows의 Visual Studio(CMake 내장 지원)로 넘어가도 같은 소스를 그대로 쓸 수 있음.
- Windows에서는 WSL2로 개발 (v4~ Linux epoll 목표와 서버 배포 환경(Linux)에 맞추기 위해).

## 현재 상태
- [x] v1 — 싱글 스레드 blocking TCP 서버
- [ ] v2 — thread-per-connection
- [ ] v3 — 고정 크기 thread pool + 작업 큐
- [ ] v4 — epoll 기반 이벤트 루프
- [ ] v5 — 이벤트 루프 + 워커 스레드 풀 결합
