# city_traffic

> 언어: **한국어** | [English](README.md) | [<- 예제 갤러리](../../EXAMPLES.kr.md)

<p align="center">
  <img src="../../../.res/city_traffic.gif" alt="city_traffic demo" width="720"/>
</p>

uniflow의 종합 예제. 차량 15대가 각자 독립된 uniflow 모듈로, 공유 신호등과 앞차를
보며 가감속/정지/회전하며 도시를 순환합니다.

핵심 속성은 **애플리케이션 레벨 스레드가 0개**라는 점입니다. 모든 차량, 모든
신호등, 렌더 스냅샷 갱신까지 전부 단일 펌프 스레드 위에서 협력적으로 돕니다. 차량
위치와 신호 상태를 담은 공유 `World`는 락이 전혀 없습니다 - 한 스레드만
접근하기 때문입니다.

---

## 무엇을 보는 예제인가 (Feature focus)

- **수십 개 모듈의 단일 스레드 협력 실행.** 차량 15대 + 교차로 신호등 + 시각화 모듈이
  한 펌프 위에서 round-robin으로 돕니다. 멀티스레드 없이 도시 전체가 동시에 동작합니다.
- **lock-free 공유 상태.** 불변 `Map`(도로망)과 가변 `World`(차량 레지스트리 + 신호
  테이블)를 모든 모듈이 공유하지만 mutex는 펌프->렌더 스냅샷 핸드오프 한 곳
  뿐입니다([snapshot.h](snapshot.h)). 모듈끼리는 평범한 멤버 읽기로 접근합니다.
- **async 없는 순수 폴링 스케줄링.** 이 예제에는 블로킹 작업이 없습니다. 모든 진행이
  `Stay()` 틱 루프로, 매 라운드 실제 경과 시간(dt)으로 물리를 적분합니다. async 없이
  수십 개 흐름이 진행됩니다.
- **모듈로 표현된 상태 기계.** 차량의 주행 단계가 step 체인입니다:
  `Step_Cruise`(가속/정지선 감속) -> `Step_Wait`(적색 대기) -> `Step_Cross`(교차로
  통과) -> `Step_Turn`(회전 곡선) -> 루프. 신호등 위상도 step 전이라 트레이스에 그대로
  기록됩니다.
- **듀얼 렌더러 (한 데이터, 두 백엔드).** 펌프가 `World`를 `Snapshot`으로 복사하면,
  렌더 측은 같은 스냅샷을 두 방식 중 하나로 그립니다 - Windows는 Win32 GDI 창, 그 외
  플랫폼(Linux/macOS)은 ANSI 콘솔. 펌프 로직과 렌더 백엔드가 분리됩니다.

---

## 동작 모델

| 요소 | 역할 |
|---|---|
| `Flow_Vehicle` | 차량 한 대 = 모듈 한 개. 위치는 방향 엣지(from -> to) + 진행도 dist[0,1]. 가속도/정지선 감속/앞차 추종(`GapAhead`)/확률적 회전을 step으로 표현 |
| `Flow_TrafficLight` | 교차로(코너 제외 노드)마다 한 개. 2-위상 고정 주기(NS 녹 -> NS 황 -> EW 녹 -> EW 황). 직진+우회전이 녹에 함께 진행. 시작 위상은 node id에서 파생해 desync |
| `Flow_Visualization` | 매 틱 `World`를 `Snapshot`으로 복사(펌프 측). 렌더는 메인 스레드에서 |
| `World` (공유) | 차량 위치 레지스트리 + 신호 테이블. 단일 펌프라 lock-free |
| `Map` (불변) | 노드/엣지로 된 고정 폐쇄 도로망. 막다른 길 없음 -> 영원히 순환 |

---

## 보면 좋은 파일

- [uf_vehicle.cpp](uf_vehicle.cpp) - 주행 step 체인. 가감속, 정지선/앞차 통합 감속, 회전 결정, 회전 곡선
- [uf_traffic_light.cpp](uf_traffic_light.cpp) - 2-위상 신호 사이클을 step으로
- [world.cpp](world.cpp) - 공유 신호/차량 레지스트리, 진입 판정 헬퍼
- [app.h](app.h) - Runtime 1개에 신호등/차량 함대/시각화 부착, 골든앵글 HSV로 차량 색 생성
- [uf_visualization.cpp](uf_visualization.cpp) - 펌프측 스냅샷 복사 + 렌더 디스패처
- [uf_visualization_win32.cpp](uf_visualization_win32.cpp) - Win32 GDI 백엔드(Windows 전용)
- [uf_visualization_console.cpp](uf_visualization_console.cpp) - ANSI 콘솔 백엔드(모든 터미널)

---

## 빌드 / 실행

같은 소스가 두 렌더러를 모두 컴파일합니다. [RunVisualisation()](uf_visualization.cpp)이
플랫폼에 맞는 백엔드를 고릅니다 - Windows 밖에서는 항상 콘솔, Windows에서는 Win32 창
(단 `UF_RENDER=console` 환경변수가 있으면 콘솔).

**Linux / macOS (콘솔, 추가 설치 불필요):**

```sh
cd cpp/examples/city_traffic
g++ -std=c++17 -O2 -I../.. *.cpp -o city_traffic -pthread
./city_traffic            # ANSI 맵이 그려짐. Enter 로 종료
```

**Windows (MSVC, x64 Native Tools 프롬프트):**

```bat
cd cpp\examples\city_traffic
cl /std:c++17 /EHsc /O2 /I..\.. *.cpp /Fe:city_traffic.exe user32.lib gdi32.lib
city_traffic.exe          :: Win32 창
set UF_RENDER=console & city_traffic.exe   :: 콘솔로 강제(미리보기)
```

> Win32 모드에서는 기본 `ConsoleObserver`가 신호 위상 변경과 각 차량의
> Cruise/Wait/Cross/Turn 전이를 콘솔에 출력해 진행 상황이 콘솔에 표시됩니다. 콘솔
> 모드에서는 렌더러가 화면을 소유하므로 무음 옵저버가 자동 적용됩니다.

---

## 더 읽기

- 협력 스케줄링과 lock-free 공유의 기초: [TUTORIAL.kr.md](../../TUTORIAL.kr.md)
- 전체 예제 갤러리: [EXAMPLES.kr.md](../../EXAMPLES.kr.md)
