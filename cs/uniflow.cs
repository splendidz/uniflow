// uniflow.cs - single-threaded, step-driven cooperative async framework.
//
// C# port of cpp/uniflow.hpp (and python/uniflow.py). The public surface mirrors
// the C++ and Python ports so all three read alike (Task-Level Syntax): a module
// subclasses Module (the C++ Uniflow<Derived>) and owns one or more Task
// instances; each Task subclass owns its step METHODS and the state they share.
// A step returns a StepResult intent (Stay / Next / Done / Fail). One Runtime
// pump thread drives every attached module round-robin; blocking work goes to a
// thread pool via SubmitAsync and is polled by AsyncId - the pump never blocks.
//
// Target: net9.0, BCL only. ASCII only. Nullable enabled.

#nullable enable

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

namespace Uniflow
{
    // ----- Version -----
    // uniflow's own version. Exposed as a semver string and a tuple.
    public static class Info
    {
        public const string Version = "1.0.0";
        public static readonly (int Major, int Minor, int Patch) VersionTuple = (1, 0, 0);
    }

    // ----- StepAction: a step returns an intent, not a state change -----
    // Four intents only: Stay, Next, Done, Fail.
    public enum StepAction
    {
        Stay, // re-run the same step
        Next, // advance to the next step on the next pump round
        Done, // flow completed normally -> idle
        Fail  // flow aborted -> idle
    }

    // ----- StartResult: outcome of StartTask / StartFlow (launching a task) -----
    //   Ok   - the task was launched.
    //   Busy - a task is already running on this module; nothing happened.
    public enum StartResult
    {
        Ok,
        Busy
    }

    // ----- AsyncState: classifies one async submission slot -----
    public enum AsyncState
    {
        NotFound, // id never matched a live slot (bad id, 0, or cleared)
        Pending,  // the worker is still in flight
        Done,     // the worker returned - ReturnValue holds it
        Failed,   // the worker threw
        TimedOut  // the worker missed its deadline
    }

    // ----- StepResult: what a step returns - an intent, not a state change -----
    // Next/StayUntil carry a NextFn target; StayUntil also carries a TimeoutSec
    // (logical-time deadline from step entry; 0 means a plain Stay with no
    // step timeout).
    public sealed class StepResult
    {
        public StepAction Action { get; }
        public Func<StepResult>? NextFn { get; }
        public string NextName { get; }
        public string Reason { get; }
        public double TimeoutSec { get; }

        // StayUntil with a wait condition: Cond is polled each round and, once it
        // has stayed true continuously for SettleSec (post-wait / settling), the
        // step transitions to SuccessFn. NextFn stays the timeout target. Cond
        // null means the classic timeout-only Stay.
        public Func<bool>? Cond { get; }
        public double SettleSec { get; }
        public Func<StepResult>? SuccessFn { get; }
        public string SuccessName { get; }

        internal StepResult(StepAction action, Func<StepResult>? nextFn = null,
                            string nextName = "", string reason = "", double timeoutSec = 0.0,
                            Func<bool>? cond = null, double settleSec = 0.0,
                            Func<StepResult>? successFn = null, string successName = "")
        {
            Action = action;
            NextFn = nextFn;
            NextName = nextName;
            Reason = reason;
            TimeoutSec = timeoutSec;
            Cond = cond;
            SettleSec = settleSec;
            SuccessFn = successFn;
            SuccessName = successName;
        }
    }

    // ----- AsyncOutcome<T>: by-value snapshot of one async submission -----
    // Returned by AsyncResult<T>(id). 'State' classifies the slot; 'ReturnValue'
    // holds the worker's result and is meaningful ONLY when State == Done. A bad /
    // cleared / rejected id (including 0) reads back as NotFound.
    public sealed class AsyncOutcome<T>
    {
        public AsyncState State { get; }
        public T ReturnValue { get; }

        internal AsyncOutcome(AsyncState state, T returnValue = default!)
        {
            State = state;
            ReturnValue = returnValue;
        }

        public bool Ok => State == AsyncState.Done;
        public bool Pending => State == AsyncState.Pending;
        public bool Failed => State == AsyncState.Failed;
        public bool IsTimeout => State == AsyncState.TimedOut;
        public bool Found => State != AsyncState.NotFound;
    }

    // ----- Observer -----
    // Every framework event funnels through one of these hooks; the framework
    // never touches stdout itself. Subclass and override only the events you care
    // about. A plain 'new Observer()' is the silent observer. Hooks run on the
    // pump thread.
    public class Observer
    {
        public virtual void OnFlowStarted(string obj, string firstStep) { }
        public virtual void OnStepChanged(string obj, string prevStep, string nextStep,
                                          string description, int stepOrdinal,
                                          double elapsedMs, int ticks) { }
        public virtual void OnStepThrew(string obj, string step, string what,
                                        int stepOrdinal, int tick) { }
        public virtual void OnAsyncSubmitted(string obj, string step, string job) { }
        public virtual void OnAsyncCompleted(string obj, string job, double waitMs,
                                             bool hadError, bool timedOut) { }
        public virtual void OnAsyncAbandoned(string obj, string job, double pendingMs) { }
        public virtual void OnAsyncHighWater(string obj, string job, int inflight) { }
        public virtual void OnFlowEnded(string obj, StepAction terminalAction,
                                        int finalStepOrdinal, double wallMs, string reason) { }
    }

