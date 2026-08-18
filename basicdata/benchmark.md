# 벤치마크

버전을 감으로 비교하지 않고 숫자로 비교하기 위한 기록. 버전이 완료될 때마다 아래 표에 실측치를 채운다.

- **측정 항목**: 동시 접속 수(테스트 범위), 처리량(TPS, 초당 echo 처리 메시지 수),
  평균/p95/p99 지연시간(ms), 스레드 수, 메모리 사용량
- **측정 방법**: 버전마다 동일한 부하 클라이언트(공통 테스트/QA 도구의 멀티 커넥션 클라이언트)로 동일한 시나리오(예: 동시 접속 N개, 각 클라이언트가 초당 M개 메시지 송신)를 재현해서 비교

| 버전 | 동시 접속(테스트 범위) | TPS | 평균 지연(ms) | p95 지연(ms) | p99 지연(ms) | 스레드 구조 | 메모리 | 비고 |
|---|---:|---:|---:|---:|---:|---|---:|---|
| v1 | 1 | - | - | - | - | main 1개 | - | 기능 테스트만 완료 |
| v2 | 50 echo | - | 5.1 | 9.9 | - | main + 접속당 1개 | - | 50/50 성공, 총 15.1ms |
| v3 | 50 echo / 132 장기 연결 | - | 3.6 | 5.6 | - | main + accept 1 + worker 4 + monitor 1 | - | 50/50 성공, 총 17.8ms |
| v4 | 50 clients / 1000 echo | 326 | - | - | - | main + server 1 | - | Docker/Linux, 1000/1000 echo 성공 |
| v5 | 50 clients / 1000 echo (최대 2000 동시) | 32419 | - | - | - | reactor 1 + worker 4 + monitor 1 | - | 1000/1000, 40000/40000 성공, ASan/LSan 누수 없음 |
| v6 | 200 clients / 2000 echo | - | - | - | - | IOCP worker 16개(논리 코어 기준) | - | Windows, 2000/2000 성공, ASan 1000/1000 |
| v7 | - | - | - | - | - | - | - | 미구현 |

## 측정 메모

#### 측정일: 2026-07-28
- 환경: Apple Silicon macOS, localhost, 상세 디버그 로그 활성화
- v2와 v3 수치는 동시 요청 50개의 단일 실행 표본이다. 지속 부하 TPS, p99, 메모리는
  측정하지 않았으므로 일반적인 성능 결론이 아니라 기능 회귀와 대략적인 비교에만 사용한다.
- v3 포화 테스트에서는 장기 연결 140개 중 worker가 4개, bounded queue가 128개를
  유지했고 초과 8개 연결을 거부했다.
- ThreadSanitizer 빌드에서 v3의 동시 echo 50개는 모두 성공했지만, 의도적으로 동기화하지
  않은 다중 스레드 `std::cout` 출력에서 data race가 보고됐다.


#### 측정일: 2026-07-30
- 환경: Docker Desktop + Ubuntu 24.04 컨테이너, macOS host port mapping(`-p 9000:9000`)
- v4는 50개 client가 각각 20개 메시지를 보내는 방식으로 총 1000개 echo를 요청했고,
  1000/1000 응답 성공을 확인했다. 총 소요 시간은 3063ms, 단순 계산 TPS는 약 326 echo/sec였다.
  상세 디버그 로그가 활성화된 단일 실행 표본이므로 정밀 성능 결론이 아니라 부하 회귀 확인용
  수치로 기록한다. 평균/p95/p99 지연시간과 메모리 사용량은 아직 측정하지 않았다.


#### 측정일: 2026-08-07
- 환경: 이번엔 Docker가 아니라 Ubuntu Linux 개발 컨테이너에서 직접 빌드/실행. v1~v4와
  달리 macOS/Docker 포트 매핑 과정이 없다.
