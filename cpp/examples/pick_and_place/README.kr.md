# pick_and_place

> 언어: **한국어** | [English](README.md) &nbsp;|&nbsp; [<- 예제 갤러리](../../EXAMPLES.kr.md)

<p align="center">
  <img src="../../../.res/pick_and_place.gif" alt="pick_and_place 데모" width="720"/>
</p>

uniflow의 레퍼런스 프로젝트이자 **Task-Level Syntax**의 표준 시연입니다. 가상 pick-and-place 라인을 단일 펌프 스레드 위에서 돌립니다 - Load 피커가 zone A에서 원자재를 집어 zone B에 놓고, Stage가 B에서 가공하고, Unload 피커가 완성품을 B에서 zone C로 빼냅니다.

라인 엔지니어가 흔히 마주하는 제약 - **두 피커가 절대 동시에 zone B에 있으면 안 된다** - 을 오케스트레이터와 모듈 간 상태 폴링으로 해결합니다. 모든 모듈이 같은 펌프 위에 있으므로, 존 점유 확인은 락이 아니라 평범한 멤버 읽기입니다.

---

## 왜 이것이 레퍼런스 프로젝트인가

피커는 평평한 이동 목록이 아닙니다. 두 개의 *의미 있는 동작* - **Pick**(소스 존에서 부품 획득)과 **Place**(목적 존에 전달) - 이고, 각각이 여러 모션 step으로 이뤄집니다. 이것이 Task-Level Syntax가 표현하려는 형태라서, 피커가 기계가 실제로 동작하는 방식 그대로 읽힙니다:

```text
Flow_LoadPicker   :  Pick (zone A)  ->  Place (zone B)
Flow_UnloadPicker :  Pick (zone B)  ->  Place (zone C)
Flow_Stage        :  Prepare  ->  Process  ->  Cleanup
```