    // ----- ConsoleObserver -----
    // Pretty-prints events to stdout, one line each. Thread-safe.
    public class ConsoleObserver : Observer
    {
        private const int ColObj = 16;
        private const int ColStep = 28;
        private const int ColDesc = 30;

        private readonly object _mu = new object();

        private void Print(string line)
        {
            lock (_mu)
            {
                Console.WriteLine(line);
            }
        }

        private static string Pad(string s, int width)
        {
            return (s ?? "").PadRight(width);
        }

        public override void OnFlowStarted(string obj, string firstStep)
        {
            Print($"[{Pad(obj, ColObj)}] FLOW START  -> {firstStep}");
        }

        public override void OnStepChanged(string obj, string prevStep, string nextStep,
                                           string description, int stepOrdinal,
                                           double elapsedMs, int ticks)
        {
            string transition = string.IsNullOrEmpty(nextStep) ? prevStep : $"{prevStep} -> {nextStep}";
            Print($"[{Pad(obj, ColObj)}] {Pad(transition, ColStep)} {Pad(description, ColDesc)} "
                  + $"#{stepOrdinal:D2} elapsed={elapsedMs:F2}ms tick x{ticks}");
        }

        public override void OnStepThrew(string obj, string step, string what,
                                         int stepOrdinal, int tick)
        {
            Print($"[{Pad(obj, ColObj)}] {Pad(step, ColStep)} [THREW] {what} #{stepOrdinal:D2}");
        }

        public override void OnAsyncSubmitted(string obj, string step, string job)
        {
            Print($"[{Pad(obj, ColObj)}] {Pad(step, ColStep)} ASYNC SUBMIT  {job}");
        }

        public override void OnAsyncCompleted(string obj, string job, double waitMs,
                                              bool hadError, bool timedOut)
        {
            string tag = timedOut ? " [TIMEOUT]" : (hadError ? " [ERROR]" : "");
            Print($"[{Pad(obj, ColObj)}] {Pad("", ColStep)} ASYNC DONE    {job}  wait={waitMs:F2}ms{tag}");
        }

        public override void OnAsyncAbandoned(string obj, string job, double pendingMs)
        {
            Print($"[{Pad(obj, ColObj)}] {Pad("", ColStep)} [ASYNC ABANDONED]  {job}  pending={pendingMs:F2}ms");
        }

        public override void OnAsyncHighWater(string obj, string job, int inflight)
        {
            Print($"[{Pad(obj, ColObj)}] {Pad("", ColStep)} [ASYNC HIGHWATER]  {job} rejected, inflight={inflight}");
        }

        public override void OnFlowEnded(string obj, StepAction terminalAction,
                                         int finalStepOrdinal, double wallMs, string reason)
        {
            string extra = string.IsNullOrEmpty(reason) ? "" : $"  reason={reason}";
            Print($"[{Pad(obj, ColObj)}] FLOW END  {Pad(terminalAction.ToString(), 5)} "
                  + $"steps=#{finalStepOrdinal:D2} wall={wallMs:F2}ms{extra}");
        }
    }

    // Wraps an observer so a hook exception cannot kill the pump thread. Runtime
    // wraps every observer in this.
    internal sealed class SafeObserver : Observer
    {
        private readonly Observer _inner;

        public SafeObserver(Observer inner) { _inner = inner; }

        private static void Guard(Action a) { try { a(); } catch { } }

        public override void OnFlowStarted(string o, string f)
            => Guard(() => _inner.OnFlowStarted(o, f));
        public override void OnStepChanged(string o, string p, string n, string d, int so, double e, int t)
            => Guard(() => _inner.OnStepChanged(o, p, n, d, so, e, t));
        public override void OnStepThrew(string o, string s, string w, int so, int t)
            => Guard(() => _inner.OnStepThrew(o, s, w, so, t));
        public override void OnAsyncSubmitted(string o, string s, string j)
            => Guard(() => _inner.OnAsyncSubmitted(o, s, j));
        public override void OnAsyncCompleted(string o, string j, double w, bool he, bool to)
            => Guard(() => _inner.OnAsyncCompleted(o, j, w, he, to));
        public override void OnAsyncAbandoned(string o, string j, double p)
            => Guard(() => _inner.OnAsyncAbandoned(o, j, p));
        public override void OnAsyncHighWater(string o, string j, int i)
            => Guard(() => _inner.OnAsyncHighWater(o, j, i));
        public override void OnFlowEnded(string o, StepAction a, int fo, double w, string r)
            => Guard(() => _inner.OnFlowEnded(o, a, fo, w, r));
    }

    // ----- VirtualClock: scalable / freezable LOGICAL time -----
    // The time source for UFTimer and the StayUntil step deadline. By default it
    // tracks the real monotonic clock 1:1, but it can be sped up / slowed down
    // (SetScale) or paused (Freeze/Resume). It governs ONLY logical waits;
    // async/IO deadlines and the pump's own sleeps stay on real wall-clock.
    // Now() is computed fresh on every call. Thread-safe. Seconds, monotonic.
    public sealed class VirtualClock
    {
        private readonly object _mu = new object();
        private double _baseReal;     // real seconds at last rebase
        private double _baseVirtual;  // virtual seconds at last rebase
        private double _scale = 1.0;
        private bool _frozen = false;

