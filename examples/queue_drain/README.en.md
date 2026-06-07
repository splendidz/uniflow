# queue_drain

> 🌐 Language: [한국어](README.md) | **English** &nbsp;|&nbsp; [<- Example gallery](../../EXAMPLES.en.md)

The classic "receiver thread -> queue -> worker thread" pattern, moved onto uniflow. `UF_Sender` pushes 1-10 messages into the queue every second, and `UF_Receiver` pulls them one at a time until the queue is empty.

---

## What it shows

- Why the message queue needs *no lock*: the sender and receiver run on the same pump thread
- The sender pushes messages and, if `receiver.IsIdle()`, wakes it immediately with `UF_START_FLOW`
- The receiver's dispatch pattern - `OnRecv_TakeNext` -> `OnRecv_Add` or `OnRecv_Sub` -> back to `OnRecv_TakeNext`
- A Win32 viz panel: the two source vectors, the current queue contents, the receiver state, and the last result

**Mapping to reality** - in a real system the sender comes from another machine/process. uniflow gives you the skeleton for *how you process it once received*. One mutex-friendly inbox plus one receiver module gives a real system the same shape.

---

## Files worth reading

- [uf_sender.cpp](uf_sender.cpp) - burst generation + waking the receiver
- [uf_receiver.cpp](uf_receiver.cpp) - the drain loop + dispatch
- [uf_visualization.cpp](uf_visualization.cpp) - a chip-based Win32 panel

---

## Build / run

It uses Win32 visualization, so this is a Windows + MSVC example.

```powershell
# Visual Studio
add examples\queue_drain\queue_drain.vcxproj to your solution and hit F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I . examples\queue_drain\*.cpp /Fe:queue_drain.exe
```

---

## Read more

- Inter-module communication and IsIdle polling: [TUTORIAL.en.md chapter 5, final chapter](../../TUTORIAL.en.md)
- Full example gallery: [EXAMPLES.en.md](../../EXAMPLES.en.md)
