"""shared_ostream - two Flow_Writer modules append to ONE shared buffer at the
same time. They appear to race, but the Runtime drives every step on a single
pump thread, so the shared sink stays consistent without a lock.

Each writer takes (text, count, turn_id). One writes "Hello " ten times, the
other writes "World. " ten times. A shared turn flag forces them to alternate so
the output reads:
    "Hello World. Hello World. ..."  (x10)

At the end we count how many times the substring "Hello World." appears - it
must be exactly the configured repeat count. This is a finite program: it runs
the writers, prints the interleaved-yet-ordered output, verifies it, then exits.

FEATURE FOCUS: lock-free shared state on one pump thread.

C++ 원본: cpp/examples/shared_ostream/ (uf_writer, shared_state, app.h, main.cpp).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import uniflow


# ----- shared_state.h/.cpp -----
# 모든 writer 가 건드리는 단일 sink 와 turn 플래그. C++ 의 SharedState 정적
# 멤버와 동일한 역할. 모든 step 이 ONE 펌프 스레드에서 돌기 때문에 락이 필요 없다.
class SharedState:
    # The single sink every writer appends to (C++: std::ostringstream Log()).
    log = []

    # Whose turn is it to write? 0 = first writer, 1 = second writer.
    # Compared and flipped from inside step bodies on the pump thread.
    turn = 0

    @classmethod
    def Log(cls):
        return "".join(cls.log)

    @classmethod
    def Append(cls, text):
        cls.log.append(text)

    @classmethod
    def Turn(cls):
        return cls.turn

    @classmethod
    def FlipTurn(cls):
        cls.turn = 1 - cls.turn


# ----- uf_writer.h/.cpp -----
# 하나의 writer 모듈: 자기 turn 일 때 shared sink 에 'text' 를 'count' 번 append.
# 두 인스턴스가 같은 Runtime 을 공유하고, 공유 turn 플래그로 교대한다.
#
# The interesting bit: no lock anywhere. Both modules run their steps on the
# Runtime's single pump thread, so writes to SharedState.Log() and reads of
# SharedState.Turn() cannot race.
class Flow_Writer(uniflow.Uniflow):
    # text    - what to append every time it is this writer's turn
    # count   - how many times to append
    # turn_id - 0 or 1; the writer waits while the shared turn flag != turn_id
    def __init__(self, rt, text, count, turn_id):
        super().__init__(rt, name="Flow_Writer")
        self.text = text
        self.remaining = count
        self.turn_id = turn_id
        # The flow's single task; AddTask wires flow() so its steps reach here.
        self.ctx_write = self.Task_Write()
        self.AddTask(self.ctx_write)

    def Remaining(self):
        return self.remaining

    # The flow's single task (C++: struct Task_Write : uniflow::Task<Flow_Writer>).
    class Task_Write(uniflow.Task):
        def Entry(self):
            return self.Step1_Begin()

        # Step 1: announce the work, then advance to the append loop. Next() stays
        # within this task and re-enters at Step2_Loop on the next pump round.
        def Step1_Begin(self):
            f = self.flow()
            self.Describe('begin: will append "', f.text, '" x ', f.remaining)
            return self.Next(self.Step2_Loop)

        # Step 2: the lock-free core. Both writers run this on the SAME pump
        # thread, so touching the shared buffer and the shared turn flag needs
        # no lock.
        def Step2_Loop(self):
            f = self.flow()
            if f.remaining <= 0:
                self.Describe("all writes done")
                return self.Done()   # task finished; the module goes idle
            if SharedState.Turn() != f.turn_id:
                self.Describe("waiting for turn")
                return self.Stay()   # not our turn yet - poll again next round

            SharedState.Append(f.text)   # shared sink, no lock
            SharedState.FlipTurn()       # hand the turn to the peer
            f.remaining -= 1
            self.Describe('appended "', f.text, '", remaining=', f.remaining)
            return self.Stay()


# ----- app.h -----
# 하나의 Runtime, 두 Flow_Writer 인스턴스. 둘 다 같은 펌프 스레드를 공유하는 것이
# 바로 그 트릭이다: "다른 모듈" 에서 같은 버퍼로 쓰는 것이 락 없이 가능하다.
class App:
    kRepeats = 10

    def __init__(self):
        # The verification owns stdout; an empty Observer() suppresses the
        # default ConsoleObserver trace so only the program's own output appears.
        # Stay() comes back immediately - the two writers ping-pong via the turn
        # flag, so spin both modules tight.
        config = uniflow.Config(idle_sleep=0.001, stay_sleep=0.0,
                                step_interval_sleep=0.0)
        self.rt = uniflow.Runtime(observer=uniflow.Observer(), config=config)
        self.hello = Flow_Writer(self.rt, "Hello ", self.kRepeats, 0)
        self.world = Flow_Writer(self.rt, "World. ", self.kRepeats, 1)

    # Phase 2 - launch each writer's task on the pump.
    def Start(self):
        self.hello.ctx_write.StartFlow()
        self.world.ctx_write.StartFlow()

    def WaitForDone(self):
        self.hello.WaitUntilIdle()
        self.world.WaitUntilIdle()


# ----- main.cpp -----
def count_occurrences(hay, needle):
    if not needle:
        return 0
    hits = 0
    pos = 0
    while True:
        pos = hay.find(needle, pos)
        if pos < 0:
            break
        hits += 1
        pos += len(needle)
    return hits


def main():
    print("=== shared_ostream: two writers, one log, no locks ===\n")

    app = App()
    app.Start()
    app.WaitForDone()
    app.rt.stop()

    out = SharedState.Log()
    got = count_occurrences(out, "Hello World.")

    print("--- output ---")
    print(out)
    print("--- end ---\n")
    print('expected "Hello World." occurrences = {}, got = {}'.format(
        App.kRepeats, got))
    if got == App.kRepeats:
        print("PASS: shared log is race-free, order preserved")
    else:
        print("FAIL: order was not preserved")

    return 0 if got == App.kRepeats else 1


if __name__ == "__main__":
    sys.exit(main())