        private static double RealNow()
        {
            return Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency;
        }

        public VirtualClock()
        {
            _baseReal = RealNow();
            _baseVirtual = _baseReal;
        }

        private double NowLocked()
        {
            if (_frozen)
            {
                return _baseVirtual;
            }
            return _baseVirtual + (RealNow() - _baseReal) * _scale;
        }

        public double Now()
        {
            lock (_mu)
            {
                return NowLocked();
            }
        }

        // Capture current virtual time as the new origin so a scale / freeze
        // change does not discontinuously move Now().
        private void RebaseLocked()
        {
            _baseVirtual = NowLocked();
            _baseReal = RealNow();
        }

        public void SetScale(double scale)
        {
            lock (_mu)
            {
                RebaseLocked();
                _scale = scale;
            }
        }

        public double Scale()
        {
            lock (_mu)
            {
                return _scale;
            }
        }

        public void Freeze()
        {
            lock (_mu)
            {
                RebaseLocked();
                _frozen = true;
            }
        }

        public void Resume()
        {
            lock (_mu)
            {
                if (_frozen)
                {
                    _baseReal = RealNow();
                    _frozen = false;
                }
            }
        }

        public bool Frozen()
        {
            lock (_mu)
            {
                return _frozen;
            }
        }
    }

    // Process-wide real-time clock: the default source for a standalone UFTimer
    // (scale 1, never frozen).
    internal static class RealClockHolder
    {
        public static readonly VirtualClock Instance = new VirtualClock();
    }

    // ----- UFTimer: polling timer for step waits -----
    // HeldFor answers "has the condition STAYED true long enough?" (settling);
    // Elapsed / Passed answer the raw "how long since armed?". A standalone timer
    // reads the process real clock; UFTimer(rt.Clock) binds it to the runtime's
    // virtual clock (scale / freeze). Seconds.
    public sealed class UFTimer
    {
        private readonly VirtualClock _clk;
        private double _armedAt;
        private double? _condSince;

        public UFTimer(VirtualClock? clock = null)
        {
            _clk = clock ?? RealClockHolder.Instance;
            _armedAt = _clk.Now();
            _condSince = null;
        }

        public void Restart()
        {
            _armedAt = _clk.Now();
            _condSince = null;
        }

        public double Elapsed()
        {
            return _clk.Now() - _armedAt;
        }

        public bool Passed(double seconds)
        {
            return Elapsed() >= seconds;
        }

        // True once 'condition' has held continuously for 'seconds' since it first
        // turned true; any read of false resets the accumulator. Level semantics.
        public bool HeldFor(bool condition, double seconds)
        {
            if (!condition)
            {
                _condSince = null;
                return false;
            }
            if (_condSince == null)
            {
                _condSince = _clk.Now();
            }
            return (_clk.Now() - _condSince.Value) >= seconds;
        }
    }

    // ----- Config: per-Runtime tuning -----
    // Pump-thread sleep knobs (seconds). IdleSleepSec: no flow running anywhere;
    // StaySleepSec: flows running but every active module Stay'd this round;
    // StepIntervalSleepSec: at least one module advanced this round (0 = burst).
    public sealed class Config
    {
        public double IdleSleepSec { get; set; } = 0.001;
        public double StaySleepSec { get; set; } = 0.02;
        public double StepIntervalSleepSec { get; set; } = 0.0;
        public int MaxInflightAsync { get; set; } = 64;
    }

    // One in-flight (or just-resolved) async submission, owned by the module. The
    // worker writes into 'Task'; the pump's per-round sweep moves it into
    // Value/Exc and flips 'Done'. The worker never touches the slot or the module,
    // so a dropped slot (ClearAsync) simply abandons the worker - it finishes
    // harmlessly into an orphaned Task.
    internal sealed class AsyncSlot
    {
        public int Id;
        public string Label = "";
        public double? TimeoutSec;
        public double SubmittedAtSec;          // real seconds
        public System.Threading.Tasks.Task<object?> Task = null!;
        public bool Done;
        public object? Value;
        public Exception? Exc;
        public bool TimedOut;
    }

    // Internal interface used by Module.AddTask to wire a task's back-reference
    // without reflection on the typed Flow property.
    internal interface ITaskBinding
    {
        void BindFlow(Module module);
        StepResult InvokeEntry();
        void InvokeOnEnter();
    }

    // ----- Task<TFlow> - the per-task base a user task derives from -----
    // A unit operation: a set of step methods that share the state this object
    // owns. Subclass it, define step methods, override Entry() to name the first
    // step, and optionally OnEnter() to re-arm per-task state on entry. Flow
    // reaches the owning module (wired by AddTask).
    public abstract class Task<TFlow> : ITaskBinding where TFlow : Module
    {
        private Module? _module;

        // The owning module (typed). Wired by AddTask in the module ctor.
        protected TFlow Flow => (TFlow)_module!;

        // Class name of this task ("Task_Pick"), the per-task identity the
        // module reports through CurrentTaskName.
        public string Name => GetType().Name;

