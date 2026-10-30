// TTA_HOST_PROFILE=<file>: where the PC's time goes, by host instruction address. A host thread samples every thread
// of the process each millisecond (suspend, read the instruction pointer, resume) and every 10 s appends to <file>
// each thread's CPU time (QueryThreadCycleTime, the driver's threads too) and, for the busiest six, the 40 most
// sampled addresses as offsets into the image (resolve them with addr2line -f -C -e ps2EntryRunner.debug
// 0x140000000+offset; bit 62 set: waiting outside the image, called from there) and "waits a>b>c>d" lines, the first
// four return addresses into the image found on a waiting thread's stack. Windows only.

#include "tta_host_profile.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    void sampler(std::string path)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const DWORD self = GetCurrentThreadId();
        std::unordered_map<DWORD, std::unordered_map<uintptr_t, uint32_t>> samples; // thread -> offset -> count
        std::unordered_map<DWORD, std::map<std::array<uintptr_t, 4>, uint32_t>> waits; // thread -> callers -> count
        std::unordered_map<DWORD, ULONG64> cycles; // CPU cycles at the last report, the driver's threads included
        std::unordered_map<DWORD, uint32_t> totals; // samples inside the image
        const uintptr_t imageSize = reinterpret_cast<const IMAGE_NT_HEADERS *>(
            base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew)->OptionalHeader.SizeOfImage;
        auto since = std::chrono::steady_clock::now();
        std::vector<std::pair<DWORD, HANDLE>> threads; // refreshed once a second: a snapshot per sample is too slow
        auto listed = std::chrono::steady_clock::time_point{};
        for (;;)
        {
            if (std::chrono::steady_clock::now() - listed >= std::chrono::seconds(1))
            {
                for (const auto &[id, handle] : threads)
                    CloseHandle(handle);
                threads.clear();
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (snapshot != INVALID_HANDLE_VALUE)
                {
                    THREADENTRY32 entry{};
                    entry.dwSize = sizeof(entry);
                    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
                    {
                        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self)
                            continue;
                        if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID))
                            threads.emplace_back(entry.th32ThreadID, thread);
                    }
                    CloseHandle(snapshot);
                }
                listed = std::chrono::steady_clock::now();
            }
            for (const auto &[id, thread] : threads)
            {
                if (SuspendThread(thread) == static_cast<DWORD>(-1))
                    continue;
                // Nothing that may take a lock (the heap's) while the thread is suspended.
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                const bool got = GetThreadContext(thread, &context) != 0;
                // Outside the image (a wait in a system DLL): the first return address into the image on the stack
                // says who waits; recorded with bit 62 set.
                std::array<uintptr_t, 4> callers{};
                size_t found = 0;
                const uintptr_t rip = static_cast<uintptr_t>(context.Rip);
                if (got && !(rip >= base && rip - base < imageSize))
                {
                    const auto *stack = reinterpret_cast<const uintptr_t *>(context.Rsp);
                    MEMORY_BASIC_INFORMATION region{};
                    size_t words = 0;
                    if (VirtualQuery(stack, &region, sizeof(region)) && region.State == MEM_COMMIT)
                        words = std::min<size_t>(512, (static_cast<const char *>(region.BaseAddress) + region.RegionSize -
                                                       reinterpret_cast<const char *>(stack)) / sizeof(uintptr_t));
                    for (size_t i = 0; i < words && found < callers.size(); ++i)
                        if (stack[i] > base && stack[i] - base < imageSize)
                            callers[found++] = stack[i] - base;
                }
                ResumeThread(thread);
                if (got)
                {
                    if (rip >= base && rip - base < imageSize)
                    {
                        ++samples[id][rip - base];
                        ++totals[id];
                    }
                    else
                    {
                        ++samples[id][found ? callers[0] | (uintptr_t{1} << 62) : rip | (uintptr_t{1} << 63)];
                        if (found)
                            ++waits[id][callers];
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - since >= std::chrono::seconds(10))
            {
                if (FILE *file = std::fopen(path.c_str(), "a"))
                {
                    std::fprintf(file, "== 10 s%c", 10);
                    std::vector<std::pair<ULONG64, DWORD>> busy;
                    for (const auto &[id, thread] : threads)
                    {
                        ULONG64 now = 0;
                        if (QueryThreadCycleTime(thread, &now))
                        {
                            const auto it = cycles.find(id);
                            busy.emplace_back(it == cycles.end() ? 0 : now - it->second, id);
                            cycles[id] = now;
                        }
                    }
                    std::sort(busy.rbegin(), busy.rend());
                    for (size_t t = 0; t < busy.size() && t < 8u; ++t)
                        std::fprintf(file, "cpu thread %lu: %.2f Gcycles%c", static_cast<unsigned long>(busy[t].second),
                                     static_cast<double>(busy[t].first) / 1e9, 10);
                    for (size_t t = 0; t < busy.size() && t < 6u; ++t)
                    {
                        const DWORD id = busy[t].second;
                        std::vector<std::pair<uint32_t, uintptr_t>> hot;
                        for (const auto &[offset, count] : samples[id])
                            hot.emplace_back(count, offset);
                        std::sort(hot.rbegin(), hot.rend());
                        std::fprintf(file, "thread %lu: %u samples in the image%c", static_cast<unsigned long>(id), totals[id], 10);
                        for (size_t i = 0; i < hot.size() && i < 40u; ++i)
                            std::fprintf(file, "  %llx %u%c", static_cast<unsigned long long>(hot[i].second), hot[i].first, 10);
                        std::vector<std::pair<uint32_t, std::array<uintptr_t, 4>>> chains;
                        for (const auto &[chain, count] : waits[id])
                            chains.emplace_back(count, chain);
                        std::sort(chains.rbegin(), chains.rend());
                        for (size_t i = 0; i < chains.size() && i < 8u; ++i)
                            std::fprintf(file, "  waits %llx>%llx>%llx>%llx %u%c",
                                         static_cast<unsigned long long>(chains[i].second[0]),
                                         static_cast<unsigned long long>(chains[i].second[1]),
                                         static_cast<unsigned long long>(chains[i].second[2]),
                                         static_cast<unsigned long long>(chains[i].second[3]), chains[i].first, 10);
                    }
                    std::fclose(file);
                }
                samples.clear();
                waits.clear();
                totals.clear();
                since = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

namespace tta
{
    void startHostProfile(const char *path)
    {
        std::thread(sampler, std::string(path)).detach();
    }
}
#else
namespace tta
{
    void startHostProfile(const char *) {}
}
#endif
