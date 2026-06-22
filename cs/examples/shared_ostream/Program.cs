// shared_ostream - two Flow_Writer modules append to ONE shared buffer at the
// same time. They appear to race, but the Runtime drives every step on a single
// pump thread, so the shared sink stays consistent without a lock.
//
// Each writer takes (text, count, turnId). One writes "Hello " ten times, the
// other writes "World. " ten times. A shared turn flag forces them to alternate
// so the output reads:
//     "Hello World. Hello World. ..."  (x10)
//
// At the end we count how many times the substring "Hello World." appears - it
// must be exactly the configured repeat count. This is a finite program: it runs
// the writers, prints the interleaved-yet-ordered output, verifies it, then
// exits 0 (nonzero on FAIL).
//
// FEATURE FOCUS: lock-free shared state on one pump thread.
//
// Closest reference: python/examples/shared_ostream.py
// C++ original:      cpp/examples/shared_ostream/

using System;
using System.Collections.Generic;
using Uniflow;

namespace SharedOstreamExample
{
    // ----- shared_state.h/.cpp -----
    // The single sink every writer touches, plus the turn flag. Same role as the
    // C++ SharedState static members. Because every step runs on the ONE pump
    // thread, no lock is needed: appends to Log and reads/writes of Turn cannot
    // race even though "different modules" drive them.
    internal static class SharedState
    {
        // The single sink every writer appends to (C++: std::ostringstream Log()).
        private static readonly List<string> _log = new List<string>();

        // Whose turn is it to write? 0 = first writer, 1 = second writer.
        // Compared and flipped from inside step bodies on the pump thread.
        private static int _turn = 0;

        public static string Log()
        {
            return string.Concat(_log);
        }

        public static void Append(string text)
        {
            _log.Add(text);
        }

        public static int Turn()
        {
            return _turn;
        }

        public static void FlipTurn()
        {
            _turn = 1 - _turn;
        }
    }

    // ----- uf_writer.h/.cpp -----
    // One writer module: when it is its turn, append 'text' to the shared sink
    // 'count' times. Two instances share the same Runtime and alternate via the
    // shared turn flag.
    //
    // The interesting bit: no lock anywhere. Both modules run their steps on the
    // Runtime's single pump thread, so writes to SharedState.Log() and reads of
    // SharedState.Turn() cannot race.
    internal sealed class Flow_Writer : Module
    {
        // text      - what to append every time it is this writer's turn
        // remaining - how many appends are still pending
        // turnId    - 0 or 1; the writer waits while the shared turn flag != turnId
        public readonly string Text;
        public int Remaining;
        public readonly int TurnId;

        // The flow's single task; AddTask wires its Flow back-pointer so its steps
        // reach this module.
        public readonly Task_Write CtxWrite;

        public Flow_Writer(Runtime rt, string text, int count, int turnId)
            : base(rt, "Flow_Writer")
        {
            Text = text;
            Remaining = count;
            TurnId = turnId;
            CtxWrite = new Task_Write();
            AddTask(CtxWrite);
        }

        // The flow's single task (C++: struct Task_Write : uniflow::Task<Flow_Writer>).
        public sealed class Task_Write : Task<Flow_Writer>
        {
            protected override StepResult Entry() => Step1_Begin();

            // Step 1: announce the work, then advance to the append loop. Next()
            // stays within this task and re-enters at Step2_Loop next pump round.
            private StepResult Step1_Begin()
            {
                Describe($"begin: will append \"{Flow.Text}\" x {Flow.Remaining}");
                return Next(Step2_Loop);
            }

            // Step 2: the lock-free core. Both writers run this on the SAME pump
            // thread, so touching the shared buffer and the shared turn flag needs
            // no lock.
            private StepResult Step2_Loop()
            {
                if (Flow.Remaining <= 0)
                {
                    Describe("all writes done");
                    return Done();   // task finished; the module goes idle
                }
                if (SharedState.Turn() != Flow.TurnId)
                {
                    Describe("waiting for turn");
                    return Stay();   // not our turn yet - poll again next round
                }

                SharedState.Append(Flow.Text);   // shared sink, no lock
                SharedState.FlipTurn();           // hand the turn to the peer
                Flow.Remaining -= 1;
                Describe($"appended \"{Flow.Text}\", remaining={Flow.Remaining}");
                return Stay();
            }
        }
    }

    // ----- app.h -----
    // One Runtime, two Flow_Writer instances. The trick is exactly that both share
    // the same pump thread: writing to one buffer from "different modules" works
    // without a lock.
    internal sealed class App
    {
        public const int Repeats = 10;

        public readonly Runtime Rt;
        private readonly Flow_Writer _hello;
        private readonly Flow_Writer _world;

        public App()
        {
            // The verification owns stdout; a plain Observer() suppresses the
            // ConsoleObserver trace so only the program's own output appears.
            // Stay() comes back immediately - the two writers ping-pong via the
            // turn flag, so spin both modules tight (no stay/step nap).
            var cfg = new Config { IdleSleep = 0.001, StaySleep = 0.0, StepIntervalSleep = 0.0 };
            Rt = new Runtime(new Runtime.Options { Observer = new Observer(), Config = cfg });
            _hello = new Flow_Writer(Rt, "Hello ", Repeats, 0);
            _world = new Flow_Writer(Rt, "World. ", Repeats, 1);
        }

        // Launch each writer's task on the pump.
        public void Start()
        {
            _hello.CtxWrite.StartFlow();
            _world.CtxWrite.StartFlow();
        }

        public void WaitForDone()
        {
            _hello.WaitUntilIdle();
            _world.WaitUntilIdle();
        }
    }

    // ----- main.cpp -----
    internal static class Program
    {
        private static int CountOccurrences(string hay, string needle)
        {
            if (string.IsNullOrEmpty(needle))
            {
                return 0;
            }
            int hits = 0;
            int pos = 0;
            while (true)
            {
                int idx = hay.IndexOf(needle, pos, StringComparison.Ordinal);
                if (idx < 0)
                {
                    break;
                }
                hits += 1;
                pos = idx + needle.Length;
            }
            return hits;
        }

        public static int Main()
        {
            Console.WriteLine("=== shared_ostream: two writers, one log, no locks ===\n");

            var app = new App();
            app.Start();
            app.WaitForDone();
            app.Rt.Stop();

            string outText = SharedState.Log();
            int got = CountOccurrences(outText, "Hello World.");

            Console.WriteLine("--- output ---");
            Console.WriteLine(outText);
            Console.WriteLine("--- end ---\n");
            Console.WriteLine($"expected \"Hello World.\" occurrences = {App.Repeats}, got = {got}");
            if (got == App.Repeats)
            {
                Console.WriteLine("PASS: shared log is race-free, order preserved");
            }
            else
            {
                Console.WriteLine("FAIL: order was not preserved");
            }

            return got == App.Repeats ? 0 : 1;
        }
    }
}