        // ----- overridable -----
        protected abstract StepResult Entry();
        protected virtual void OnEnter() { }

        // ----- ITaskBinding (framework-internal) -----
        void ITaskBinding.BindFlow(Module module) { _module = module; }
        StepResult ITaskBinding.InvokeEntry() => Entry();
        void ITaskBinding.InvokeOnEnter() => OnEnter();

        // ----- step intents -----
        protected StepResult Stay()
        {
            return new StepResult(StepAction.Stay);
        }

        protected StepResult Done()
        {
            return new StepResult(StepAction.Done);
        }

        protected StepResult Fail(string reason = "")
        {
            return new StepResult(StepAction.Fail, reason: reason);
        }

        // Advance to a sibling step of THIS task. The step name is captured from
        // the delegate's method name for tracing.
        protected StepResult Next(Func<StepResult> step)
        {
            return new StepResult(StepAction.Next, nextFn: step, nextName: StepName(step));
        }

        // Wait for 'condition' with a settle window and a timeout catch, all in
        // one step intent. Each round 'condition' is polled; once it has stayed
        // true continuously for 'settleSec' (post-wait / settling) the step
        // transitions to 'success'. If 'timeoutSec' of LOGICAL time elapses since
        // step entry first, it transitions to 'timeoutStep' instead. The deadline
        // is measured from step entry, not from this call. For a plain timeout
        // escape with no condition (the body decides success), use StayTimeout.
        //
        //   return StayUntil(() => SensorOn, 3, Step_Clamp, 100, Step_Error);
        protected StepResult StayUntil(Func<bool> condition, double settleSec,
                                       Func<StepResult> success, double timeoutSec,
                                       Func<StepResult> timeoutStep)
        {
            return new StepResult(StepAction.Stay, nextFn: timeoutStep,
                                  nextName: StepName(timeoutStep), timeoutSec: timeoutSec,
                                  cond: condition, settleSec: settleSec, successFn: success,
                                  successName: StepName(success));
        }

        // Plain step deadline (the original StayUntil): keep polling THIS step,
        // but if 'timeoutSec' of LOGICAL time elapses since step entry, transition
        // to 'timeoutStep' - the step-level "catch". The body decides the success
        // path with its own Next/Done/Fail; this only guarantees an exit if the
        // wait never resolves.
        //
        //   return StayTimeout(2.0, Step_AckTimeout);
        protected StepResult StayTimeout(double timeoutSec, Func<StepResult> timeoutStep)
        {
            return new StepResult(StepAction.Stay, nextFn: timeoutStep,
                                  nextName: StepName(timeoutStep), timeoutSec: timeoutSec);
        }

        // Switch the module to another of its tasks mid-flow (rare). Next never
        // leaves the current task; this is the only in-task way to cross over.
        protected StartResult StartTask(object otherTask)
        {
            return _module!.SwitchTask(this, otherTask);
        }

        // Launch THIS task from any thread; sugar for module.StartTask(this).
        public StartResult StartFlow()
        {
            return _module!.StartTask(this);
        }

        protected void Describe(string text)
        {
            _module!.Describe(text);
        }

        // ----- async (forwarded to the owning module) -----
        protected int SubmitAsync(Func<object?> work, string label, double? timeoutSec = null)
        {
            return _module!.SubmitAsyncImpl(work, label, timeoutSec);
        }

        protected AsyncOutcome<T> AsyncResult<T>(int asyncId)
        {
            return _module!.AsyncResultImpl<T>(asyncId);
        }

        protected bool AnyAsyncPending()
        {
            return _module!.AnyAsyncPendingImpl();
        }

        protected void ClearAsync()
        {
            _module!.ClearAsyncImpl();
        }

        private static string StepName(Func<StepResult> step)
        {
            return step.Method?.Name ?? "?";
        }
    }

    // ----- Module: module base (the C++ Uniflow<Derived>) -----
    // Holds one or more Task instances and runs ONE at a time. Attaches to a
    // Runtime in the ctor and is driven round-robin by that runtime's pump. Bind
    // tasks with AddTask(task) once each in the ctor; launch with StartTask(task)
    // (or task.StartFlow()).
    public abstract class Module
    {
        private readonly Runtime _rt;
        private readonly string _name;
        private readonly object _lock = new object();

        // Per-module built-in timer, bound to the runtime clock (scale / freeze).
        protected UFTimer UfTimer { get; }

        // Timers re-armed on every step change (Next / StayUntil timeout / flow
        // start / task switch) but NOT on a Stay. The built-in UfTimer is always
        // registered; user member timers opt in via NewAutoTimer().
        private readonly List<UFTimer> _autoTimers = new List<UFTimer>();

        // StayUntil-with-condition settle accumulator: the virtual-clock instant
        // the wait condition last turned true (null = not currently held). Reset
        // on every step change so settle is measured from within the step.
        private double? _settleSinceSec;

        private readonly List<ITaskBinding> _tasks = new List<ITaskBinding>();

        // -- current position within the running flow --
        private bool _flowRunning;
        private ITaskBinding? _activeTask;
        private Func<StepResult>? _current;
        private string _currentName = "";
        private string _desc = "";
        private ITaskBinding? _unitEntered;
        private bool _flowStartedPending;