- v5는 50 clients × 20 msg(1000 echo)에서 44ms, TPS 약 32419/sec를 기록했다. 같은
  시나리오의 v4(326 TPS)와는 수치 차이가 크지만, v4는 Docker 컨테이너, v5는 네이티브
  Linux 환경이고 두 버전 다 상세 디버그 로그가 켜진 단일 실행 표본이라 절대 수치를
  직접 비교하기보다는 각자 부하 회귀 확인용으로만 쓴다.
- 2000 clients × 20 msg(총 40,000 echo) 순간 부하에서는 TPS가 약 9848/sec로 떨어졌다.
  `top -H`로 스레드별 CPU를 보면 reactor 스레드 하나가 80%, 워커 4개는 각각 10~20%만
  사용해, job queue가 아니라 reactor 스레드가 병목이라는 걸 확인했다(자세한 내용은
  feedback.md의 v5 항목 참고).
- 300 clients를 100ms 간격으로 30초 이상에 걸쳐 늘리는 지속 부하에서는 3000/3000
  echo가 모두 성공했다.
- valgrind가 없는 환경이라 AddressSanitizer + LeakSanitizer로 대체해, 120개 연결·
  1,800개 echo 규모의 반복 연결/해제 부하 후 정상 종료까지 확인했고 메모리 누수나
  메모리 안전성 에러는 발견되지 않았다.
- 평균/p95/p99 지연시간과 정밀 메모리(RSS) 수치는 아직 측정하지 않았다 — 지금 쓰는
  부하 클라이언트가 총 소요 시간과 성공/실패 카운트만 측정하고 요청별 지연시간은
  기록하지 않는다.

#### 측정일: 2026-08-17
- 환경: native Windows, MSVC C++20, IOCP completion worker는 실행 환경의 논리 코어 수인
  16개로 생성했다.
- 일반 빌드에서 50 clients가 각각 20개 메시지를 보내 총 1000/1000 echo에 성공했다.
  별도 client의 `WSARecv()`가 pending인 상태에서 서버를 종료해 pending I/O drain,
  worker 16개 join, 프로세스 종료 코드 0을 확인했다.
- 세션/작업별 context 분리와 Accept context 16개 선게시 후 200 clients가 각각 10개 메시지를
  보내 총 2000/2000 echo에 성공했다.
- MSVC AddressSanitizer 빌드에서는 50 clients × 20 msg(1000/1000 echo)와 같은 pending I/O
  종료를 수행했고 use-after-free나 버퍼 오류가 보고되지 않았다. MSVC AddressSanitizer는
  LeakSanitizer와 동일한 누수 검사를 제공하지 않으므로 누수 검증 결과로 확대 해석하지 않는다.
- `V6_FORCE_PARTIAL_SEND_TEST` 빌드에서 느린 수신 client에 4 MiB를 전송해 전체 데이터 일치와
  send continuation 4,032회를 확인했다.
- 기능 회귀 확인용 실행이며 TPS, 요청별 평균/p95/p99 지연시간과 메모리 사용량은 측정하지 않았다.

#### 측정일: 2026-08-18
- 환경: GitHub Actions `windows-2022` runner, MSVC C++20. 성능 측정이 아니라 v6 보완 후
  Windows 회귀 검증을 목적으로 실행했다.
- 일반 Debug 작업에서 256 KiB echo 데이터 일치와, 응답을 읽지 않는 client가 4 MiB를 보낸
  상태의 graceful shutdown 및 종료 코드 0을 확인했다.
- `V6_FORCE_PARTIAL_SEND_TEST=ON` 작업에서 실제 partial-send 로그와 echo 데이터 일치를 확인했다.
- MSVC AddressSanitizer `RelWithDebInfo` 작업에서 같은 echo 및 pending-send 종료 시나리오를
  통과했고 use-after-free나 buffer overflow가 보고되지 않았다. LeakSanitizer와 동일한 누수
  검증 결과로 확대 해석하지 않는다.
