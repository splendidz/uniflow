# city_traffic

> 🌐 언어: **한국어** | [English](README.md) &nbsp;|&nbsp; [<- 예제 갤러리](../../../EXAMPLES.kr.md)

<p align="center">
  <img src="../../../docs/videos/city_traffic.gif" alt="city_traffic demo" width="720"/>
</p>

uniflow의 마지막 피날레 데모. 수십 대의 차량이 각자 독립된 uniflow 모듈로, 공유 신호등과 앞차를 보며 가감속/정지/회전하며 도시를 영원히 순환합니다.

핵심은 단 하나: **애플리케이션 레벨 스레드가 0개**라는 점입니다. 모든 차량, 모든 신호등, 렌더 스냅샷 갱신까지 전부 단일 펌프 스레드 위에서 협력적으로 돕니다. 차량 위치와 신호 상태를 담은 공유 `World`는 락이 전혀 없습니다 - 어차피 한 스레드만 만지니까요.

---

## 무엇을 보여주나

- **수십 개 모듈의 협력 실행** - 차량 15대 + 교차로 신호등 + 시각화 모듈이 한 펌프 위에서 round-robin으로 돕니다. 멀티스레드 없이 "동시에 도는 도시"를 만듭니다.
- **lock-free 공유 상태** - 불변 `Map`(도로망)과 가변 `World`(차량 레지스트리 + 신호 테이블)를 모든 모듈이 공유하지만 mutex가 단 하나도 없습니다.
- **UF_ASYNC 없는 순수 협력 스케줄링** - 이 데모엔 블로킹 작업이 없습니다. 모든 진행이 `Stay()` 틱 루프로, 매 라운드 실제 경과 시간(dt)으로 물리를 적분합니다. async 없이도 수십 개 흐름이 매끄럽게 진행되는 모습.
- **모듈로 표현된 상태 기계** - 차량의 주행 단계가 step 체인으로: `OnSeg_Cruise`(가속/정지선 감속) -> `OnSeg_Wait`(적색 대기) -> `OnSeg_Cross`(교차로 통과) -> `OnSeg_Turn`(회전 곡선) -> 루프. 신호등 위상도 step 전이라 콘솔에 자동 로깅됩니다.
- **Win32 GDI 시각화** - 메인 스레드가 `Snapshot`을 읽어 도로/신호/차량을 렌더. 펌프 스레드와 렌더 스레드의 경계가 깔끔하게 분리됩니다.

---

## 동작 모델

| 요소 | 역할 |
|---|---|
| `UF_Vehicle` | 차량 한 대 = 모듈 한 개. 위치는 방향 엣지(from -> to) + 진행도 dist[0,1]. 가속도/정지선 감속/앞차 추종(`GapAhead`)/확률적 회전을 step으로 표현 |
| `UF_TrafficLight` | 교차로(코너 제외 노드)마다 한 개. 2-위상 고정 주기(NS 녹 -> NS 황 -> EW 녹 -> EW 황). 직진+우회전이 녹에 함께 진행. 시작 위상은 node id에서 파생해 desync |
| `UF_Visualization` | `Snapshot`을 읽어 Win32 GDI로 렌더하고 메인 메시지 루프를 돈다 |
| `World` (공유) | 차량 위치 레지스트리 + 신호 테이블. 단일 펌프라 lock-free |
| `Map` (불변) | 노드/엣지로 된 고정 폐쇄 도로망. 막다른 길 없음 -> 영원히 순환 |

차량은 매 틱 `World`에 자기 중심 좌표를 publish하고, 앞차 탐지는 같은 엣지 위 차량을 brute-force 스캔합니다(수십 대라 비용 무시). 신호 진입 판정은 `World`의 신호 테이블을 읽어 결정합니다.

---

## 보면 좋은 파일

- [uf_vehicle.cpp](uf_vehicle.cpp) - 주행 step 체인. 가감속, 정지선/앞차 통합 감속, 회전 결정, 베지어 곡선 회전
- [uf_traffic_light.cpp](uf_traffic_light.cpp) - 2-위상 신호 사이클을 step으로
- [world.cpp](world.cpp) - 공유 신호/차량 레지스트리, 진입 판정 헬퍼
- [app.h](app.h) - Runtime 1개에 신호등/차량 함대/시각화 부착, 골든앵글 HSV로 차량 색 생성
- [map.cpp](map.cpp) - 노드/엣지 도로망 테이블
- [uf_visualization.cpp](uf_visualization.cpp) - Win32 GDI 렌더(도로, 신호 화살표, 차체+바퀴+깜박이)

---

## 빌드 / 실행

Win32 GDI를 쓰므로 Windows + MSVC 환경입니다. `user32`/`gdi32`는 소스 내 `#pragma comment(lib, ...)`로 링크됩니다.

```powershell
# vcvars64 환경에서
cl /std:c++17 /EHsc /I cpp cpp\examples\city_traffic\*.cpp /Fe:build\city_traffic.exe /Fo:build\
build\city_traffic.exe
```

실행하면 도시 창이 뜨고, 콘솔에는 기본 `ConsoleObserver`가 신호 위상 변경과 각 차량의 Cruise/Wait/Cross/Turn 전이를 흘려보냅니다 (시뮬레이션이 돌고 있다는 체감용):

```text
[UF_TrafficLight] OnLight_NsGo -> OnLight_EwGo   #03 elapsed=4500.12ms  ...
[UF_Vehicle     ] OnSeg_Cruise -> OnSeg_Wait     #11 elapsed=820.40ms   ...
[UF_Vehicle     ] OnSeg_Wait -> OnSeg_Cross      #12 elapsed=1200.05ms  ...
[UF_Vehicle     ] OnSeg_Cross -> OnSeg_Turn      #13 elapsed=210.33ms   ...
```

창을 닫으면 `App::Shutdown()`이 정지를 요청하고 모든 모듈이 idle이 될 때까지 기다린 뒤 종료합니다.

---

## 더 읽기

- 협력 스케줄링과 lock-free 공유의 기초: [TUTORIAL.kr.md 챕터 3, 5](../../../TUTORIAL.kr.md)
- 오케스트레이션/상태 폴링 패턴: [TUTORIAL.kr.md 마지막 챕터](../../../TUTORIAL.kr.md)
- 전체 예제 갤러리: [EXAMPLES.kr.md](../../../EXAMPLES.kr.md)
