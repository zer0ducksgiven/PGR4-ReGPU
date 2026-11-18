#include "kernel/thread.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <deque>
#include <array>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include "live_switch.h"
#include "log.h"
#include "settings.h"
#include <cstring>

namespace
{
uint32_t g_tlsTemplate = 0;
uint32_t g_tlsDataSize = 0;
uint32_t g_tlsSlotCount = 64;
std::atomic<uint32_t> g_nextThreadId{ 1 };
thread_local GuestThread* t_thread = nullptr;

std::mutex g_threadsMutex;
std::unordered_map<uint32_t, std::weak_ptr<ThreadObject>> g_threadsByKthread;

std::mutex g_tlsMutex;
std::vector<bool> g_tlsUsed;

// Host stack for recompiled code: frames are big and call chains deep.
constexpr size_t kHostStackSize = 16 * 1024 * 1024;

struct ExitThreadSignal
{
    uint32_t code;
};

std::shared_ptr<GuestThread> SetUpGuestThread(uint32_t stackSize, const char* name)
{
    auto thread = std::make_shared<GuestThread>();
    thread->id = g_nextThreadId++;
    thread->name = name ? name : "";

    stackSize = std::max<uint32_t>((stackSize + 0xFFF) & ~0xFFFu, 0x40000);
    thread->stackLow = GuestAlloc(Heap::Virtual, stackSize, 0x1000);
    thread->stackHigh = thread->stackLow + stackSize;

    // 64 KB aligned: titles disable interrupts with "mtmsrd r13,1", relying
    // on the PCR address having MSR[EE] (0x8000) clear.
    thread->pcr = GuestAlloc(Heap::Virtual, kpcr::Size, 0x10000);
    thread->kthread = GuestAlloc(Heap::Virtual, kthread::Size, 0x100);
    // Static TLS: the .tls template, then the dynamic slots.
    thread->tls = GuestAlloc(Heap::Virtual, g_tlsDataSize + g_tlsSlotCount * 4 + 16);
    if (g_tlsDataSize)
        memcpy(GuestPtr(thread->tls), GuestPtr(g_tlsTemplate), g_tlsDataSize);

    StoreBE32(thread->pcr + kpcr::TlsPtr, thread->tls);
    StoreBE32(thread->pcr + kpcr::PcrPtr, thread->pcr);
    StoreBE32(thread->pcr + kpcr::StackBase, thread->stackHigh);
    StoreBE32(thread->pcr + kpcr::StackEnd, thread->stackLow);
    StoreBE32(thread->pcr + kpcr::CurrentThread, thread->kthread);
    *GuestPtr<uint8_t>(thread->pcr + kpcr::CurrentCpu) = uint8_t(thread->id % 6);

    *GuestPtr<uint8_t>(thread->kthread) = 6; // dispatcher type: thread
    StoreBE32(thread->kthread + kthread::StackBase, thread->stackHigh);
    StoreBE32(thread->kthread + kthread::StackLimit, thread->stackLow);
    StoreBE32(thread->kthread + kthread::TlsAddress, thread->tls);
    StoreBE32(thread->kthread + kthread::ThreadId, thread->id);
    thread->tlsSlots.assign(g_tlsSlotCount, 0);
    return thread;
}

DWORD WINAPI HostThreadMain(void* param)
{
    auto* object = static_cast<ThreadObject*>(param);
    std::shared_ptr<GuestThread> thread = object->thread;
    // Keep the object alive until the thread is done with it.
    std::shared_ptr<ThreadObject> self = std::static_pointer_cast<ThreadObject>(GetHandle(object->handle));
    t_thread = thread.get();

    {
        std::unique_lock lock(thread->suspendMutex);
        thread->suspendCv.wait(lock, [&] { return thread->suspendCount <= 0; });
    }

    uint32_t exitCode = 0;
    try
    {
        PPCContext ctx{};
        InitGuestContext(ctx, *thread);
        if (thread->hostEntry)
            thread->hostEntry(ctx);
        else if (thread->xapiStartup)
        {
            ctx.r3.u64 = thread->entry;
            ctx.r4.u64 = thread->param;
            GuestCall(ctx, thread->xapiStartup);
        }
        else
        {
            ctx.r3.u64 = thread->param;
            GuestCall(ctx, thread->entry);
        }
        exitCode = ctx.r3.u32;
    }
    catch (const ExitThreadSignal& exit)
    {
        exitCode = exit.code;
    }

    {
        std::lock_guard lock(g_objectMutex);
        self->exited = true;
        self->exitCode = exitCode;
        if (self->guestObject)
            StoreBE32(self->guestObject + 4, 1);
    }
    SignalObjectsChanged();
    thread->ctx = nullptr;
    return 0;
}
}

