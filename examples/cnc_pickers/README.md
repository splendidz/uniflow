# cnc_pickers

> 🌐 언어: **한국어** | [English](README.en.md) &nbsp;|&nbsp; [<- 예제 갤러리](../../EXAMPLES.md)

<p align="center">
  <img src="../../docs/videos/cnc_picker.gif" alt="cnc_pickers demo" width="720"/>
</p>

uniflow의 레퍼런스 프로젝트. 가상 CNC 가공 라인을 단일 펌프 위에서 운전합니다. Load 픽커가 원자재를 zone A에서 집어 zone B로, Stage가 B에서 가공하고, Unload 픽커가 B에서 zone C로 내보냅니다.

장비 자동화의 전형적 난제 - **두 픽커가 zone B에 동시에 들어가면 충돌** - 을 오케스트레이터와 모듈 간 상태 폴링으로 푸는 모습이 이 예제의 핵심입니다. 모든 모듈이 같은 펌프 위에 있으므로 zone 점유 상태 공유에 락이 필요 없습니다.

---

## 무엇을 보여주나

- **Cmd -> Wait 2-step 모션 패턴** - 모든 축/그리퍼 동작이 "명령을 내리는 step(`OnLoad_CmdMoveToSource`)"과 "도착을 폴링하는 step(`OnLoad_WaitAtSource`, `Stay()`로 `InPosition()` 폴링)"의 쌍으로 표현됩니다. 장비 모션 코드의 자연스러운 골격.
- **가상 하드웨어 모델링** - `MotorAxis`가 가속/settle 시간까지 흉내내고, `Stay()` 폴링으로 "도착할 때까지 대기"를 비블로킹으로 구현합니다.
- **상호 배제 without 락** - Load/Unload 픽커가 `PartnerInZoneB()`로 상대 위치를 확인해 zone B 동시 진입을 피합니다. 둘 다 같은 펌프 스레드 위라 단순 멤버 읽기로 충분.
- **오케스트레이터 패턴** - `UF_Orchestrator`가 라인 전체를 조율: 원자재 spawn(시간 구동), 각 모듈의 다음 flow 시작(상태 구동, `picker.IsIdle()` / `stage.state()` 관찰). 픽커/Stage는 *언제* 움직일지 스스로 결정하지 않습니다.
- **콘솔 + 파일 동시 로깅** - `EnvLogObserver`가 `ConsoleObserver` 출력을 콘솔과 `cnc_pickers.log`에 동시에 씁니다. 커스텀 observer 실사용 예.

---

## 동작 모델

| 모듈 | 역할 |
|---|---|
| `UF_LoadPicker` | 원자재를 A -> B로. 16-step 사이클(이동/하강/그립/상승/이동/하강/릴리스/상승/후퇴) |
| `UF_Stage` | zone B에서 가공. 픽커가 부품을 놓으면 가공 flow 시작, 완료 후 Unload 가능 상태로 |
| `UF_UnloadPicker` | 가공된 부품을 B -> C로 |
| `UF_Orchestrator` | 라인 스케줄러. 원자재 생성 타이밍과 각 모듈의 다음 flow 시작을 전담 |
| `UF_Visualization` | Win32로 라인 실시간 시각화 |

전부 `App`의 멤버로 보유하며, 2단계 초기화를 씁니다: phase 1에서 모든 모듈을 생성(ctor 본문은 다른 모듈을 만지지 않음), phase 2 `Start()`에서 flow를 시작(이때부터 모듈 간 참조 안전).

---

## 보면 좋은 파일

- [uf_orchestrator.cpp](uf_orchestrator.cpp) - 스케줄링 결정의 모든 것(spawn/start 판정)
- [uf_load_picker.cpp](uf_load_picker.cpp) - 16-step 픽커 사이클, Cmd -> Wait 패턴의 완성형
- [app.h](app.h) - 모듈 2단계 초기화 패턴, Runtime Opts(threads/observer/sleep 정책)
- [env_log_observer.h](env_log_observer.h) - 콘솔 + 파일 동시 출력 커스텀 observer
- [picker_motion.h](picker_motion.h) - 가상 모터축/그리퍼 모델

---

## 빌드 / 실행

Win32 시각화를 쓰므로 Windows + MSVC 환경입니다. `.vcxproj`가 포함되어 있습니다.

```powershell
# Visual Studio
examples\cnc_pickers\cnc_pickers.vcxproj 를 솔루션에 추가하고 F5

# CLI (MSVC, vcvars64 환경)
cl /std:c++17 /EHsc /I . examples\cnc_pickers\*.cpp /Fe:build\cnc_pickers.exe /Fo:build\
```

실행하면 라인 창이 뜨고, 콘솔과 `cnc_pickers.log`에 step 전이가 동시에 기록됩니다. 창을 닫으면 종료 시 전달된 부품 수가 출력됩니다:

```text
parts delivered to Unload: 7
```

---

## 더 읽기

- Cmd -> Wait 폴링 패턴: [TUTORIAL.md 챕터 3-4](../../TUTORIAL.md)
- 커스텀 observer 만들기: [TUTORIAL.md 챕터 8](../../TUTORIAL.md)
- 오케스트레이션 패턴: [TUTORIAL.md 마지막 챕터](../../TUTORIAL.md)
- 전체 예제 갤러리: [EXAMPLES.md](../../EXAMPLES.md)
