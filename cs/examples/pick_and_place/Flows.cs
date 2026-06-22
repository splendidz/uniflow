// Flows.cs - the pick-and-place line's modules. C# port of the Python port's
// Flow_LoadPicker / Flow_UnloadPicker / Flow_Stage / Flow_Orchestrator /
// Flow_Visualization (cpp/examples/pick_and_place).
//
// FEATURE FOCUS:
//   - orchestrator + state polling: one perpetual Schedule task launches each
//     module's next task when that module IsIdle, chosen from plain member reads.
//   - lock-free B-zone handoff on ONE pump: the two pickers must NEVER both sit
//     in zone B. Solved purely by reading peer members (InsideZoneB / Carrying);
//     no locks, because every module advances on the same pump thread round-robin.
//   - async-poll command acks: the Stage's start/cleanup commands are SubmitAsync
//     workers, polled by AsyncId with a StayUntil timeout catch - the pump never
//     blocks on them.
//   - multi-task module: one module (the Stage) holds three tasks and the
//     orchestrator runs them one at a time.

#nullable enable

using System;
using Uniflow;

namespace Uniflow.PickAndPlaceExample
{
    // ========================================================================
    // Flow_LoadPicker - carries a raw part A -> B. Two tasks: Pick (A) -> Place (B)
    // ========================================================================
    internal sealed class Flow_LoadPicker : Module
    {
        public readonly DeviceClock Dev = new DeviceClock();
        public readonly MotorAxis X;
        public readonly MotorAxis Z;
        public readonly MotorAxis Finger;
        public bool Carrying;

        public readonly Task_Pick CtxPick;
        public readonly Task_Place CtxPlace;

        public Flow_LoadPicker(Runtime rt)
            : base(rt, "Flow_LoadPicker")
        {
            X = Dev.Add(new MotorAxis("load_x", Geometry.ZONE_A_MM, Geometry.X_SPEED_MM_PER_S));
            Z = Dev.Add(new MotorAxis("load_z", Geometry.Z_UP_MM, Geometry.Z_SPEED_MM_PER_S));
            Finger = Dev.Add(new MotorAxis("load_finger", Geometry.FINGER_OPEN_MM, Geometry.FINGER_SPEED_MM_PER_S));
            Carrying = false;

            CtxPick = new Task_Pick();
            AddTask(CtxPick);
            CtxPlace = new Task_Place();
            AddTask(CtxPlace);
        }

        // -- Motion state read by the unload picker's B-zone gate and the snapshot
        public double XMm() => X.Position();
        public double ZMm() => Z.Position();
        public bool CarryingFlag() => Carrying;
        public bool InsideZoneB() => Geometry.InsideZoneB(XMm());
        public bool PartnerInZoneB() => App.Inst().Unload.InsideZoneB();

        // ----- Task: Pick (zone A) -----
        public sealed class Task_Pick : Task<Flow_LoadPicker>
        {
            protected override StepResult Entry() => Step1_CmdMoveToSource();

            private StepResult Step1_CmdMoveToSource()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: move to zone A");
                Flow.X.Move(Geometry.ZONE_A_MM);
                return Next(Step2_WaitAtSource);
            }

            private StepResult Step2_WaitAtSource()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("approaching A");
                if (Flow.X.InPosition()) return Next(Step3_CmdLowerToPick);
                return Stay();
            }