void ThreadSystemInit(uint32_t tlsTemplate, uint32_t tlsDataSize, uint32_t tlsSlotCount)
{
    g_tlsTemplate = tlsTemplate;
    g_tlsDataSize = tlsDataSize;
    g_tlsSlotCount = tlsSlotCount ? tlsSlotCount : 64;
    g_tlsUsed.assign(g_tlsSlotCount, false);
}

void InitGuestContext(PPCContext& ctx, GuestThread& thread)
{
    ctx.r1.u64 = thread.stackHigh - 0x100;
    ctx.r13.u64 = thread.pcr;
    ctx.msr = 0x9030; // EE | ME | IR | DR
    ctx.fpscr.csr = ctx.fpscr.getcsr();
    ctx.fpscr.disableFlushModeUnconditional();
    thread.ctx = &ctx;
}

namespace
{
// PGR4_PIN (from FM2): affinity masks for guest threads, set as each one is created (from boot). Unset, it is
// "auto" (below), or "off" when settings.ini has thread_pinning=0; PGR4_PIN=off pins nothing.
// "1:3,8:C,*:F0" gives masks (hex) by guest thread id, '*' for the rest.
// "auto": the main thread and the GPU command processor get a physical core each (all its logical processors;
// a module on Bulldozer-family CPUs), from the fastest efficiency class, and every other guest thread gets the
// remaining cores. Needs 4 cores or more in processor group 0; with fewer nothing is pinned. (FM2 on the
// FX-8350: about +10%.)
// "m:MASK" is the main thread's mask and "c:MASK" the command processor's, found by name.
// "h:MASK" also keeps every other thread of the process (renderer, audio, the driver's) on those processors,
// through the process's default CPU sets; "auto" adds h: with the remaining cores. The threads with their own
// mask get matching CPU sets, so the default doesn't apply to them.
struct PinSpec
{
    std::unordered_map<uint32_t, uint64_t> masks;
    uint64_t rest = 0;
    uint64_t mainThread = 0;
    uint64_t commandProcessor = 0;
    uint64_t host = 0;
};

// CPU set ids of the logical processors in mask (processor group 0).
std::vector<ULONG> CpuSetIds(uint64_t mask)
{
    std::vector<ULONG> ids;
    ULONG bytes = 0;
    GetSystemCpuSetInformation(nullptr, 0, &bytes, GetCurrentProcess(), 0);
    std::vector<uint8_t> buffer(bytes);
    if (!bytes || !GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data()), bytes, &bytes, GetCurrentProcess(), 0))
        return ids;
    for (ULONG at = 0; at < bytes;)
    {
        auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + at);
        if (info->Type == CpuSetInformation && info->CpuSet.Group == 0 && info->CpuSet.LogicalProcessorIndex < 64 &&
            (mask >> info->CpuSet.LogicalProcessorIndex & 1))
            ids.push_back(info->CpuSet.Id);
        at += info->Size;
    }
    return ids;
}