        private int _stepOrdinal;
        private double _flowT0Sec;   // real seconds
        private double _stepRealT0Sec;   // real seconds (profiling elapsedMs)
        private double _stepVirtT0Sec;  // virtual seconds (StayUntil deadline)
        private int _stepTicks;

        public bool Failed { get; private set; }
        public string FailReason { get; private set; } = "";
        public Exception? FailExc { get; private set; }

        // -- async slots: every in-flight / just-resolved submission for the flow.
        private List<AsyncSlot> _asyncSlots = new List<AsyncSlot>();
        private int _nextAsyncId = 1;

        protected Module(Runtime rt, string? name = null)
        {
            _rt = rt;
            _name = name ?? GetType().Name;
            UfTimer = new UFTimer(rt.Clock);
            _autoTimers.Add(UfTimer);
            rt.Attach(this);
        }

        // Create a UFTimer bound to this runtime's clock and register it for
        // auto-reset: it re-arms on every step change, like the built-in UfTimer.
        // For a self-managed timer, construct new UFTimer(Clock) directly instead.
        protected UFTimer NewAutoTimer()
        {
            var t = new UFTimer(_rt.Clock);
            _autoTimers.Add(t);
            return t;
        }

        // Re-arm every auto-reset timer and clear the StayUntil settle
        // accumulator. Called on each step change (NOT on a Stay).
        private void ResetStepTimers()
        {
            foreach (var t in _autoTimers)
            {
                t.Restart();
            }
            _settleSinceSec = null;
        }

        // The runtime's logical clock, reachable from a step via Flow.Clock.
        public VirtualClock Clock => _rt.Clock;

        // Convenience: current logical time in seconds (Flow.ClockNow()).
        public double ClockNow() => _rt.Clock.Now();

        private static double RealNow()
        {
            return Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency;
        }

        // ----- task binding -----
        // Wire the task's Flow back-pointer so its steps reach this module and
        // task.StartFlow() knows which module to launch. Does NOT start anything.
        // Call once per task in the ctor.
        protected void AddTask(object task)
        {
            var binding = task as ITaskBinding
                ?? throw new ArgumentException("AddTask expects a Task<TFlow> subclass.");
            binding.BindFlow(this);
            _tasks.Add(binding);
        }

        // Launch this module's flow at 'task's Entry(). Callable from ANY thread.
        // Ok if launched, Busy if a task is already running on the module.
        public StartResult StartTask(object task)
        {
            var binding = task as ITaskBinding
                ?? throw new ArgumentException("StartTask expects a Task<TFlow> subclass.");
            return ArmFlow(binding);
        }

        private StartResult ArmFlow(ITaskBinding task)
        {
            lock (_lock)
            {
                if (_flowRunning)
                {
                    return StartResult.Busy;
                }
                double now = RealNow();
                _flowRunning = true;
                _activeTask = task;
                _current = EntryThunk(task);
                _currentName = "Entry";
                _desc = "";
                _stepOrdinal = 0;
                _stepTicks = 0;
                Failed = false;
                FailReason = "";
                FailExc = null;
                _flowT0Sec = now;
                _stepRealT0Sec = now;
                _stepVirtT0Sec = _rt.Clock.Now();
                _asyncSlots = new List<AsyncSlot>();
                _nextAsyncId = 1;
                _unitEntered = null;
                _flowStartedPending = true;
            }
            // The first step is a step change too: arm the auto-reset timers so
            // the entry step sees fresh timers, not time accrued since construction.
            ResetStepTimers();
            _rt.Wake();
            return StartResult.Ok;
        }

        // Runs on the pump thread: enter the unit (OnEnter / re-arm) then call its
        // Entry(). Idempotent per task so a Stay-polling entry does not re-enter.
        private Func<StepResult> EntryThunk(ITaskBinding task)
        {
            return () =>
            {
                BeginUnit(task);
                return task.InvokeEntry();
            };
        }

        private void BeginUnit(ITaskBinding task)
        {
            if (!ReferenceEquals(_activeTask, task))
            {
                _activeTask = task;
            }
            if (!ReferenceEquals(_unitEntered, task))
            {
                _unitEntered = task;
                task.InvokeOnEnter();
            }
        }

        // In-task switch to another task of the same module. Runs on the pump
        // thread (called from within a step), so it installs the new task's entry
        // thunk as the current step directly; the new task enters at its Entry()
        // next round. Returns Ok unless the target is not bound to this module.
        internal StartResult SwitchTask(object from, object other)
        {
            var binding = other as ITaskBinding;
            if (binding == null || !_tasks.Contains(binding))
            {
                return StartResult.Busy;
            }
            lock (_lock)
            {
                _current = EntryThunk(binding);
                _currentName = "Entry";
                _desc = "";
                _stepOrdinal += 1;
                _stepRealT0Sec = RealNow();
                _stepVirtT0Sec = _rt.Clock.Now();
                _stepTicks = 0;
                _unitEntered = null;
            }
            // A task switch advances the step (new task's Entry next round): re-arm
            // the auto-reset timers, same as a Next transition does.
            ResetStepTimers();
            return StartResult.Ok;
        }