            private StepResult Step3_CmdLowerToPick()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: lower to pick");
                Flow.Z.Move(Geometry.Z_DOWN_MM);
                return Next(Step4_WaitAtPickDown);
            }

            private StepResult Step4_WaitAtPickDown()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("lowering to pick");
                if (Flow.Z.InPosition()) return Next(Step5_HandGrip);
                return Stay();
            }

            private StepResult Step5_HandGrip()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("closing gripper");
                Flow.Finger.Move(0.0);
                if (!Flow.Finger.InPosition()) return Stay();
                Env.ConsumeZoneAPart();
                Flow.Carrying = true;
                return Next(Step6_CmdLiftWithPart);
            }

            private StepResult Step6_CmdLiftWithPart()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: lift with part");
                Flow.Z.Move(Geometry.Z_UP_MM);
                return Next(Step7_WaitAtPickUp);
            }

            private StepResult Step7_WaitAtPickUp()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("lifting with part");
                // Pick done: part is up and carried. The flow goes idle; the
                // orchestrator launches Place next (it sees Carrying).
                if (Flow.Z.InPosition()) return Done();
                return Stay();
            }
        }

        // ----- Task: Place (zone B) - gated by stage readiness + partner -----
        public sealed class Task_Place : Task<Flow_LoadPicker>
        {
            protected override StepResult Entry() => Step1_CmdMoveToDest();

            private StepResult Step1_CmdMoveToDest()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Flow_Stage stage = App.Inst().Stage;
                // lock-free B-zone handoff: read the stage's readiness and the
                // partner's position - plain member reads on the one pump thread.
                bool mayEnterB = stage.ReadyToReceiveRawPart() && !Flow.PartnerInZoneB();
                if (!Flow.InsideZoneB() && !mayEnterB)
                {
                    Flow.X.Move(Geometry.ZONE_B_MM - Geometry.B_SAFETY_GAP_MM);
                    if (!Flow.X.InPosition())
                    {
                        Describe("moving to A-gap");
                        return Stay();
                    }
                    Describe("parked at A-gap: stage=" + stage.State()
                             + " partner_in_B=" + Flow.PartnerInZoneB());
                    return Stay();
                }
                Describe("cmd: move to zone B");
                Flow.X.Move(Geometry.ZONE_B_MM);
                return Next(Step2_WaitAtDest);
            }

            private StepResult Step2_WaitAtDest()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("approaching B");
                if (Flow.X.InPosition()) return Next(Step3_CmdLowerToPlace);
                return Stay();
            }

            private StepResult Step3_CmdLowerToPlace()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: lower to place");
                Flow.Z.Move(Geometry.Z_DOWN_MM);
                return Next(Step4_WaitAtPlaceDown);
            }

            private StepResult Step4_WaitAtPlaceDown()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("lowering to place");
                if (Flow.Z.InPosition()) return Next(Step5_HandRelease);
                return Stay();
            }

            private StepResult Step5_HandRelease()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("opening gripper");
                Flow.Finger.Move(Geometry.FINGER_OPEN_MM);
                if (!Flow.Finger.InPosition()) return Stay();
                App.Inst().Stage.OnRawPartReceived();
                Flow.Carrying = false;
                return Next(Step6_CmdLiftEmpty);
            }

            private StepResult Step6_CmdLiftEmpty()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: lift empty");
                Flow.Z.Move(Geometry.Z_UP_MM);
                return Next(Step7_WaitAtPlaceUp);
            }

            private StepResult Step7_WaitAtPlaceUp()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("lifting empty");
                if (Flow.Z.InPosition()) return Next(Step8_CmdRetreat);
                return Stay();
            }

            private StepResult Step8_CmdRetreat()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: retreat to A");
                Flow.X.Move(Geometry.ZONE_A_MM);
                return Next(Step9_WaitAtRetreat);
            }

            private StepResult Step9_WaitAtRetreat()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("retreating");
                if (Flow.X.InPosition())
                {
                    Describe("flow done");
                    return Done();
                }
                return Stay();
            }
        }
    }

    // ========================================================================
    // Flow_UnloadPicker - carries the finished part B -> C. Same shape, but the
    // SOURCE is the contested B zone (Pick at B -> Place at C).
    // ========================================================================
    internal sealed class Flow_UnloadPicker : Module
    {
        public readonly DeviceClock Dev = new DeviceClock();
        public readonly MotorAxis X;
        public readonly MotorAxis Z;
        public readonly MotorAxis Finger;
        public bool Carrying;

        public readonly Task_Pick CtxPick;
        public readonly Task_Place CtxPlace;

        public Flow_UnloadPicker(Runtime rt)
            : base(rt, "Flow_UnloadPicker")
        {
            X = Dev.Add(new MotorAxis("unload_x", Geometry.ZONE_C_MM, Geometry.X_SPEED_MM_PER_S));
            Z = Dev.Add(new MotorAxis("unload_z", Geometry.Z_UP_MM, Geometry.Z_SPEED_MM_PER_S));
            Finger = Dev.Add(new MotorAxis("unload_finger", Geometry.FINGER_OPEN_MM, Geometry.FINGER_SPEED_MM_PER_S));
            Carrying = false;

            CtxPick = new Task_Pick();
            AddTask(CtxPick);
            CtxPlace = new Task_Place();
            AddTask(CtxPlace);
        }

        public double XMm() => X.Position();
        public double ZMm() => Z.Position();
        public bool CarryingFlag() => Carrying;
        public bool InsideZoneB() => Geometry.InsideZoneB(XMm());
        public bool PartnerInZoneB() => App.Inst().Load.InsideZoneB();

        // ----- Task: Pick (zone B, contested) -----
        public sealed class Task_Pick : Task<Flow_UnloadPicker>
        {
            protected override StepResult Entry() => Step1_CmdMoveToSource();

            private StepResult Step1_CmdMoveToSource()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Flow_LoadPicker load = App.Inst().Load;
                string st = App.Inst().Stage.State();
                bool stagePastLoading = st == StageState.PREPARED || st == StageState.MACHINED
                                        || st == StageState.PROCESSED_PART_READY;
                // load.Carrying catches "approaching B with a part"; InsideZoneB
                // catches "still lifting/retreating in B after the handoff".
                bool loadThreatensB = load.CarryingFlag() || load.InsideZoneB();
                bool mayEnterB = stagePastLoading && !loadThreatensB;
                if (!Flow.InsideZoneB() && !mayEnterB)
                {
                    Flow.X.Move(Geometry.ZONE_B_MM + Geometry.B_SAFETY_GAP_MM);
                    if (!Flow.X.InPosition())
                    {
                        Describe("moving to C-gap");
                        return Stay();
                    }
                    Describe("parked at C-gap: stage=" + st
                             + " load_carry=" + load.CarryingFlag()
                             + " load_in_B=" + load.InsideZoneB());
                    return Stay();
                }
                Describe("cmd: move to zone B");
                Flow.X.Move(Geometry.ZONE_B_MM);
                return Next(Step2_WaitAtSource);
            }

            private StepResult Step2_WaitAtSource()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("approaching B");
                if (Flow.X.InPosition()) return Next(Step3_CmdLowerToPick);
                return Stay();
            }

            private StepResult Step3_CmdLowerToPick()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                if (!App.Inst().Stage.ReadyToHandOffProcessedPart())
                {
                    Describe("hovering above B: stage=" + App.Inst().Stage.State());
                    return Stay();
                }
                Describe("cmd: lower to pick");
                Flow.Z.Move(Geometry.Z_DOWN_MM);
                return Next(Step4_WaitAtPickDown);
            }

            private StepResult Step4_WaitAtPickDown()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("lowering to pick");
                if (Flow.Z.InPosition()) return Next(Step5_HandGrip);
                return Stay();
            }

            private StepResult Step5_HandGrip()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("closing gripper");
                Flow.Finger.Move(0.0);
                if (!Flow.Finger.InPosition()) return Stay();
                App.Inst().Stage.OnProcessedPartTaken();
                Flow.Carrying = true;
                return Next(Step6_CmdLiftWithPart);
            }

            private StepResult Step6_CmdLiftWithPart()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: lift with part");
                Flow.Z.Move(Geometry.Z_UP_MM);
                return Next(Step7_WaitAtPickUp);
            }

            private StepResult Step7_WaitAtPickUp()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("lifting with part");
                if (Flow.Z.InPosition()) return Done();
                return Stay();
            }
        }

        // ----- Task: Place (zone C) -----
        public sealed class Task_Place : Task<Flow_UnloadPicker>
        {
            protected override StepResult Entry() => Step1_CmdMoveToUnload();

            private StepResult Step1_CmdMoveToUnload()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: move to zone C");
                Flow.X.Move(Geometry.ZONE_C_MM);
                return Next(Step2_WaitAtUnload);
            }

            private StepResult Step2_WaitAtUnload()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("approaching C");
                if (Flow.X.InPosition()) return Next(Step3_CmdLowerToPlace);
                return Stay();
            }

            private StepResult Step3_CmdLowerToPlace()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: lower to place");
                Flow.Z.Move(Geometry.Z_DOWN_MM);
                return Next(Step4_WaitAtPlaceDown);
            }

            private StepResult Step4_WaitAtPlaceDown()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("lowering to place");
                if (Flow.Z.InPosition()) return Next(Step5_HandRelease);
                return Stay();
            }

            private StepResult Step5_HandRelease()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("opening gripper");
                Flow.Finger.Move(Geometry.FINGER_OPEN_MM);
                if (!Flow.Finger.InPosition()) return Stay();
                Env.IncDelivered();
                Flow.Carrying = false;
                return Next(Step6_CmdLiftEmpty);
            }

            private StepResult Step6_CmdLiftEmpty()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: lift empty");
                Flow.Z.Move(Geometry.Z_UP_MM);
                return Next(Step7_WaitAtPlaceUp);
            }

            private StepResult Step7_WaitAtPlaceUp()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("lifting empty");
                if (Flow.Z.InPosition()) return Next(Step8_CmdRetreat);
                return Stay();
            }

            private StepResult Step8_CmdRetreat()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("cmd: retreat to C");
                Flow.X.Move(Geometry.ZONE_C_MM);
                return Next(Step9_WaitAtRetreat);
            }

            private StepResult Step9_WaitAtRetreat()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("retreating");
                if (Flow.X.InPosition())
                {
                    Describe("flow done");
                    return Done();
                }
                return Stay();
            }
        }
    }

    // ========================================================================
    // Flow_Stage - machining cell at zone B. A multi-task module: one flow per
    // part, three unit operations - Prepare -> Process -> Cleanup - by state.
    // ========================================================================
    internal sealed class Flow_Stage : Module
    {
        public const double PROCESS_DURATION_S = 5.0;
        public const double TABLE_SPEED_MM_PER_S = 5000.0;  // near direct position control

        public readonly DeviceClock Dev = new DeviceClock();
        public readonly MotorAxis TableX;
        public readonly DigitalLatch HwReady;
        public double TableYOffsetMm;
        public string StateField = StageState.IDLE;

        public readonly Task_Prepare CtxPrepare;
        public readonly Task_Process CtxProcess;
        public readonly Task_Cleanup CtxCleanup;

        public Flow_Stage(Runtime rt)
            : base(rt, "Flow_Stage")
        {
            TableX = Dev.Add(new MotorAxis("stage_table_x", Geometry.ZONE_B_MM, TABLE_SPEED_MM_PER_S));
            HwReady = Dev.Add(new DigitalLatch("stage_hw_ready", 0.2, 0.7));
            TableYOffsetMm = 0.0;
            StateField = StageState.IDLE;

            CtxPrepare = new Task_Prepare();
            AddTask(CtxPrepare);
            CtxProcess = new Task_Process();
            AddTask(CtxProcess);
            CtxCleanup = new Task_Cleanup();
            AddTask(CtxCleanup);
        }

        public string State() => StateField;
        public double TableXMm() => TableX.Position();
        public double TableYMm() => TableYOffsetMm;
        public bool ReadyToReceiveRawPart() => StateField == StageState.IDLE;
        public bool ReadyToHandOffProcessedPart() => StateField == StageState.PROCESSED_PART_READY;

        public void OnRawPartReceived()
        {
            StateField = StageState.RAW_PART_LOADED;
            Describe("raw part loaded");
        }

        public void OnProcessedPartTaken()
        {
            StateField = StageState.IDLE;
            Describe("empty");
        }

        // ----- Task: Prepare - start cmd (async), ack, then wait HW ready -----
        public sealed class Task_Prepare : Task<Flow_Stage>
        {
            private UFTimer _settle = null!;

            // re-arm the hw-ready settle timer on the runtime clock.
            protected override void OnEnter()
            {
                _settle = new UFTimer(Flow.Clock);
            }

            protected override StepResult Entry() => Step1_SendStart();

            private static object? SimulateStartCmd()
            {
                System.Threading.Thread.Sleep(300);
                return true;
            }

            private StepResult Step1_SendStart()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                // Guarded bootstrap: only a freshly loaded raw part may begin.
                if (Flow.StateField != StageState.RAW_PART_LOADED) return Fail();
                Describe("send start cmd");
                // async-poll command ack: submit the worker, carry its id to the
                // poller; id 0 means rejected (in-flight cap).
                int cmd = SubmitAsync(SimulateStartCmd, "start_cmd");
                if (cmd == 0)
                {
                    Describe("start cmd rejected");
                    return Fail();
                }
                _pendingCmd = cmd;
                return Next(Step2_WaitStartAck);
            }

            private int _pendingCmd;

            private StepResult Step2_WaitStartAck()
            {
                Flow.Dev.Tick();
                AsyncOutcome<bool> r = AsyncResult<bool>(_pendingCmd);
                if (r.Pending)
                {
                    Describe("wait start ack");
                    return StayUntil(2.0, Step_StartAckTimeout);
                }
                if (!r.Ok || !r.ReturnValue)
                {
                    Describe("start cmd failed");
                    return Fail();
                }
                Describe("wait hw ready");
                Flow.HwReady.Arm();
                return Next(Step3_WaitHwReady);
            }

            private StepResult Step_StartAckTimeout()
            {
                Describe("start ack timeout");
                ClearAsync();
                return Fail();
            }

            private StepResult Step3_WaitHwReady()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                // HeldFor proceeds once ready has held STABLE for 50ms (settling),
                // not on the first transient high. settle was re-armed in OnEnter.
                if (_settle.HeldFor(Flow.HwReady.IsReady(), 0.05))
                {
                    Flow.StateField = StageState.PREPARED;
                    Describe("prepared");
                    return Done();
                }
                Describe("wait hw ready");
                return StayUntil(3.0, Step4_HwTimeout);
            }

            private StepResult Step4_HwTimeout()
            {
                Describe("hw ready timeout");
                return Fail();
            }
        }

        // ----- Task: Process - figure-8 (Gerono lemniscate) machining run -----
        public sealed class Task_Process : Task<Flow_Stage>
        {
            private UFTimer _run = null!;

            protected override void OnEnter()
            {
                _run = new UFTimer(Flow.Clock);
            }

            protected override StepResult Entry() => Step1_Process();

            private StepResult Step1_Process()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                double elapsed = _run.Elapsed();
                double frac = Math.Min(1.0, elapsed / Flow_Stage.PROCESS_DURATION_S);
                // Figure-8: x = sin(t), y = sin(2t). The 2:1 ratio makes the '8'
                // cross at zone centre; four loops over the process duration.
                const double tau = 6.2831853071795864;
                const int loops = 4;
                const double ampY = 30.0;
                double phase = frac * tau * loops;
                double sweepMm = Geometry.STAGE_TRAVEL_MM * Math.Sin(phase);
                Flow.TableX.Move(Geometry.ZONE_B_MM + sweepMm);
                Flow.TableYOffsetMm = ampY * Math.Sin(phase * 2.0);
                if (elapsed >= Flow_Stage.PROCESS_DURATION_S)
                {
                    Flow.StateField = StageState.MACHINED;
                    Describe("machined");
                    return Done();
                }
                Describe("processing");
                return Stay();
            }
        }

        // ----- Task: Cleanup - cleanup cmd (async), ack, return to pick pos -----
        public sealed class Task_Cleanup : Task<Flow_Stage>
        {
            private int _pendingCmd;

            protected override StepResult Entry() => Step1_SendCleanup();

            private static object? SimulateCleanupCmd()
            {
                System.Threading.Thread.Sleep(200);
                return true;
            }

            private StepResult Step1_SendCleanup()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Describe("send cleanup cmd");
                int cmd = SubmitAsync(SimulateCleanupCmd, "cleanup_cmd");
                if (cmd == 0)
                {
                    Describe("cleanup cmd rejected");
                    return Fail();
                }
                _pendingCmd = cmd;
                return Next(Step2_WaitCleanupAck);
            }

            private StepResult Step2_WaitCleanupAck()
            {
                Flow.Dev.Tick();
                AsyncOutcome<bool> r = AsyncResult<bool>(_pendingCmd);
                if (r.Pending)
                {
                    Describe("wait cleanup ack");
                    return StayUntil(2.0, Step_CleanupAckTimeout);
                }
                if (!r.Ok || !r.ReturnValue)
                {
                    Describe("cleanup failed");
                    return Fail();
                }
                Describe("return to pick pos");
                Flow.TableX.Move(Geometry.ZONE_B_MM);
                return Next(Step3_ReturnToPickPos);
            }

            private StepResult Step_CleanupAckTimeout()
            {
                Describe("cleanup ack timeout");
                ClearAsync();
                return Fail();
            }

            private StepResult Step3_ReturnToPickPos()
            {
                Flow.Dev.Tick();
                if (Env.Stop()) return Done();
                Flow.TableYOffsetMm = 0.0;
                if (!Flow.TableX.InPosition()) return Stay();
                Flow.StateField = StageState.PROCESSED_PART_READY;
                Describe("ready to hand off");
                return Done();
            }
        }
    }

    // ========================================================================
    // Flow_Orchestrator - line-level scheduler. One perpetual Schedule task whose
    // single step polls the line every pump round and launches each module's next
    // task when it IsIdle. The pickers / stage never sequence themselves.
    // ========================================================================
    internal sealed class Flow_Orchestrator : Module
    {
        public readonly Task_Schedule CtxSchedule;

        public Flow_Orchestrator(Runtime rt)
            : base(rt, "Flow_Orchestrator")
        {
            CtxSchedule = new Task_Schedule();
            AddTask(CtxSchedule);
        }

        public sealed class Task_Schedule : Task<Flow_Orchestrator>
        {
            protected override StepResult Entry() => Step1_Tick();

            private StepResult Step1_Tick()
            {
                if (Env.Stop()) return Done();
                TryCreateRawPart();
                TryDriveLoadPicker();
                TryDriveStage();
                TryDriveUnloadPicker();
                return Stay();
            }

            private void TryCreateRawPart()
            {
                // A fresh raw part is staged the instant zone A is empty.
                if (Env.ZoneAHasPart()) return;
                Env.CreateFakeZoneAPart();
            }

            private void TryDriveLoadPicker()
            {
                Flow_LoadPicker picker = App.Inst().Load;
                if (!picker.IsIdle) return;
                // Carrying -> deliver (Place); else grab the next one (Pick).
                if (picker.CarryingFlag())
                {
                    picker.CtxPlace.StartFlow();
                }
                else if (Env.ZoneAHasPart())
                {
                    picker.CtxPick.StartFlow();
                }
            }

            private void TryDriveStage()
            {
                Flow_Stage stage = App.Inst().Stage;
                if (!stage.IsIdle) return;
                // One task per machining phase, launched as the previous completes.
                string st = stage.State();
                if (st == StageState.RAW_PART_LOADED)
                {
                    stage.CtxPrepare.StartFlow();
                }
                else if (st == StageState.PREPARED)
                {
                    stage.CtxProcess.StartFlow();
                }
                else if (st == StageState.MACHINED)
                {
                    stage.CtxCleanup.StartFlow();
                }
            }

            private void TryDriveUnloadPicker()
            {
                Flow_UnloadPicker picker = App.Inst().Unload;
                if (!picker.IsIdle) return;
                if (picker.CarryingFlag())
                {
                    picker.CtxPlace.StartFlow();
                    return;
                }
                // Prefetch Pick as soon as a part is incoming, so the picker is
                // already hovering above B when the stage finishes.
                string st = App.Inst().Stage.State();
                bool stageHasPartIncoming = st == StageState.PREPARED || st == StageState.MACHINED
                                            || st == StageState.PROCESSED_PART_READY;
                if (stageHasPartIncoming)
                {
                    picker.CtxPick.StartFlow();
                }
            }
        }
    }

    // ========================================================================
    // Flow_Visualization - pump-side snapshot writer. One perpetual Snapshot task
    // copies live line state into the shared snapshot every round (uf_visualization).
    // ========================================================================
    internal sealed class Flow_Visualization : Module
    {
        public readonly Task_Snapshot CtxSnapshot;

        public Flow_Visualization(Runtime rt)
            : base(rt, "Flow_Visualization")
        {
            CtxSnapshot = new Task_Snapshot();
            AddTask(CtxSnapshot);
        }

        public sealed class Task_Snapshot : Task<Flow_Visualization>
        {
            protected override StepResult Entry() => Step1_Tick();

            private StepResult Step1_Tick()
            {
                if (Env.Stop()) return Done();
                App app = App.Inst();
                Flow_LoadPicker load = app.Load;
                Flow_UnloadPicker unload = app.Unload;
                Flow_Stage stage = app.Stage;
                Snapshot g = SnapshotStore.Snap;
                lock (SnapshotStore.Mu)
                {
                    g.LoadXMm = load.XMm();
                    g.LoadZMm = load.ZMm();
                    g.LoadCarry = load.CarryingFlag();
                    g.LoadPhase = load.CurrentStepDescription;
                    g.UnloadXMm = unload.XMm();
                    g.UnloadZMm = unload.ZMm();
                    g.UnloadCarry = unload.CarryingFlag();
                    g.UnloadPhase = unload.CurrentStepDescription;
                    g.StageTableXMm = stage.TableXMm();
                    g.StageTableYMm = stage.TableYMm();
                    g.StageState = stage.State();
                    g.StagePhase = stage.CurrentStepDescription;
                    g.ZoneAHasPart = Env.ZoneAHasPart();
                    g.Delivered = Env.DeliveredCount();
                }
                return Stay();
            }
        }
    }
}