PinSpec ParsePinSpec()
{
    PinSpec pin;
    const char* spec = getenv("PGR4_PIN");
    if (!spec || !*spec)
        spec = GetSettings().threadPinning ? "auto" : "off";
    if (!strcmp(spec, "off"))
    {
        Log("pin: off");
        return pin;
    }
    std::string autoSpec;
    if (!strcmp(spec, "auto"))
    {
        DWORD bytes = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
        std::vector<uint8_t> buffer(bytes);
        std::vector<std::pair<int, uint64_t>> cores; // efficiency class, mask
        if (bytes && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &bytes))
            for (DWORD at = 0; at < bytes;)
            {
                auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
                if (info->Processor.GroupMask[0].Group == 0)
                    cores.push_back({ info->Processor.EfficiencyClass, info->Processor.GroupMask[0].Mask });
                at += info->Size;
            }
        if (cores.size() < 4)
        {
            Log("pin: auto needs 4 cores, found %zu", cores.size());
            return pin;
        }
        std::stable_sort(cores.begin(), cores.end(), [](auto& a, auto& b) { return a.first > b.first; });
        uint64_t rest = 0;
        for (size_t i = 2; i < cores.size(); i++)
            rest |= cores[i].second;
        char s[96];
        snprintf(s, sizeof(s), "m:%llX,c:%llX,*:%llX,h:%llX", (unsigned long long)cores[0].second, (unsigned long long)cores[1].second,
            (unsigned long long)rest, (unsigned long long)rest);
        autoSpec = s;
        spec = autoSpec.c_str();
    }
    Log("pin: %s", spec);
    for (const char* p = spec; *p;)
    {
        const char key = *p == '*' || *p == 'h' || *p == 'c' || *p == 'm' ? *p : 0;
        char* end;
        const uint32_t id = key ? 0 : uint32_t(strtoul(p, &end, 10));
        p = key ? p + 1 : end;
        if (*p != ':')
            break;
        const uint64_t mask = strtoull(p + 1, &end, 16);
        p = end;
        (key == '*' ? pin.rest : key == 'h' ? pin.host : key == 'c' ? pin.commandProcessor : key == 'm' ? pin.mainThread : pin.masks[id]) = mask;
        if (*p == ',')
            ++p;
    }
    if (pin.host)
    {
        const std::vector<ULONG> ids = CpuSetIds(pin.host);
        Log("pin: host threads -> %llX (%zu CPU sets, %s)", (unsigned long long)pin.host, ids.size(),
            !ids.empty() && SetProcessDefaultCpuSets(GetCurrentProcess(), ids.data(), ULONG(ids.size())) ? "ok" : "failed");
    }
    return pin;
}

void PinGuestThread(GuestThread& t)
{
    static const PinSpec pin = ParsePinSpec();
    auto it = pin.masks.find(t.id);
    const uint64_t mask = it != pin.masks.end() ? it->second
        : pin.commandProcessor && t.name == "gpu command processor" ? pin.commandProcessor
        : pin.mainThread && t.name == "main" ? pin.mainThread
        : pin.rest;
    if (!mask)
        return;
    bool ok = SetThreadAffinityMask(HANDLE(t.hostHandle), DWORD_PTR(mask)) != 0;
    if (pin.host)
    {
        // The process default would otherwise apply to this thread too (affinity beats CPU sets only when
        // they don't overlap at all).
        const std::vector<ULONG> ids = CpuSetIds(mask);
        ok = ok && !ids.empty() && SetThreadSelectedCpuSets(HANDLE(t.hostHandle), ids.data(), ULONG(ids.size()));
    }
    Log("pin: t%u %s -> %llX (%s)", t.id, t.name.c_str(), (unsigned long long)mask, ok ? "ok" : "failed");
}
}

namespace
{
std::shared_ptr<ThreadObject> StartThread(uint32_t entry, uint32_t param, uint32_t xapiStartup, uint32_t stackSize,
    bool suspended, const char* name, std::function<void(PPCContext&)> hostEntry)
{
    auto object = std::make_shared<ThreadObject>();
    object->thread = SetUpGuestThread(stackSize, name);
    object->thread->hostEntry = std::move(hostEntry);
    object->thread->entry = entry;
    object->thread->param = param;
    object->thread->xapiStartup = xapiStartup;
    object->thread->object = object;
    object->thread->suspendCount = suspended ? 1 : 0;
    StoreBE32(object->thread->kthread + kthread::StartAddress, entry);
    object->guestObject = object->thread->kthread;
    CreateHandle(object);
    {
        std::lock_guard lock(g_threadsMutex);
        g_threadsByKthread[object->thread->kthread] = object;
    }

    DWORD hostId = 0;
    HANDLE handle = CreateThread(nullptr, kHostStackSize, HostThreadMain, object.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, &hostId);
    if (!handle)
        Fatal("CreateThread failed (%lu)", GetLastError());
    object->thread->hostHandle = handle;
    PinGuestThread(*object->thread);

    wchar_t wname[64];
    swprintf(wname, 64, L"guest %u %S", object->thread->id, name ? name : "");
    SetThreadDescription(handle, wname);
    Log("thread %u created: %s %08X param %08X%s", object->thread->id, name ? name : "entry", entry, param, suspended ? " (suspended)" : "");
    return object;
}
}

