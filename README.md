# MMO_Server

## 프로젝트 목적
C++ 실무 경험이 많지 않은 상태에서, MMO RPG 서버의 기초 밑단(네트워크 + 멀티스레드)을 직접 만들어보며 C++ 실력을 쌓기 위한 학습 프로젝트.

## 진행 방식
한 번에 완성된 서버를 만드는 대신, 버전을 올려가며 여러 개의 "기초 뼈대"를 단계적으로 구현한다.
각 뼈대는 서로 다른 동시성/네트워크 처리 방식을 대표하며, 이 과정을 거치면 어떤 구조(뼈대)로도 서버를 만들 수 있는 기반 지식을 갖추는 것이 목표.

버전별 상세 계획은 [basicdata/roadmap.md](basicdata/roadmap.md) 참고.

## 버전 개요
1. **v1** — 싱글 스레드 blocking TCP 서버 (완료)
2. **v2** — thread-per-connection (완료)
3. **v3** — 고정 크기 thread pool + bounded 작업 큐 (완료)
4. **v4** — Linux epoll 기반 단일 스레드 reactor echo 서버 (완료)
5. **v5** — 이벤트 루프 + 워커 스레드 풀 결합 (완료)
6. **v6** — Windows IOCP 기반 비동기 I/O 서버 (예정)
7. **v7** — IOCP + 워커/game logic queue 결합 (예정)

## 타겟 플랫폼
- **클라이언트**: Windows에서 플레이됨. 단, TCP/IP는 프로토콜 레벨이라 클라이언트 OS는 서버 코드에 영향을 주지 않음.
- **서버 배포 환경**: Linux로 확정. 따라서 v4~v5의 Linux epoll 선택은 일반론이 아니라 실제 배포 타겟에 맞춘 것.
- v1~v3: macOS(로컬)에서 바로 진행 (BSD 소켓 API, Linux에서도 동일하게 동작)
- v4~v5: Linux epoll 타겟 (Docker/VM 환경 필요, 실제 배포 환경과 동일)
- v6~v7: native Windows IOCP 비교 학습 트랙 (Windows + Visual Studio 환경)

## 빌드 / 개발 환경
- 빌드 시스템: CMake (Homebrew로 설치 완료).
- v1~v3의 POSIX/BSD 소켓 코드는 macOS와 Linux에서 같은 CMake 구조로 빌드한다.
- v4~v5의 epoll 코드는 Linux 전용이므로 Docker/WSL2 같은 Linux 환경에서 빌드한다.
- 에디터: 맥에서는 **VS Code**(CMake 확장) 사용. Visual Studio 2022/2026은 Windows 전용이라 맥에서는 실행 불가 ("Visual Studio for Mac"은 단종 + C++용도 아니었음).
- Windows에서는 v4~v5를 WSL2에서 개발하고, v6~v7은 native Windows + Visual Studio에서
  IOCP를 직접 구현한다.

## 관련 문서
- [basicdata/benchmark.md](basicdata/benchmark.md) — 버전별 성능 실측치
- [basicdata/troubleshooting.md](basicdata/troubleshooting.md) — 버그 재현/수정 기록
- [basicdata/learnings.md](basicdata/learnings.md) — 버전별로 깨달은 개념 정리
- [basicdata/feedback.md](basicdata/feedback.md) — AI의 피드백 기록

## 현재 상태
- [x] **v1** — blocking socket 흐름, partial send, `EINTR`, 길이 기반 로그,
  `SO_REUSEADDR`까지 구현 및 echo 회귀 테스트 완료

- [x] **v2** — thread-per-connection, 활성 client 목록의 mutex 보호, 동시 접속과
  fd 정리 테스트 완료

- [x] **v3** — 고정 worker 4개, 최대 128개의 bounded queue, condition variable,
  worker monitor, SIGINT/SIGTERM graceful shutdown 구현 완료
  - 동시 echo 50개 요청 50/50 성공
  - 장기 연결 140개 포화 테스트에서 worker 처리 4개 + 큐 대기 128개 유지,
    초과 연결 8개 거부 확인
  - 활성 연결 6개 상태에서 `Ctrl+C` 종료 시 client 정리, worker/monitor join,
    `Stopping` 출력과 프로세스 종료 확인

- [x] **v4** — Linux epoll 기반 단일 스레드 reactor echo 서버 구현 완료
  - Docker Ubuntu 24.04 환경에서 `v4_server` 빌드 성공
  - `epoll_create1()`/`epoll_ctl()`/`epoll_wait()`로 listen fd, client fd,
    `eventfd` 기반 stop fd를 하나의 이벤트 루프에서 처리
  - 5개 동시 `nc` 클라이언트 echo 성공
  - `Ctrl+C` 시 SIGINT를 `sigwait()`로 받고 `eventfd`로 `epoll_wait()`를 깨워
    client fd, listen fd, stop fd, epoll fd 정리 확인

- [x] **v5** — 이벤트 루프 + 워커 스레드 풀 결합 구현 완료
  - reactor(epoll 루프)는 I/O(recv/send)만 담당, 워커는 job queue로 받은 데이터를
    세션별 송신 버퍼에 적재하고 `EPOLLOUT`으로 reactor에게 송신을 넘김
  - client fd non-blocking + `EPOLLOUT` + 세션별 송신 버퍼, `Send_All()`의
    Completed/Partial/Failed 상태 구분, 같은 커넥션의 job을 항상 같은 워커로
    보내는 sticky routing, 뮤텍스로 보호된 단일 로그 출력 지점, 종료 시 pending
    데이터 best-effort flush까지 구현
  - v4와 동일 시나리오(50 clients × 20 msg)에서 1000/1000, 2000 clients × 20 msg
    (총 40,000 echo) 순간 부하에서 40000/40000, 300 clients 지속 부하에서 3000/3000
    echo 성공
  - `top -H` 스레드별 CPU 측정으로 job queue가 아니라 reactor 스레드 하나가
    병목이라는 것을 확인
  - AddressSanitizer + LeakSanitizer로 반복 연결/해제 부하 후 메모리 누수 없음 확인
  - 리뷰 과정에서 발견한 버그와 회고는 [basicdata/feedback.md](basicdata/feedback.md),
    구체적인 버그 재현/수정은 [basicdata/troubleshooting.md](basicdata/troubleshooting.md)의
    v5 항목 참고

- [ ] **v6** — Windows IOCP 기반 비동기 I/O 서버

- [ ] **v7** — IOCP + 워커/game logic queue