        // ----- waiting / introspection -----
        // Block the calling thread until the running flow finishes (returns at
        // once if already idle). Call from the owning thread, never inside a step.
        public bool WaitUntilIdle(double? timeoutSec = null)
        {
            double deadline = timeoutSec.HasValue ? RealNow() + timeoutSec.Value : 0.0;
            lock (_lock)
            {
                while (_flowRunning)
                {
                    if (timeoutSec.HasValue)
                    {
                        double remaining = deadline - RealNow();
                        if (remaining <= 0)
                        {
                            return false;
                        }
                        Monitor.Wait(_lock, TimeSpan.FromSeconds(remaining));
                    }
                    else
                    {
                        Monitor.Wait(_lock);
                    }
                }
                return true;
            }
        }

        public string InstanceName => _name;

        public bool IsIdle
        {
            get
            {
                lock (_lock)
                {
                    return !_flowRunning;
                }
            }
        }

        public string CurrentStepName
        {
            get
            {
                lock (_lock)
                {
                    return _flowRunning ? _currentName : "";
                }
            }
        }

        public int CurrentStepOrdinal
        {
            get
            {
                lock (_lock)
                {
                    return _flowRunning ? _stepOrdinal : -1;
                }
            }
        }

        // Class name of the task currently running ("Task_Pick"), empty when
        // idle. Tracks an in-task StartTask switch.
        public string CurrentTaskName
        {
            get
            {
                lock (_lock)
                {
                    return _flowRunning && _activeTask != null
                        ? _activeTask.GetType().Name : "";
                }
            }
        }

        public string CurrentStepDescription
        {
            get
            {
                lock (_lock)
                {
                    return _flowRunning ? _desc : "";
                }
            }
        }

        // Stop the flow immediately as Fail. Callable from pump / external.
        public void Cancel()
        {
            lock (_lock)
            {
                if (!_flowRunning)
                {
                    return;
                }
                _flowRunning = false;
                Failed = true;
                FailReason = "cancelled";
                Monitor.PulseAll(_lock);
            }
        }

        // ----- module-scope helpers used by Task -----
        internal void Describe(string text)
        {
            _desc = text ?? "";
        }

        internal int SubmitAsyncImpl(Func<object?> work, string label, double? timeoutSec)
        {
            Config cfg = _rt.ConfigRef;
            int inflight = 0;
            foreach (var s in _asyncSlots)
            {
                if (!s.Done)
                {
                    inflight++;
                }
            }
            if (cfg.MaxInflightAsync != 0 && inflight >= cfg.MaxInflightAsync)
            {
                _rt.ObserverRef.OnAsyncHighWater(_name, label, inflight);
                return 0;
            }

            var slot = new AsyncSlot
            {
                Id = _nextAsyncId,
                Label = label,
                TimeoutSec = timeoutSec,
                SubmittedAtSec = RealNow(),
            };
            // Run on the pool. Wake the pump the instant the worker finishes so the
            // next poll is not delayed by a full stay nap.
            slot.Task = System.Threading.Tasks.Task.Run<object?>(() => work());
            slot.Task.ContinueWith(_ => _rt.Wake(), TaskScheduler.Default);

            _nextAsyncId += 1;
            _asyncSlots.Add(slot);
            _rt.ObserverRef.OnAsyncSubmitted(_name, _currentName, label);
            return slot.Id;
        }

        internal AsyncOutcome<T> AsyncResultImpl<T>(int asyncId)
        {
            foreach (var s in _asyncSlots)
            {
                if (s.Id != asyncId)
                {
                    continue;
                }
                if (!s.Done)
                {
                    return new AsyncOutcome<T>(AsyncState.Pending);
                }
                if (s.TimedOut)
                {
                    return new AsyncOutcome<T>(AsyncState.TimedOut);
                }
                if (s.Exc != null)
                {
                    return new AsyncOutcome<T>(AsyncState.Failed);
                }
                T val = s.Value is T t ? t : default!;
                return new AsyncOutcome<T>(AsyncState.Done, val);
            }
            return new AsyncOutcome<T>(AsyncState.NotFound);
        }

        internal bool AnyAsyncPendingImpl()
        {
            foreach (var s in _asyncSlots)
            {
                if (!s.Done)
                {
                    return true;
                }
            }
            return false;
        }

        internal void ClearAsyncImpl()
        {
            DropAsyncSlots();
        }

        // Abandon in-flight workers (they keep running into a discarded Task);
        // OnAsyncAbandoned fires per abandoned worker so the leak is visible.
        private void DropAsyncSlots()
        {
            double now = RealNow();
            foreach (var s in _asyncSlots)
            {
                if (!s.Done)
                {
                    double pendingMs = (now - s.SubmittedAtSec) * 1000.0;
                    _rt.ObserverRef.OnAsyncAbandoned(_name, s.Label, pendingMs);
                }
            }
            _asyncSlots = new List<AsyncSlot>();
        }