std::shared_ptr<ThreadObject> CreateGuestThread(uint32_t entry, uint32_t param, uint32_t xapiStartup,
    uint32_t stackSize, bool suspended, const char* name)
{
    return StartThread(entry, param, xapiStartup, stackSize, suspended, name, nullptr);
}

std::shared_ptr<ThreadObject> CreateHostGuestThread(const char* name, std::function<void(PPCContext&)> body)
{
    return StartThread(0, 0, 0, 0x10000, false, name, std::move(body));
}

void ResumeGuestThread(GuestThread& thread)
{
    {
        std::lock_guard lock(thread.suspendMutex);
        --thread.suspendCount;
    }
    thread.suspendCv.notify_all();
}

int SuspendGuestThread(GuestThread& thread)
{
    // Only threads that have not started yet can be suspended for real;
    // the title only does this at creation time.
    std::lock_guard lock(thread.suspendMutex);
    LOG_ONCE("SuspendGuestThread on a running thread is not implemented");
    return thread.suspendCount++;
}

void ExitCurrentGuestThread(uint32_t code)
{
    throw ExitThreadSignal{ code };
}

uint32_t InterruptLockOwner();
std::string HostStack(void* threadHandle, int maxFrames);

void ArmWatchpoint(void* threadHandle);

void ArmWatchpointAll()
{
    std::lock_guard lock(g_threadsMutex);
    for (auto& [kthread, weak] : g_threadsByKthread)
        if (auto object = weak.lock(); object && !object->exited && object->thread->hostHandle)
            ArmWatchpoint(object->thread->hostHandle);
}

void LogGuestThreads()
{
    Log("  interrupt lock owner: thread %u", InterruptLockOwner());
    std::lock_guard lock(g_threadsMutex);
    for (auto& [kthread, weak] : g_threadsByKthread)
    {
        auto object = weak.lock();
        if (!object || object->exited)
            continue;
        GuestThread& t = *object->thread;
        if (t.ctx)
        {
            Log("  thread %u %-16s %s", t.id, t.name.c_str(), HostStack(t.hostHandle, 10).c_str());
            if (!t.waitingOn.empty())
                Log("      waits on %s", t.waitingOn.c_str());
        }
        else
            Log("  thread %u %-16s not started (suspend %d)", t.id, t.name.c_str(), t.suspendCount);
    }
}

std::shared_ptr<ThreadObject> FindThreadByKthread(uint32_t kthread)
{
    std::lock_guard lock(g_threadsMutex);
    auto it = g_threadsByKthread.find(kthread);
    return it == g_threadsByKthread.end() ? nullptr : it->second.lock();
}

GuestThread* CurrentGuestThread()
{
    return t_thread;
}

uint32_t CurrentGuestThreadId()
{
    return t_thread ? t_thread->id : 0;
}

void GuestCall(PPCContext& ctx, uint32_t address)
{
    PPCFunc* func = PPC_LOOKUP_FUNC(g_base, address);
    if (!func)
        Fatal("call to unmapped guest function %08X (lr %08llX)", address, ctx.lr);
    func(ctx, g_base);
}