각 task는 `struct ... : uniflow::Task<Flow_...>`로, 그 task의 step들이 공유하는 상태**와 step 멤버 함수 자체**를 들고 있고 flow가 인스턴스 하나(`ctx_pick_`, `ctx_place_`)를 **public**으로 보유합니다(peer가 런치할 수 있게). step은 자기 task의 멤버 함수이므로 그 task에 속하고, `flow()`로 부모 flow에 접근합니다(`flow().x_->Move(...)`, `flow().carrying_`) - `flow()`는 타입이 박힌 flow 참조를 돌려주고 task가 flow 안에 중첩돼 있어 flow의 private 멤버까지 읽습니다. 그래서 `Next`는 같은 task 구조체의 형제 step만 가리키고, task 경계를 넘는 것은 명시적 `StartTask`입니다. 각 task는 `Entry()`를 오버라이드해 진입 step을 이름으로 밝히고(`StepResult Entry() override { return Step_First(); }`), flow 생성자는 `AddTask(ctx_...)` 한 줄씩으로 task를 등록합니다. 모듈은 **한 번에 task 하나**를 돌립니다: 누구든 `module.ctx_x_.StartFlow()`로 task를 런치하고, 반환은 `StartResult` — `Ok`, `Busy`(이미 task 도는 중). 오케스트레이터는 모듈이 idle일 때까지 기다렸다 다음 task를 런치(`Pick` 다음 `Place`; `Prepare` 다음 `Process` 다음 `Cleanup`)해 라인을 구동합니다 - 그래서 *시퀀스*가 한 곳에 명시적으로 있고, 각 task는 `Done()`으로 끝나는 자족적 동작입니다. `Task`는 task 공통 정보도 거저 줍니다 - `Name()`, 진입 후 경과 `Elapsed()`, 방문한 step 경로 `Trajectory()`(각 항목이 `{이름, 머문 시간 ms, 틱 수}`) - 그리고 task 자기 멤버를 리셋하는 `OnEnter()` 훅. 자세한 내용은 메인 [README의 Task-Level Syntax 절](../../../README.kr.md#task-level-syntax)을 보십시오.

이 예제는 step 단위 매크로 대신 **직접 호출 API**를 씁니다: step은 평범한 task 멤버 함수로, `Next(...)`, `StayUntil(...)`, `SubmitAsync(...)`를 직접 호출하고 부모 flow 상태에 `flow()`로 닿습니다. 매크로는 인자 헬퍼 하나뿐 - `UF_FN(fn)`(`&Task::fn, "fn"` 멤버 포인터 + 로그 이름 쌍으로 확장) - 라서 함수가 독자와 인텔리센스에 그대로 보입니다.

---

## 무엇을 보여주는가

- **실제 적용된 Task-Level Syntax** - 각 피커는 `Pick -> Place` task 쌍, Stage는 `Prepare -> Process -> Cleanup`. step이 자기 task의 멤버라 그 task에 속하고, 각 task의 진입 step(`Entry()` 오버라이드)이 명시적이고 컴파일러로 검증됩니다. task는 한 번에 하나씩, 어디서든 `ctx_x_.StartFlow() -> StartResult`로 런치합니다.
- **Cmd -> WaitAt 2-step 모션 패턴** - 모든 축/그리퍼 동작은 한 쌍입니다: `Move(target)`을 내리고 전진하는 "명령" step과, `InPosition()`을 폴링하며 `Stay()`하는 "대기" step. 절대 블로킹하지 않는 협동 세계에서 장비 모션 코드의 자연스러운 골격입니다.
- **팩토리 뒤로 추상화된 하드웨어** - 하드웨어 타입은 `MotorAxis`(`Move` / `InPosition` / `Position`) 하나뿐입니다. flow는 자기가 쓰는 축만 `MotorIOFactory` 싱글턴이 내준 포인터로 보유하고, 팩토리는 모든 축을 스레드 하나로 적분합니다 - 애플리케이션은 절대 모터를 직접 스텝하지 않습니다. 가공 헤드 준비 핸드셰이크인 `DigitalLatch` IO도 같은 팩토리에 삽니다.
- **락 없는 상호 배제** - Load/Unload 피커는 zone B에 들어가기 전에 `PartnerInZoneB()`로 상대 위치를 확인합니다. 둘 다 같은 펌프 스레드 위에 있어 멤버 읽기로 충분합니다 - 뮤텍스도, 레이스도 없습니다.
- **오케스트레이터 패턴** - `Flow_Orchestrator`(단일 영속 `Schedule` task)가 라인 전체를 조율합니다: 원자재 생성(zone A가 비는 즉시 새 부품 투입)과 각 모듈이 idle일 때 *다음 task* 런치(`ctx_x_.StartFlow()`, `picker.Carrying()` / `stage.state()` 상태 구동). 피커와 Stage는 *어느 task*가 다음일지 스스로 결정하지 않습니다.
- **task별 transient 상태** - Stage의 `Prepare` task는 하드웨어 세틀 타이머를, `Process`는 가공 진행 타이머를 그 컨텍스트 struct의 멤버로 들고 있습니다. `OnEnter()`가 task 진입 시 재무장해 task 안의 `Stay()` 재진입을 가로지릅니다.
- **비동기 명령** - Stage는 시작/정리 명령을 `SubmitAsync(UF_FN(...))`로 내려 반환된 `AsyncId`를 다음 step으로 넘기고(`Next(UF_FN(...), id)`), 그 step에서 `AsyncResult<bool>(id)`로 응답을 폴링합니다 - `Pending`이면 `StayUntil`로 대기하다 2초 내 미응답이면 타임아웃 step에서 `ClearAsync()`로 워커를 포기하고 `Fail`. 모두 task 안에서.
- **콘솔 + 파일 이중 로깅** - `EnvLogObserver`가 `ConsoleObserver` 출력을 콘솔과 `pick_and_place.log` 양쪽에 미러링합니다. 실전 커스텀 옵저버입니다.

---

## 모델

| 모듈 | task | 역할 |
|---|---|---|
| `Flow_LoadPicker` | `Pick`, `Place` | 원자재 A -> B 운반 |
| `Flow_Stage` | `Prepare`, `Process`, `Cleanup` | zone B에서 가공; 완료 시 Unload 준비 |
| `Flow_UnloadPicker` | `Pick`, `Place` | 가공품 B -> C 운반 |
| `Flow_Orchestrator` | `Schedule` | 라인 스케줄러: 원자재 타이밍 + 각 모듈의 다음 task 런치 |
| `Flow_Visualization` | `Snapshot` | 실시간 라인 시각화 (Win32) |

모두 `App`의 멤버로, 2단계 초기화를 씁니다: 1단계는 모든 모듈을 생성(생성자 본문은 다른 모듈을 건드리지 않음), 2단계 `Start()`가 영속 task를 런치(`viz.ctx_snapshot_.StartFlow()`, `orch.ctx_schedule_.StartFlow()`). 각 flow는 생성자에서 `AddTask(ctx_...)`로 각 task를 등록하고(진입점은 task의 `Entry()` 오버라이드), 오케스트레이터가 모듈이 idle이 될 때마다 `ctx_x_.StartFlow()`로 작업 task를 런치합니다.

---

## step 하나, 처음부터 끝까지

task는 자기 step 멤버 함수와 공유 상태를 함께 들고 있는 struct(와 flow의 public 인스턴스 하나)이고, step은 task의 멤버라 그 task에 속합니다. task는 `Entry()`로 진입 step을 이름으로 밝힙니다:

```cpp
public:
    // public: 오케스트레이터가 ctx.StartFlow()로 런치
    struct Task_Pick : uniflow::Task<Flow_LoadPicker>
    {
        StepResult Entry() override { return Step1_CmdMoveToSource(); }

    private:                                 // step은 private - Entry/Next만 접근
        StepResult Step1_CmdMoveToSource();  // Pick step
        StepResult Step2_WaitAtSource();
        // ...
    } ctx_pick_;

    struct Task_Place : uniflow::Task<Flow_LoadPicker>
    {
        StepResult Entry() override { return Step1_CmdMoveToDest(); }

    private:
        StepResult Step1_CmdMoveToDest();    // Place step
        // ...
    } ctx_place_;
```

```cpp
// Pick task, 명령 step: 이동을 내리고 task 안에서 전진.
StepResult Flow_LoadPicker::Task_Pick::Step1_CmdMoveToSource()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: move to zone A");
    flow().x_->Move(GlobalGeometry::kZoneA_mm);   // flow()로 부모 flow에 접근
    return Next(UF_FN(Step2_WaitAtSource));       // 같은 task의 형제 step으로 전진
}

// Pick task, 마지막 step: 부품 들어올림 -> task 끝, flow는 idle로.
StepResult Flow_LoadPicker::Task_Pick::Step7_WaitAtPickUp()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("lifting with part");
    if (flow().z_->InPosition())
    {
        return Done();   // Pick 끝; 오케스트레이터가 다음 Place 런치(Carrying() 보고)
    }
    return Stay();
}
```

오케스트레이터가 바깥에서 task를 시퀀싱합니다:

```cpp
auto& p = App::inst().load;
if (p.IsIdle())
{
    if (p.Carrying())                   p.ctx_place_.StartFlow();   // 전달
    else if (GlobalEnv::ZoneAHasPart()) p.ctx_pick_.StartFlow();    // 집기
}
```

상태를 공유하는 task는 그 상태를 struct에 선언하고 `OnEnter()`에서 리셋합니다:

```cpp
struct Task_Prepare : uniflow::Task<Flow_Stage>
{
    void OnEnter() override { settle.Restart(); }   // task 진입 시 재무장
    StepResult Entry() override { return Step1_SendStart(); }

private:
    uniflow::UFTimer settle;
    StepResult Step1_SendStart();
    // ...
} ctx_prepare_;
```

step은 task의 멤버라서 빌려 쓰는 축(`x_`, `z_`, `finger_`)과 peer에 `flow()`로 닿습니다. `Next`는 같은 task 구조체의 형제 step만 가리키고(task 경계를 넘는 것은 명시적 `StartTask`), task가 끝나면 `Done()`을 반환해 오케스트레이터가 다음 task를 런치합니다. 모터는 팩토리 스레드에서 스스로 적분하므로, 대기 step은 `InPosition()`만 폴링할 뿐 축을 구동하지 않습니다.

---

## 읽어볼 만한 파일

- [uf_load_picker.h](uf_load_picker.h) / [.cpp](uf_load_picker.cpp) - `Pick -> Place` 단위 쌍 전체; Task-Level Syntax를 명확하게 보여주는 예
- [uf_stage.cpp](uf_stage.cpp) - 단위별 타이머, `SubmitAsync`, `StayUntil` 하드웨어 준비 타임아웃을 갖춘 `Prepare -> Process -> Cleanup`
- [uf_orchestrator.cpp](uf_orchestrator.cpp) - 스케줄링(생성 / 시작 결정) 전부, 단일 `Schedule` 단위로
- [app.h](app.h) - 2단계 초기화 패턴, Runtime Opts (스레드 / 옵저버 / 슬립 정책)
- [motor_io_factory.h](motor_io_factory.h) - `MotorAxis` / `DigitalLatch`와 이를 적분하는 단일 팩토리 스레드
- [env_log_observer.h](env_log_observer.h) - 콘솔 + 파일에 쓰는 커스텀 옵저버

---

## 빌드 / 실행

Win32 시각화를 쓰므로 Windows + MSVC 예제입니다. `.vcxproj`가 포함돼 있습니다.

```powershell
# Visual Studio
cpp\examples\pick_and_place\pick_and_place.vcxproj 를 솔루션에 추가하고 F5

# CLI (MSVC, vcvars64 환경)
cl /std:c++17 /EHsc /I cpp cpp\examples\pick_and_place\*.cpp /Fe:build\pick_and_place.exe /Fo:build\
```

라인 창이 열리고, step 전이가 콘솔과 `pick_and_place.log` 양쪽에 기록됩니다. 로그를 보면 라인이 도는 동안 단위 구조 - `Prepare` -> `Process` -> `Cleanup`, `Pick` -> `Place` 경계 - 가 또렷이 드러납니다. 종료 시 전달된 부품 수가 출력됩니다:

```text
parts delivered to Unload: 7
```

---

## 더 읽기

- Cmd -> WaitAt 폴링 패턴과 `Stay()`: [TUTORIAL.md](../../TUTORIAL.md)
- Task-Level Syntax 심화: [TUTORIAL.md](../../TUTORIAL.md)와 메인 [README](../../../README.kr.md#task-level-syntax)
- 커스텀 옵저버 작성: [TUTORIAL.md](../../TUTORIAL.md)
- 전체 예제 갤러리: [EXAMPLES.kr.md](../../EXAMPLES.kr.md)