        // ----- pump-driven async sweep (non-blocking) -----
        private void SweepAsync(Observer observer)
        {
            double now = RealNow();
            foreach (var s in _asyncSlots)
            {
                if (s.Done)
                {
                    continue;
                }
                double elapsed = now - s.SubmittedAtSec;
                bool ready = s.Task.IsCompleted;
                bool timedOut = s.TimeoutSec.HasValue && elapsed >= s.TimeoutSec.Value;
                if (!ready && !timedOut)
                {
                    continue;
                }
                if (timedOut && !ready)
                {
                    // Deadline missed. The worker is not killed - it keeps running
                    // and finishes into its (now ignored) Task.
                    s.Value = null;
                    s.Exc = new TimeoutException($"async timeout: {s.Label}");
                    s.TimedOut = true;
                }
                else
                {
                    if (s.Task.IsFaulted)
                    {
                        s.Value = null;
                        s.Exc = s.Task.Exception?.InnerException ?? s.Task.Exception;
                        s.TimedOut = false;
                    }
                    else
                    {
                        s.Value = s.Task.Result;
                        s.Exc = null;
                        s.TimedOut = false;
                    }
                }
                s.Done = true;
                observer.OnAsyncCompleted(_name, s.Label, elapsed * 1000.0,
                                          s.Exc != null, s.TimedOut);
            }
        }

        // ----- one pump tick. Returns true iff a transition (Next/Done/Fail). ----
        internal bool ExecuteOnce(Observer observer)
        {
            bool fireStarted;
            string first;
            bool running;
            Func<StepResult>? step;
            string prevName;
            lock (_lock)
            {
                if (_flowStartedPending)
                {
                    _flowStartedPending = false;
                    fireStarted = true;
                    first = _currentName;
                }
                else
                {
                    fireStarted = false;
                    first = "";
                }
                running = _flowRunning;
                step = _current;
                prevName = _currentName;
            }
            if (fireStarted)
            {
                observer.OnFlowStarted(_name, first);
            }
            if (!running || step == null)
            {
                return false;
            }

            // 1. non-blocking async sweep
            SweepAsync(observer);

            // 2. run the step (timed)
            _stepTicks += 1;
            StepResult result;
            try
            {
                result = step();
            }
            catch (Exception e)
            {
                observer.OnStepThrew(_name, prevName, e.ToString(), _stepOrdinal, _stepTicks);
                EndFlow(StepAction.Fail, observer, reason: $"step threw: {e.Message}", exc: e);
                return true;
            }

            if (result.Action == StepAction.Stay)
            {
                double nowV = _rt.Clock.Now();
                // A StayUntil wait condition that has stayed true for SettleSec
                // (post-wait / settling) transitions to its success target.
                // Checked before the timeout so a satisfied wait wins if both
                // are ready the same round.
                if (result.Cond != null)
                {
                    if (result.Cond())
                    {
                        if (_settleSinceSec == null)
                        {
                            _settleSinceSec = nowV;
                        }
                        if ((nowV - _settleSinceSec.Value) >= result.SettleSec
                            && result.SuccessFn != null)
                        {
                            Transition(observer, prevName, result.SuccessFn, result.SuccessName);
                            return true;
                        }
                    }
                    else
                    {
                        _settleSinceSec = null;
                    }
                }
                // A StayUntil whose deadline (logical time, from step entry) has
                // passed becomes a transition to its timeout target.
                if (result.TimeoutSec > 0.0 && (nowV - _stepVirtT0Sec) >= result.TimeoutSec)
                {
                    Transition(observer, prevName, result.NextFn!, result.NextName);
                    return true;
                }
                return false;
            }

            if (result.Action == StepAction.Next)
            {
                Transition(observer, prevName, result.NextFn!, result.NextName);
                return true;
            }

            // Done / Fail
            EndFlow(result.Action, observer, reason: result.Reason);
            return true;
        }

        private void Transition(Observer observer, string prevName,
                                Func<StepResult> nextFn, string nextName)
        {
            double elapsedMs = (RealNow() - _stepRealT0Sec) * 1000.0;
            observer.OnStepChanged(_name, prevName, nextName, _desc,
                                   _stepOrdinal, elapsedMs, _stepTicks);
            lock (_lock)
            {
                _current = nextFn;
                _currentName = nextName;
                _desc = "";
                _stepOrdinal += 1;
                _stepRealT0Sec = RealNow();
                _stepVirtT0Sec = _rt.Clock.Now();
                _stepTicks = 0;
            }
            ResetStepTimers();
        }

        private void EndFlow(StepAction action, Observer observer,
                             string reason = "", Exception? exc = null)
        {
            double elapsedMs = (RealNow() - _stepRealT0Sec) * 1000.0;
            string prevName = _currentName;
            observer.OnStepChanged(_name, prevName, "", _desc,
                                   _stepOrdinal, elapsedMs, _stepTicks);
            int finalOrdinal;
            double wallMs;
            lock (_lock)
            {
                _flowRunning = false;
                if (action == StepAction.Fail)
                {
                    Failed = true;
                    FailReason = reason;
                    if (exc != null)
                    {
                        FailExc = exc;
                    }
                }
                wallMs = (RealNow() - _flowT0Sec) * 1000.0;
                finalOrdinal = _stepOrdinal;
                DropAsyncSlots();
                _activeTask = null;
                _unitEntered = null;
                Monitor.PulseAll(_lock);
            }
            observer.OnFlowEnded(_name, action, finalOrdinal, wallMs, reason);
        }
    }