namespace
{
// One lock per hardware thread: clearing MSR[EE] masks interrupts only on
// the CPU the thread runs on (its PCR's CPU number), and an interrupt
// aimed at CPU n takes lock n.
std::recursive_mutex g_interruptLocks[6];
std::atomic<uint32_t> g_interruptLockOwner[6];
thread_local int t_interruptDepth = 0;
thread_local int t_interruptCpu = 0;

void Lock(int cpu)
{
    g_interruptLocks[cpu].lock();
    if (t_interruptDepth++ == 0)
    {
        t_interruptCpu = cpu;
        g_interruptLockOwner[cpu] = CurrentGuestThreadId();
    }
}

void Unlock()
{
    const int cpu = t_interruptCpu;
    if (--t_interruptDepth == 0)
        g_interruptLockOwner[cpu] = 0;
    g_interruptLocks[cpu].unlock();
}

int CpuOf(PPCContext& ctx)
{
    return *GuestPtr<uint8_t>(ctx.r13.u32 + kpcr::CurrentCpu) % 6;
}
}

void InterruptLockAcquire(int cpu) { Lock(cpu % 6); }
void InterruptLockRelease() { Unlock(); }
uint32_t InterruptLockOwner()
{
    uint32_t owners = 0;
    for (auto& o : g_interruptLockOwner)
        owners = owners ? owners : o.load();
    return owners;
}

void PPCMsrChanged(PPCContext& ctx, uint64_t oldMsr)
{
    if (oldMsr & 0x8000)
        Lock(t_interruptDepth ? t_interruptCpu : CpuOf(ctx)); // interrupts now disabled
    else if (t_interruptDepth > 0)
        Unlock(); // re-enabled
}

uint32_t TlsAllocSlot()
{
    std::lock_guard lock(g_tlsMutex);
    for (uint32_t i = 0; i < g_tlsUsed.size(); i++)
    {
        if (!g_tlsUsed[i])
        {
            g_tlsUsed[i] = true;
            return i;
        }
    }
    return 0xFFFFFFFF;
}

void TlsFreeSlot(uint32_t slot)
{
    std::lock_guard lock(g_tlsMutex);
    if (slot < g_tlsUsed.size())
        g_tlsUsed[slot] = false;
}

