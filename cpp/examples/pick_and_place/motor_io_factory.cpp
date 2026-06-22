// ======================================================================
//  motor_io_factory.cpp - MotorAxis / DigitalLatch integration and the
//  single factory thread that drives them.
// ======================================================================
#include "motor_io_factory.h"

#include <chrono>
#include <cmath>

using namespace std::chrono_literals;

// ----- MotorAxis -------------------------------------------------------

MotorAxis::MotorAxis(std::string name, double initial_mm,
                     double speed_mm_per_s)
    : name_(std::move(name)),
      pos_mm_(initial_mm),
      target_mm_(initial_mm),
      speed_mm_per_s_(speed_mm_per_s),
      home_mm_(initial_mm)
{
}

void MotorAxis::Home()
{
    std::lock_guard<std::mutex> lk(mu_);
    target_mm_ = home_mm_;
    homing_    = std::fabs(pos_mm_ - home_mm_) > 1e-6;
    moving_    = homing_;
}

void MotorAxis::Move(double target_mm)
{
    std::lock_guard<std::mutex> lk(mu_);
    target_mm_ = target_mm;
    homing_    = false;
    moving_    = std::fabs(pos_mm_ - target_mm_) > 1e-6;
}

double MotorAxis::Position() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return pos_mm_;
}

double MotorAxis::TargetPosition() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return target_mm_;
}

bool MotorAxis::InPosition() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return !moving_;
}

bool MotorAxis::Homing() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return homing_;
}

bool MotorAxis::Moving() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return moving_;
}

bool MotorAxis::Busy() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return moving_ || homing_;
}

bool MotorAxis::Idle() const
{
    return !Busy();
}

void MotorAxis::Update(double dt_s)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!moving_)
    {
        return;
    }
    double remaining = target_mm_ - pos_mm_;
    double step      = speed_mm_per_s_ * dt_s;
    if (std::fabs(remaining) <= step)
    {
        pos_mm_ = target_mm_;
        moving_ = false;
        homing_ = false;
    }
    else
    {
        pos_mm_ += (remaining > 0.0 ? step : -step);
    }
}

// ----- DigitalLatch ----------------------------------------------------

DigitalLatch::DigitalLatch(std::string name, double min_delay_s,
                           double max_delay_s)
    : name_(std::move(name)),
      min_delay_s_(min_delay_s),
      max_delay_s_(max_delay_s)
{
}

void DigitalLatch::Arm()
{
    std::lock_guard<std::mutex> lk(mu_);
    std::uniform_real_distribution<double> d(min_delay_s_, max_delay_s_);
    remaining_s_ = d(rng_);
    armed_       = true;
    ready_       = false;
}

void DigitalLatch::Reset()
{
    std::lock_guard<std::mutex> lk(mu_);
    armed_       = false;
    ready_       = false;
    remaining_s_ = 0.0;
}

bool DigitalLatch::IsReady() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return ready_;
}

void DigitalLatch::Update(double dt_s)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!armed_)
    {
        return;
    }
    remaining_s_ -= dt_s;
    if (remaining_s_ <= 0.0)
    {
        ready_ = true;
        armed_ = false;
    }
}

// ----- MotorIOFactory --------------------------------------------------

MotorIOFactory& MotorIOFactory::inst()
{
    static MotorIOFactory f;
    return f;
}

MotorIOFactory::MotorIOFactory()
{
    thread_ = std::thread([this] { Run(); });
}

MotorIOFactory::~MotorIOFactory()
{
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable())
    {
        thread_.join();
    }
}

MotorAxis* MotorIOFactory::CreateAxis(const std::string& name,
                                      double initial_mm, double speed_mm_per_s)
{
    auto                        axis = std::make_unique<MotorAxis>(
        name, initial_mm, speed_mm_per_s);
    MotorAxis*                  ptr  = axis.get();
    std::lock_guard<std::mutex> lk(mu_);
    devices_.push_back(std::move(axis));
    return ptr;
}

DigitalLatch* MotorIOFactory::CreateLatch(const std::string& name,
                                          double min_delay_s, double max_delay_s)
{
    auto                        latch = std::make_unique<DigitalLatch>(
        name, min_delay_s, max_delay_s);
    DigitalLatch*               ptr   = latch.get();
    std::lock_guard<std::mutex> lk(mu_);
    devices_.push_back(std::move(latch));
    return ptr;
}

void MotorIOFactory::Run()
{
    constexpr auto kTick = 4ms;
    auto           last  = uniflow::Clock::now();
    while (!stop_.load(std::memory_order_relaxed))
    {
        auto   now  = uniflow::Clock::now();
        double dt_s = std::chrono::duration<double>(now - last).count();
        last        = now;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& d : devices_)
            {
                d->Update(dt_s);
            }
        }
        std::this_thread::sleep_for(kTick);
    }
}
