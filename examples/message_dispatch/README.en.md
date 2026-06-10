# message_dispatch

> 🌐 Language: [한국어](README.md) | **English** &nbsp;|&nbsp; [<- Example gallery](../../EXAMPLES.en.md)

Three modules (`UF_Professor`, `UF_Friend`, `UF_Student`) cooperate through a shared mailbox. The professor sends assignments, the friend sends invitations to play, and the student handles them in arrival order. If the student lacks ability it trains, if too tired it sleeps, if ready it does the assignment - and playing is a separate chain.

---

## What it shows

- A *three-module cooperative model running at once*. Inter-module communication is a lock-free mailbox + `IsIdle` polling
- A *conditional chain* within the student's single flow - `OnAssign_CheckAbility` branches the next step
- `UF_ASYNC(SimHours, n)` simulates an "n-hour task". While the student *works*, the pump drives the other modules
- A Win32 viz: a chip-based mailbox, ability/stress gauge bars, and the pending list from both sponsors

---

## Files worth reading

- [uf_student.cpp](uf_student.cpp) - the train / sleep / work / play branching
- [uf_visualization.cpp](uf_visualization.cpp) - a clean panel + gauge bars
- [app.h](app.h) - avoiding the `friend` keyword clash (the `friend_` member name)

---

## Build / run

It uses Win32 visualization, so this is a Windows + MSVC example.

```powershell
# Visual Studio
add examples\message_dispatch\message_dispatch.vcxproj to your solution and hit F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I . examples\message_dispatch\*.cpp /Fe:message_dispatch.exe
```

---

## Read more

- Conditional branching within a flow and async: [TUTORIAL.en.md chapters 2, 6](../../TUTORIAL.en.md)
- Inter-module communication: [TUTORIAL.en.md chapter 5, final chapter](../../TUTORIAL.en.md)
- Full example gallery: [EXAMPLES.en.md](../../EXAMPLES.en.md)