// Per-thread CPU use since the previous call, as a percentage of wall time.
std::string GuestThreadCpuUsage(double wallSeconds)
{
    static std::unordered_map<uint32_t, uint64_t> last;
    std::string out;
    std::lock_guard lock(g_threadsMutex);
    for (auto& [kthread, weak] : g_threadsByKthread)
    {
        auto object = weak.lock();
        if (!object || object->exited || !object->thread->hostHandle)
            continue;
        GuestThread& t = *object->thread;
        FILETIME c, e, k, u;
        if (!GetThreadTimes(t.hostHandle, &c, &e, &k, &u))
            continue;
        const uint64_t total = (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
        const uint64_t delta = total - last[t.id];
        last[t.id] = total;
        const double pct = delta / 1e7 / wallSeconds * 100.0;
        if (pct >= 5.0)
        {
            char s[48];
            snprintf(s, sizeof(s), " t%u:%.0f%%", t.id, pct);
            out += s;
        }
    }
    return out;
}

// Spike recorder (PGR4_SPIKES=1): every millisecond, what each busy guest thread is doing
// (host function and guest return address), kept for a short while; SpikeDump logs the
// last stretch when the renderer sees a slow frame.
namespace
{
struct SpikeSample
{
    std::chrono::steady_clock::time_point time;
    uint32_t thread;
    uint64_t rip, lr;
};
std::mutex g_spikeMutex;
std::deque<SpikeSample> g_spikeSamples;
}

void StartSpikeRecorder()
{
    if (!getenv("PGR4_SPIKES"))
        return;
    // PGR4_SPIKES=<id>: only that thread, sampled every ~0.25 ms (top functions per spike).
    // PGR4_SPIKES=3,7,9: only those threads, sampled every ~0.25 ms.
    static const std::vector<uint32_t> onlyList = [] {
        std::vector<uint32_t> v;
        for (const char* p = getenv("PGR4_SPIKES"); *p;)
        {
            char* end;
            const unsigned long id = strtoul(p, &end, 10);
            if (end == p)
                break;
            if (id > 1)
                v.push_back(uint32_t(id));
            p = *end == ',' ? end + 1 : end;
        }
        return v;
    }();
    static const bool only = !onlyList.empty();
    std::thread([] {
        SetThreadDescription(GetCurrentThread(), L"spike recorder");
        for (;;)
        {
            if (only)
            {
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(250);
                while (std::chrono::steady_clock::now() < until)
                    SwitchToThread();
            }
            else
                Sleep(1);
            std::vector<std::pair<uint32_t, HANDLE>> threads;
            std::vector<PPCContext*> contexts;
            {
                std::lock_guard lock(g_threadsMutex);
                for (auto& [kthread, weak] : g_threadsByKthread)
                    if (auto object = weak.lock(); object && !object->exited && object->thread->hostHandle && (!only || std::find(onlyList.begin(), onlyList.end(), object->thread->id) != onlyList.end()))
                    {
                        threads.push_back({ object->thread->id, object->thread->hostHandle });
                        contexts.push_back(object->thread->ctx);
                    }
            }
            const auto now = std::chrono::steady_clock::now();
            std::vector<SpikeSample> batch;
            for (size_t i = 0; i < threads.size(); i++)
            {
                if (SuspendThread(threads[i].second) == DWORD(-1))
                    continue;
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                const bool ok = GetThreadContext(threads[i].second, &c);
                const uint64_t lr = contexts[i] ? contexts[i]->lr : 0;
                ResumeThread(threads[i].second);
                if (ok)
                    batch.push_back({ now, threads[i].first, c.Rip, lr });
            }
            std::lock_guard lock(g_spikeMutex);
            for (auto& b : batch)
                g_spikeSamples.push_back(b);
            while (!g_spikeSamples.empty() && now - g_spikeSamples.front().time > std::chrono::milliseconds(300))
                g_spikeSamples.pop_front();
        }
    }).detach();
}

// Logs, per thread, what it did in the last `ms` milliseconds (runs of the same place).
void SpikeDump(double ms)
{
    if (!getenv("PGR4_SPIKES"))
        return;
    std::vector<SpikeSample> samples;
    {
        std::lock_guard lock(g_spikeMutex);
        const auto from = std::chrono::steady_clock::now() - std::chrono::microseconds(int64_t(ms * 1000));
        for (auto& s : g_spikeSamples)
            if (s.time >= from)
                samples.push_back(s);
    }
    std::map<uint32_t, std::vector<const SpikeSample*>> byThread;
    for (auto& s : samples)
        byThread[s.thread].push_back(&s);
    auto name = [](uint64_t rip) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        return std::string(SymFromAddr(GetCurrentProcess(), rip, &displacement, symbol) ? symbol->Name : "?");
    };
    static const bool only = atoi(getenv("PGR4_SPIKES")) > 1 && !strchr(getenv("PGR4_SPIKES"), ',');
    if (only)
    {
        // Top places in the window.
        std::map<std::string, int> counts;
        for (auto& s : samples)
        {
            char lr[16];
            snprintf(lr, sizeof(lr), "@%08llX", (unsigned long long)s.lr);
            counts[name(s.rip) + lr]++;
        }
        std::vector<std::pair<int, std::string>> top;
        for (auto& [k, v] : counts)
            top.push_back({ v, k });
        std::sort(top.rbegin(), top.rend());
        std::string line;
        for (size_t i = 0; i < top.size() && i < 12; i++)
            line += " | " + top[i].second + " x" + std::to_string(top[i].first);
        Log("spike: %zu samples:%s", samples.size(), line.c_str());
        return;
    }
    for (auto& [thread, list] : byThread)
    {
        std::string line;
        std::string last;
        int run = 0;
        auto flush = [&] {
            if (run)
                line += " | " + last + (run > 1 ? " x" + std::to_string(run) : "");
        };
        for (auto* s : list)
        {
            char lr[16];
            snprintf(lr, sizeof(lr), "@%08llX", (unsigned long long)s->lr);
            const std::string here = name(s->rip) + lr;
            if (here == last)
                run++;
            else
            {
                flush();
                last = here;
                run = 1;
            }
        }
        flush();
        Log("spike: t%u%s", thread, line.c_str());
    }
}


