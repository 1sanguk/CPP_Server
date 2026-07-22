# MMO_Server

## 프로젝트 목적
C++ 실무 경험이 많지 않은 상태에서, MMO RPG 서버의 기초 밑단(네트워크 + 멀티스레드)을 직접 만들어보며 C++ 실력을 쌓기 위한 학습 프로젝트.

## 진행 방식
한 번에 완성된 서버를 만드는 대신, 버전을 올려가며 여러 개의 "기초 뼈대"를 단계적으로 구현한다.
각 뼈대는 서로 다른 동시성/네트워크 처리 방식을 대표하며, 이 과정을 거치면 어떤 구조(뼈대)로도 서버를 만들 수 있는 기반 지식을 갖추는 것이 목표.

버전별 상세 계획은 [basicdata/roadmap.md](basicdata/roadmap.md) 참고.

## 버전 개요
1. **v1** — 싱글 스레드 blocking TCP 서버 (소켓 API 익히기)
2. **v2** — thread-per-connection (기본 멀티스레드, mutex)
3. **v3** — 고정 크기 thread pool + 작업 큐 (조건 변수, 생산자-소비자 패턴)
4. **v4** — epoll 기반 이벤트 루프 (단일 스레드 reactor)
5. **v5** — 이벤트 루프 + 워커 스레드 풀 결합 (실제 상용 MMO 서버에 가까운 구조)

## 타겟 플랫폼
- **클라이언트**: Windows에서 플레이됨. 단, TCP/IP는 프로토콜 레벨이라 클라이언트 OS는 서버 코드에 영향을 주지 않음.
- **서버 배포 환경**: Linux로 확정. 따라서 v4~v5의 Linux epoll 선택은 일반론이 아니라 실제 배포 타겟에 맞춘 것.
- v1~v3: macOS(로컬)에서 바로 진행 (BSD 소켓 API, Linux에서도 동일하게 동작)
- v4~v5: Linux epoll 타겟 (Docker/VM 환경 필요, 실제 배포 환경과 동일)

## 빌드 / 개발 환경
- 빌드 시스템: cmake (Homebrew로 설치 완료). 같은 `CMakeLists.txt`로 macOS(Makefile/Ninja)와 Windows(Visual Studio 솔루션)를 모두 생성 가능.
- 에디터: 맥에서는 **VS Code**(CMake 확장) 사용. Visual Studio 2022/2026은 Windows 전용이라 맥에서는 실행 불가 ("Visual Studio for Mac"은 단종 + C++용도 아니었음).
- 이후 Windows로 넘어갈 때는 WSL2에서 같은 소스를 그대로 사용 (서버 배포 환경인 Linux와 100% 동일한 조건으로 개발하기 위함).

## 관련 문서
- [basicdata/benchmark.md](basicdata/benchmark.md) — 버전별 성능 실측치
- [basicdata/troubleshooting.md](basicdata/troubleshooting.md) — 버그 재현/수정 기록
- [basicdata/learnings.md](basicdata/learnings.md) — 버전별로 깨달은 개념 정리

## 현재 상태
v1(싱글 스레드 blocking TCP 서버) 완료.
