# simulator

> 언어: **한국어** | [English](README.md)

여러 flow가 하나의 펌프 스레드와 하나의 **논리 시계(VirtualClock)** 를 공유하는 콘솔
시뮬레이터다. 다섯 개의 러너가 각자 트랙을 도는 동안, 사용자는 명령으로 논리 시간의
흐름을 제어한다.

```
  uniflow simulator  v1.0.0
  ------------------------------------------------------------
  [RUNNING]   speed x2.00      pause | resume | speed <n> | quit
  ------------------------------------------------------------
  Atlas    lap  3  [##############......]  72%  Step2_Move
  Bolt     lap  5  [####................]  20%  Step1_Gate
  Comet    lap  2  [####################] 100%  Step3_Rest
  Dash     lap  4  [#########...........]  47%  Step2_Move
  Echo     lap  1  [##......]            ...
  ------------------------------------------------------------
  >
```

## 무엇을 보는 예제인가 (Feature focus)

이 예제의 초점은 **시간 제어**와 **단일 스레드 협동**이다.

- **VirtualClock - scale / freeze.** 러너의 진행은 실시간이 아니라 `Runtime::clock()`
  (논리 시계)에 대해 측정된다. `pause`는 `clock().Freeze()` 한 번으로 모든 러너를 동시에
  멈추고, `speed <n>`은 `clock().SetScale(n)` 한 번으로 전체 페이스를 늘리거나 줄인다.
  flow마다 배속을 전달하는 배선은 전혀 없다 - 시계 하나가 전부를 지배한다.
  ([uf_runner.cpp](uf_runner.cpp)의 `VElapsedMs`, [main.cpp](main.cpp)의 명령 처리)

- **한 스레드, 락 없음.** 다섯 러너와 렌더러까지 여섯 개 flow가 모두 같은 펌프에서 돈다.
  러너가 자기 행을 쓰고 렌더러가 그 행을 읽지만, 둘은 같은 스레드라 절대 겹치지 않는다.
  그래서 [snapshot.h](snapshot.h)에는 뮤텍스가 **없다**. (해당 헤더 주석 참고.)

- **렌더러도 하나의 flow.** [uf_view.cpp](uf_view.cpp)의 `Flow_View`는 같은 펌프 위의
  또 다른 모듈이다. 프레임 주기는 **실시간** `UFTimer`를 쓴다 - 논리 시계가 얼면
  (`pause`) 러너는 멈추지만 대시보드는 계속 그려져 `[PAUSED]`를 보여준다.

- **콘솔/ANSI 렌더 패턴.** [console.h](console.h) / [console.cpp](console.cpp)는 의존성
  없는 ANSI 헬퍼다(Linux 터미널, Windows Terminal/Windows 10+ 모두 동작). 같은 패턴을
  city_traffic / pick_and_place의 Linux 콘솔 렌더러가 재사용한다.

## 코드 지도

| 파일 | 역할 |
| --- | --- |
| [uf_runner.h](uf_runner.h) / [.cpp](uf_runner.cpp) | 러너 한 명. `Step1_Gate -> Step2_Move -> Step3_Rest` 를 무한 반복(한 바퀴 = 한 lap). 진행은 VirtualClock 기준. |
| [uf_view.h](uf_view.h) / [.cpp](uf_view.cpp) | 대시보드 렌더러 flow. 실시간 30fps로 스냅샷을 그림. |
| [snapshot.h](snapshot.h) / [.cpp](snapshot.cpp) | 러너 -> 렌더러 핸드오프(락 없음, 이유는 헤더 주석). |
| [console.h](console.h) / [.cpp](console.cpp) | 재사용 ANSI 콘솔 헬퍼. |
| [app.h](app.h) | Runtime + 모든 모듈, 2단계 초기화. 무음 옵저버 주입. |
| [main.cpp](main.cpp) | stdin 명령 루프(pause/resume/speed/quit). |

## 명령

| 입력 | 효과 |
| --- | --- |
| `pause` | 논리 시간을 정지 (러너 멈춤, 대시보드는 살아 있음) |
| `resume` | 논리 시간 재개 |
| `speed <n>` | 논리 시간 배속 (`n > 0`; 0.5 = 절반, 4 = 4배) |
| `quit` | 정지 후 종료 |

## 빌드 / 실행

콘솔만 쓰므로 추가로 설치할 것이 없다.

**Linux / macOS (g++ 또는 clang++):**

```sh
cd cpp/examples/simulator
g++ -std=c++17 -O2 -I../.. *.cpp -o simulator -pthread
./simulator
```

**Windows (MSVC, x64 Native Tools 프롬프트):**

```bat
cd cpp\examples\simulator
cl /std:c++17 /EHsc /O2 /I..\.. *.cpp /Fe:simulator.exe
simulator.exe
```

> 참고: 이 데모는 ANSI 터미널을 가정한다. Windows에서는 `console::EnableAnsi()`가
> 콘솔의 VT 처리를 켠다(Windows Terminal 권장).
