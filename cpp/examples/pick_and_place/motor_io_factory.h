// motor_io_factory.h - hardware abstraction for the demo.
//
// MotorAxis is a 1D linear axis that integrates its own position toward a
// commanded target. A flow does NOT step the motor; it only issues commands
// (Move / Home) and polls state (InPosition / Position). The integration is
// done by ONE background thread owned by the MotorIOFactory singleton, which
// calls Update() on every registered axis and IO object. This mirrors a real
// cell: the controller owns the servo loop, the application code just commands
// and reads.
//
// A flow holds raw MotorAxis* / DigitalLatch* pointers to the few devices it
// uses, handed out by the factory. The factory owns the objects (and keeps
// them alive for the process), the flow borrows them.
#pragma once

#include "uniflow.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Everything the factory thread services implements this. Update(dt_s) is
// called only on the factory thread; all public read/command methods are
// callable from any thread (each device guards its own state).
class IDevice
{
public:
    virtual ~IDevice() = default;
    virtual void Update(double dt_s) = 0;
};

// 1D linear axis. No tolerance / settle window (this is a simulator, not a real
// position loop) - InPosition() is simply "the commanded move has finished".
// Speed is a property of the axis; Move() does not take one.
class MotorAxis : public IDevice
{
public:
    MotorAxis(std::string name, double initial_mm, double speed_mm_per_s);

    // -- Commands --
    void Home();                  // move to the home position
    void Move(double target_mm);  // command an absolute move

    // -- State (any thread) --
    double Position() const;
    double TargetPosition() const;
    bool   InPosition() const;    // commanded move finished
    bool   Homing() const;
    bool   Moving() const;
    bool   Busy() const;          // homing || moving
    bool   Idle() const;          // !Busy

    const std::string& Name() const { return name_; }

    // -- Called only by the factory thread --
    void Update(double dt_s) override;

private:
    mutable std::mutex mu_;
    std::string        name_;
    double             pos_mm_;
    double             target_mm_;
    double             speed_mm_per_s_;
    double             home_mm_ = 0.0;
    bool               moving_  = false;
    bool               homing_  = false;
};

// A digital input that latches true a while after it is armed - the demo's
// model for a hardware handshake (e.g. "machining head ready"). The delay is
// counted down by the factory thread, so there is no per-handshake detached
// thread. Reset() drops it back to false.
class DigitalLatch : public IDevice
{
public:
    DigitalLatch(std::string name, double min_delay_s, double max_delay_s);

    void Arm();    // start the delay; becomes ready when it elapses
    void Reset();  // back to not-ready, disarmed
    bool IsReady() const;

    const std::string& Name() const { return name_; }

    void Update(double dt_s) override;

private:
    mutable std::mutex mu_;
    std::string        name_;
    double             min_delay_s_;
    double             max_delay_s_;
    double             remaining_s_ = 0.0;
    bool               armed_       = false;
    bool               ready_       = false;
    std::mt19937       rng_{std::random_device{}()};
};

// Singleton owning every motor axis and IO device, plus the single thread that
// integrates them. Devices are created through here so the factory both owns
// their lifetime and services them. Flows keep the returned pointer.
class MotorIOFactory
{
public:
    static MotorIOFactory& inst();

    MotorAxis*    CreateAxis(const std::string& name, double initial_mm,
                             double speed_mm_per_s);
    DigitalLatch* CreateLatch(const std::string& name, double min_delay_s,
                              double max_delay_s);

    MotorIOFactory(const MotorIOFactory&)            = delete;
    MotorIOFactory& operator=(const MotorIOFactory&) = delete;

private:
    MotorIOFactory();
    ~MotorIOFactory();

    void Run();  // factory thread: integrate every device each tick

    std::mutex                            mu_;
    std::vector<std::unique_ptr<IDevice>> devices_;
    std::atomic<bool>                     stop_{false};
    std::thread                           thread_;
};