// Sampling profiler (PGR4_SAMPLE_THREAD=<guest thread id>; PGR4_PROFILE is the profile name): every millisecond, read the
// thread's instruction pointer and count it per function; the top of the table
// is logged every 5 seconds. Needs the symbols crash.cpp loads.
void StartThreadProfiler()
{
    static const char* env = getenv("PGR4_SAMPLE_THREAD");
    if (!env)
        return;
    const uint32_t target = uint32_t(atoi(env)); // 0 with "w": the first draw piece worker
    std::thread([target] {
        std::unordered_map<std::string, uint64_t> counts;
        std::map<std::array<uint32_t, 3>, uint64_t> rvas; // the address, then its callers (PGR4_SAMPLE_CALLERS)
        uint64_t samples = 0;
        auto lastLog = std::chrono::steady_clock::now();
        // PGR4_SAMPLE_AFTER=<seconds>: start sampling only then (stopping the command processor while it
        // starts the renderer hung the boot)
        if (const char* after = getenv("PGR4_SAMPLE_AFTER"))
            Sleep(DWORD(atoi(after)) * 1000);
        for (;;)
        {
            Sleep(1);
            HANDLE handle = nullptr;
            extern HANDLE g_profiledWorker;
            // PGR4_SAMPLE_THREAD=n:<text>: the host thread whose description contains text ("n:replay")
            static HANDLE named = nullptr;
            if (env[0] == 'n' && env[1] == ':')
            {
                if (!named)
                {
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    THREADENTRY32 te{ sizeof(te) };
                    for (BOOL more = Thread32First(snap, &te); more && !named; more = Thread32Next(snap, &te))
                    {
                        if (te.th32OwnerProcessID != GetCurrentProcessId())
                            continue;
                        HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
                        PWSTR desc = nullptr;
                        if (t && SUCCEEDED(GetThreadDescription(t, &desc)) && desc)
                        {
                            char narrow[256] = {};
                            WideCharToMultiByte(CP_UTF8, 0, desc, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
                            LocalFree(desc);
                            if (strstr(narrow, env + 2))
                            {
                                named = t;
                                Log("profile: sampling host thread %lu (%s)", te.th32ThreadID, narrow);
                                continue;
                            }
                        }
                        if (t)
                            CloseHandle(t);
                    }
                    CloseHandle(snap);
                }
                handle = named;
            }
            else if (env[0] == 'w')
                handle = g_profiledWorker;
            else
            {
                std::lock_guard lock(g_threadsMutex);
                for (auto& [kthread, weak] : g_threadsByKthread)
                    if (auto object = weak.lock(); object && !object->exited && object->thread->id == target)
                        handle = object->thread->hostHandle;
            }
            if (!handle)
                continue;
            if (SuspendThread(handle) == DWORD(-1))
                continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            const bool ok = GetThreadContext(handle, &c);
            // PGR4_SAMPLE_CALLERS=1: also the two callers (unwound while the thread is stopped); =N: N callers (up to 6)
            static LiveSwitch callers("PGR4_SAMPLE_CALLERS");
            static const int depth = getenv("PGR4_SAMPLE_CALLERS") ? std::clamp(atoi(getenv("PGR4_SAMPLE_CALLERS")) == 1 ? 2 : atoi(getenv("PGR4_SAMPLE_CALLERS")), 1, 6) : 2;
            DWORD64 up[6] = {};
            if (ok && callers)
            {
                CONTEXT u = c;
                for (int k = 0; k < depth; k++)
                {
                    DWORD64 imageBase = 0;
                    auto* fn = RtlLookupFunctionEntry(u.Rip, &imageBase, nullptr);
                    if (!fn)
                    {
                        u.Rip = *reinterpret_cast<DWORD64*>(u.Rsp); // leaf
                        u.Rsp += 8;
                    }
                    else
                    {
                        void* handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, u.Rip, fn, &u, &handlerData, &establisher, nullptr);
                    }
                    up[k] = u.Rip;
                    if (!u.Rip)
                        break;
                }
            }
            // Symbols are looked up after the thread runs again: the lookups take locks
            // (heap, loader) that the suspended thread may hold.
            ResumeThread(handle);
            if (!ok)
                continue;
            std::string name;
            {
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 displacement = 0;
                name = SymFromAddr(GetCurrentProcess(), c.Rip, &displacement, symbol) ? symbol->Name : "?";
                // PGR4_SAMPLE_LINES=1: by source line, not by function
                static LiveSwitch byLine("PGR4_SAMPLE_LINES");
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisplacement = 0;
                if (byLine && name != "?" && SymGetLineFromAddr64(GetCurrentProcess(), c.Rip, &lineDisplacement, &line))
                {
                    const char* file = strrchr(line.FileName, 92);
                    name += std::string(" (") + (file ? file + 1 : line.FileName) + ":" + std::to_string(line.LineNumber) + ")";
                }
                // No symbol: name the module (a driver, for example).
                HMODULE module = nullptr;
                char path[MAX_PATH];
                if (name == "?" && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         reinterpret_cast<LPCSTR>(c.Rip), &module) && GetModuleFileNameA(module, path, MAX_PATH))
                    name = std::string("? ") + (strrchr(path, 92) ? strrchr(path, 92) + 1 : path);
                // Our own code without symbols (dbghelp on the machine can't read the pdb): its address in
                // the image, written to out/profile_rva.txt for tools to name against the pdb elsewhere.
                const DWORD64 base = reinterpret_cast<DWORD64>(GetModuleHandleA(nullptr));
                auto rva = [&](DWORD64 a) { return a > base && a - base < 0x10000000 ? uint32_t(a - base) : 0u; };
                if (name != "?" && module == GetModuleHandleA(nullptr) && name.rfind("? ", 0) == 0)
                    rvas[{ rva(c.Rip), rva(up[0]), rva(up[1]) }]++;
                else
                    // Outside our code (a wait, the driver): the first two of our callers, under address 0.
                    for (int k = 0; k + 1 < 6; k++)
                        if (rva(up[k]))
                        {
                            rvas[{ 0u, rva(up[k]), rva(up[k + 1]) }]++;
                            break;
                        }
            }
            for (DWORD64 addr : up)
            {
                if (!addr)
                    break;
                alignas(SYMBOL_INFO) char buffer2[sizeof(SYMBOL_INFO) + 256];
                auto* symbol2 = reinterpret_cast<SYMBOL_INFO*>(buffer2);
                symbol2->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol2->MaxNameLen = 255;
                DWORD64 displacement2 = 0;
                name += std::string(" < ") + (SymFromAddr(GetCurrentProcess(), addr, &displacement2, symbol2) ? symbol2->Name : "?");
                // REGPU_SAMPLE_CALLER_LINES=1: the first caller's source line too (which memcpy call, say)
                static LiveSwitch callerLines("REGPU_SAMPLE_CALLER_LINES");
                IMAGEHLP_LINE64 line2{};
                line2.SizeOfStruct = sizeof(line2);
                DWORD lineDisplacement2 = 0;
                if (callerLines && addr == up[0] && SymGetLineFromAddr64(GetCurrentProcess(), addr - 1, &lineDisplacement2, &line2))
                    name += ":" + std::to_string(line2.LineNumber);
            }
            counts[name]++;
            samples++;
            if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(5))
            {
                lastLog = std::chrono::steady_clock::now();
                std::vector<std::pair<uint64_t, std::string>> top;
                for (auto& [n, k] : counts)
                    top.push_back({ k, n });
                std::sort(top.rbegin(), top.rend());
                Log("profile: thread %u, %llu samples", target, (unsigned long long)samples);
                for (size_t i = 0; i < top.size() && i < 25; i++)
                    Log("profile: %5.1f%% %s", top[i].first * 100.0 / samples, top[i].second.c_str());
                if (!rvas.empty())
                {
                    if (FILE* out = fopen("out/profile_rva.txt", "a"))
                    {
                        fprintf(out, "# %llu samples\n", (unsigned long long)samples); // each 5 s apart
                        for (auto& [rva, k] : rvas)
                            if (rva[1])
                                fprintf(out, "%08X %llu %08X %08X\n", rva[0], (unsigned long long)k, rva[1], rva[2]);
                            else
                                fprintf(out, "%08X %llu\n", rva[0], (unsigned long long)k);
                        fclose(out);
                    }
                    rvas.clear();
                }
                counts.clear();
                samples = 0;
            }
        }
    }).detach();
}
