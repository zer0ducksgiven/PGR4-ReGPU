// A gear press lasts one frame in the controls (two bytes, up and down, rewritten every frame), but the gearbox only
// looks in the 60 Hz physics step. On the console's 30 fps every frame had a step; above 60 fps the press is often
// gone before the next one (40 taps: 40 shifts at 60 fps, 31 at 120, 19 at 130). The press is held until a step takes
// it, or for 50 ms (3 steps): one made while the physics is stopped is dropped. Cars: the controls sub_822DC420 (bytes
// 16 and 17 of the control block), the gearbox sub_822DE628. Bikes have their own of both: sub_822F9D70 (bike +3230
// and +3231) and sub_822FB680. PGR4_NO_GEAR_LATCH=1: off. PGR4_TRACE_SHIFT=1: logs each press a gearbox takes.
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_822DC420);
PPC_FUNC_IMPL(__imp__sub_822DE628);
PPC_FUNC_IMPL(__imp__sub_822F9D70);
PPC_FUNC_IMPL(__imp__sub_822FB680);

namespace
{
// A held press: the guest address of its up byte (the down byte follows it).
struct HeldPress
{
    uint32_t bytes = 0;
    uint8_t up = 0, down = 0;
    std::chrono::steady_clock::time_point since;
};
HeldPress g_carHeld[4], g_bikeHeld[4];
std::atomic<int64_t> g_lastPadDrive{ 0 };

LiveSwitch& NoGearLatch()
{
    static LiveSwitch s("PGR4_NO_GEAR_LATCH");
    return s;
}

bool TraceShift()
{
    static const bool trace = getenv("PGR4_TRACE_SHIFT") != nullptr;
    return trace;
}

void NoteDriving()
{
    g_lastPadDrive.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

// After a controls update: a new press is remembered; with none, a held one is put back.
void Latch(HeldPress& held, uint32_t bytes)
{
    if (NoGearLatch())
        return;
    uint8_t* const up = GuestPtr<uint8_t>(bytes);
    uint8_t* const down = up + 1;
    const auto now = std::chrono::steady_clock::now();
    if (*up || *down)
    {
        held = { bytes, *up, *down, now };
        return;
    }
    if (held.bytes != bytes || (!held.up && !held.down))
        return;
    if (now - held.since > std::chrono::milliseconds(50))
    {
        held.up = held.down = 0;
        return;
    }
    *up = held.up;
    *down = held.down;
}

void Taken(HeldPress (&held)[4], uint32_t bytes)
{
    for (HeldPress& h : held)
        if (h.bytes == bytes)
            h.up = h.down = 0;
}
}

// sub_822DC420(control block r3, pad r4)
PPC_FUNC(sub_822DC420)
{
    const uint32_t controls = ctx.r3.u32, pad = ctx.r4.u32 & 0xFF;
    __imp__sub_822DC420(ctx, base);
    if (pad >= 4)
        return;
    NoteDriving();
    Latch(g_carHeld[pad], controls + 16);
}

// sub_822DE628(gearbox r3): +36 the car, whose +808 is its control block; +29 set when a press was already taken since
// the last frame (two steps in one frame), and that step ignores the press.
PPC_FUNC(sub_822DE628)
{
    const uint32_t gearbox = ctx.r3.u32;
    const uint32_t car = LoadBE32(gearbox + 36);
    const uint32_t controls = LoadBE32(car + 808);
    const bool pressed = *GuestPtr<uint8_t>(controls + 16) || *GuestPtr<uint8_t>(controls + 17);
    const bool taken = pressed && !*GuestPtr<uint8_t>(gearbox + 29);
    const int gear = *GuestPtr<int8_t>(gearbox + 30);
    __imp__sub_822DE628(ctx, base);
    if (!taken)
        return;
    if (TraceShift())
        Log("gear: car press taken, gear %d -> %d", gear, *GuestPtr<int8_t>(gearbox + 30));
    Taken(g_carHeld, controls + 16);
}

// A bike's controls (bike r3): its own update, which never reaches sub_822DC420. A pad index of -1 is an AI rider.
PPC_FUNC(sub_822F9D70)
{
    const uint32_t bike = ctx.r3.u32;
    const int32_t pad = int32_t(LoadBE32(bike + 3904));
    const bool player = pad >= 0 && pad < 4;
    if (player && LoadBE32(bike + 20) == 0 && (ctx.r4.u32 & 0xFF) != 0)
        NoteDriving();
    __imp__sub_822F9D70(ctx, base);
    if (player)
        Latch(g_bikeHeld[pad], bike + 3230);
}

// sub_822FB680(gearbox r3): +0 the bike, +12 the gear; a press at bike +3230 / +3231 moves it up / down.
PPC_FUNC(sub_822FB680)
{
    const uint32_t gearbox = ctx.r3.u32;
    const uint32_t bike = LoadBE32(gearbox);
    const bool pressed = *GuestPtr<uint8_t>(bike + 3230) || *GuestPtr<uint8_t>(bike + 3231);
    const int gear = int32_t(LoadBE32(gearbox + 12));
    __imp__sub_822FB680(ctx, base);
    if (!pressed)
        return;
    if (TraceShift())
        Log("gear: bike press taken, gear %d -> %d", gear, int32_t(LoadBE32(gearbox + 12)));
    Taken(g_bikeHeld, bike + 3230);
}

// The garage's walking camera moves by the stick too (garage_walk.cpp).
void NoteGarageWalk()
{
    NoteDriving();
}

// Whether a player's pad is driving a vehicle (a race) or walking a garage: its controls update every frame then. The
// menus' background cars are not driven by a pad.
bool CarsDriving()
{
    const auto last = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(g_lastPadDrive.load(std::memory_order_relaxed)));
    return std::chrono::steady_clock::now() - last < std::chrono::milliseconds(200);
}