    // ----- Runtime: one pump thread + executor + observer + clock -----
    // Construction spins up the pump thread, the observer (default silent), and
    // the logical clock. The pump drives every attached module round-robin and
    // sweeps completed async ids each round. Dispose() stops the pump thread.
    public sealed class Runtime : IDisposable
    {
        public sealed class Options
        {
            public int Threads { get; set; } = 4;
            public Observer? Observer { get; set; }
            public Config? Config { get; set; }
        }

        public const string Version = Info.Version;
        public static readonly (int Major, int Minor, int Patch) VersionTuple = Info.VersionTuple;

        private readonly List<Module> _objects = new List<Module>();
        private readonly object _objectsMu = new object();
        private readonly List<Action> _posted = new List<Action>();
        private readonly object _postedMu = new object();

        private readonly Observer _observer;
        private readonly Config _config;
        private readonly VirtualClock _clock = new VirtualClock();

        private volatile bool _stop;
        private readonly object _wakeMu = new object();
        private bool _wakeRequested;
        private Action? _preRound;

        private readonly Thread _pump;

        public Runtime() : this(null) { }

        public Runtime(Options? options)
        {
            options ??= new Options();
            _observer = new SafeObserver(options.Observer ?? new ConsoleObserver());
            _config = options.Config ?? new Config();

            _pump = new Thread(PumpLoop) { Name = "uf-pump", IsBackground = true };
            _pump.Start();
        }

        // ----- attach / detach -----
        internal void Attach(Module m)
        {
            lock (_objectsMu)
            {
                _objects.Add(m);
            }
        }

        public void Detach(Module m)
        {
            lock (_objectsMu)
            {
                _objects.Remove(m);
            }
        }

        // ----- accessors -----
        public VirtualClock Clock => _clock;
        internal Observer ObserverRef => _observer;
        internal Config ConfigRef => _config;

        // Run a callback on the pump thread (cross-thread state access without
        // locks). Forwarded onto the pump via the posted-callback queue. Keep it
        // short and non-blocking.
        public void Post(Action fn)
        {
            lock (_postedMu)
            {
                _posted.Add(fn);
            }
            Wake();
        }

        // Force the pump out of its inter-round nap now. Safe from any thread.
        public void Wake()
        {
            lock (_wakeMu)
            {
                _wakeRequested = true;
                Monitor.Pulse(_wakeMu);
            }
        }

        // Hook run once at the top of each round. Exceptions are swallowed.
        public void SetPreRound(Action? fn)
        {
            _preRound = fn;
        }

        private bool DrainPosted()
        {
            List<Action> batch;
            lock (_postedMu)
            {
                if (_posted.Count == 0)
                {
                    return false;
                }
                batch = new List<Action>(_posted);
                _posted.Clear();
            }
            foreach (var fn in batch)
            {
                try { fn(); } catch { }
            }
            return true;
        }

        private void PumpLoop()
        {
            while (!_stop)
            {
                Action? pre = _preRound;
                if (pre != null)
                {
                    try { pre(); } catch { }
                }

                int outcome = 0; // 0 = idle, 1 = staying, 2 = advanced
                if (DrainPosted())
                {
                    outcome = 2;
                }

                List<Module> objs;
                lock (_objectsMu)
                {
                    objs = new List<Module>(_objects);
                }
                foreach (var o in objs)
                {
                    if (o.IsIdle)
                    {
                        continue;
                    }
                    if (outcome == 0)
                    {
                        outcome = 1;
                    }
                    if (o.ExecuteOnce(_observer))
                    {
                        outcome = 2;
                    }
                }

                double nap;
                if (outcome == 2)
                {
                    nap = _config.StepIntervalSleepSec;
                }
                else if (outcome == 1)
                {
                    nap = _config.StaySleepSec;
                }
                else
                {
                    nap = _config.IdleSleepSec;
                }

                if (nap > 0)
                {
                    lock (_wakeMu)
                    {
                        if (!_wakeRequested)
                        {
                            Monitor.Wait(_wakeMu, TimeSpan.FromSeconds(nap));
                        }
                        _wakeRequested = false;
                    }
                }
            }
        }

        // Block the calling thread until every module is idle. Never from the
        // pump thread.
        public bool WaitUntilIdle(double? timeoutSec = null, double poll = 0.005)
        {
            double start = Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency;
            while (true)
            {
                bool busy = false;
                lock (_objectsMu)
                {
                    foreach (var o in _objects)
                    {
                        if (!o.IsIdle)
                        {
                            busy = true;
                            break;
                        }
                    }
                }
                if (!busy)
                {
                    return true;
                }
                double elapsed = Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency - start;
                if (timeoutSec.HasValue && elapsed >= timeoutSec.Value)
                {
                    return false;
                }
                Thread.Sleep(TimeSpan.FromSeconds(poll));
            }
        }

        public void CancelAll()
        {
            List<Module> objs;
            lock (_objectsMu)
            {
                objs = new List<Module>(_objects);
            }
            foreach (var o in objs)
            {
                o.Cancel();
            }
        }

        public void Stop(bool join = true, double timeoutSec = 2.0)
        {
            _stop = true;
            lock (_wakeMu)
            {
                _wakeRequested = true;
                Monitor.Pulse(_wakeMu);
            }
            if (join && _pump.IsAlive)
            {
                _pump.Join(TimeSpan.FromSeconds(timeoutSec));
            }
        }

        public void Dispose()
        {
            Stop();
        }
    }
}
