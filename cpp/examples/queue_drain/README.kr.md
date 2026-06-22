# queue_drain

> 언어: **한국어** | [English](README.md)

uniflow로 옮긴 **생산자 / 소비자** 패턴. `Flow_Sender`가 무작위 산술 작업을
버스트 단위로 공유 메일박스에 밀어 넣고, `Flow_Receiver`가 메일박스를 하나씩 비우며
연산자에 따라 분기한다. 둘 다 **하나의 펌프 스레드**에서 돌기 때문에 그 사이의
메일박스에는 **락이 필요 없다**.

```
  uniflow queue_drain  v1.0.0
  ------------------------------------------------------------
  sender   bursts 7/20   last burst 5   idle gap
  vec A: 10  7 24 11 27  6 30  9 30  4
  vec B: 18 14 13 14 24 14  4 18  9  2
  ------------------------------------------------------------
  queue    depth 3
  [12 + 4] [27 - 9] [6 + 30]
  ------------------------------------------------------------
  receiver Adding   processed 41   add: 12 + 4 = 16
  last result: 12 + 4 = 16
  ------------------------------------------------------------
  press Enter to quit
```

## 무엇을 보는 예제인가 (Feature focus)

이 예제의 초점은 **한 펌프 스레드 위의 락 없는 생산자 / 소비자**와 **park / relaunch**
깨우기 패턴이다.

- **한 스레드, 큐에 락 없음.** sender와 receiver는 같은 펌프 위의 두 모듈이라
  라운드로빈으로 번갈아 돌며 메일박스를 동시에 건드리는 일이 없다. `Mailbox::Push` /
  `TryPop`은 뮤텍스 없는 평범한 `std::deque` 호출이다
  ([mailbox.h](mailbox.h) / [mailbox.cpp](mailbox.cpp)).

- **큐 드레인 루프.** receiver의 단일 task는 `Step1_TakeNext -> Step2_Add` 또는
  `Step3_Sub -> Step1_TakeNext` 를 반복한다. `TryPop`이 빈 큐를 만나면 `Done()`을
  반환해 모듈이 park 한다 ([uf_receiver.cpp](uf_receiver.cpp)).

- **park / relaunch 깨우기.** 매 버스트 뒤 sender는 `receiver.IsIdle()`을 확인하고,
  receiver가 park 했으면 `recv.ctx_drain_.StartFlow()` 로 드레인 task를 다시 띄운다.
  둘 다 같은 스레드 내 호출이라 스레드 간 시그널이 없다
  ([uf_sender.cpp](uf_sender.cpp)의 `Step1_Tick`).

- **콘솔/ANSI 렌더 패턴.** [console.h](console.h) / [console.cpp](console.cpp)는 의존성
  없는 ANSI 헬퍼다(simulator의 것과 바이트 단위로 동일). 펌프 측 스냅샷 스텝이 뮤텍스
  하나로 상태를 복사하고, 백그라운드 스레드가 ~25fps로 다시 그리는 동안 `main`은 stdin을
  대기한다(Enter 한 번으로 종료).

## 코드 지도

| 파일 | 역할 |
| --- | --- |
| [uf_sender.h](uf_sender.h) / [.cpp](uf_sender.cpp) | 생산자. 단일 영속 task: `kSendGap`마다 1..10개 작업 버스트를 push 하고 park 한 receiver를 다시 띄움. |
| [uf_receiver.h](uf_receiver.h) / [.cpp](uf_receiver.cpp) | 소비자. 드레인 루프: 작업 하나 pop, Add/Sub 분기, 반복; 비면 `Done()`. |
| [mailbox.h](mailbox.h) / [.cpp](mailbox.cpp) | 락 없는 FIFO(이유는 헤더 주석). |
| [uf_visualization.h](uf_visualization.h) / [.cpp](uf_visualization.cpp) | 펌프 측 스냅샷 스텝 + 메인 스레드 ANSI 대시보드. |
| [snapshot.h](snapshot.h) / [.cpp](snapshot.cpp) | 펌프 -> 렌더 스레드 핸드오프(이 데모의 유일한 뮤텍스). |
| [console.h](console.h) / [.cpp](console.cpp) | 재사용 ANSI 콘솔 헬퍼. |
| [globals.h](globals.h) / [.cpp](globals.cpp) | `Msg` 값 타입, 튜닝 상수, 정지 래치. |
| [app.h](app.h) | Runtime + 모든 모듈, 2단계 초기화, 무음 옵저버. |
| [main.cpp](main.cpp) | Start, 렌더러 실행, shutdown, 요약 한 줄 출력. |

## 빌드 / 실행

콘솔만 쓰므로 추가로 설치할 것이 없다.

**Linux / macOS (g++ 또는 clang++):**

```sh
cd cpp/examples/queue_drain
g++ -std=c++17 -O2 -I../.. *.cpp -o queue_drain -pthread
./queue_drain
```

**Windows (MSVC, x64 Native Tools 프롬프트):**

```bat
cd cpp\examples\queue_drain
cl /std:c++17 /EHsc /O2 /I..\.. *.cpp /Fe:queue_drain.exe
queue_drain.exe
```

> 참고: 이 데모는 ANSI 터미널을 가정한다. Windows에서는 `console::EnableAnsi()`가
> 콘솔의 VT 처리를 켠다(Windows Terminal 권장).
