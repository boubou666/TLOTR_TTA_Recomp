// TTA_HOST_PROFILE=<file>: where the PC's time goes, by host instruction address. A host thread samples every thread
// of the process each millisecond (suspend, read the instruction pointer, resume) and every 10 s appends to <file> the
// busiest threads and, for each, its 40 most sampled addresses as offsets into the runner's image (resolve them with
// addr2line -f -C -e ps2EntryRunner.debug 0x140000000+offset). Windows only.

#include "tta_host_profile.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
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
        std::unordered_map<DWORD, uint32_t> totals;
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
                        if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID))
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
                ResumeThread(thread);
                if (got)
                {
                    const uintptr_t rip = static_cast<uintptr_t>(context.Rip);
                    ++samples[id][rip >= base ? rip - base : rip | (uintptr_t{1} << 63)];
                    ++totals[id];
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - since >= std::chrono::seconds(10))
            {
                if (FILE *file = std::fopen(path.c_str(), "a"))
                {
                    std::vector<std::pair<uint32_t, DWORD>> threads;
                    for (const auto &[id, count] : totals)
                        threads.emplace_back(count, id);
                    std::sort(threads.rbegin(), threads.rend());
                    std::fprintf(file, "== 10 s%c", 10);
                    for (size_t t = 0; t < threads.size() && t < 6u; ++t)
                    {
                        const DWORD id = threads[t].second;
                        std::vector<std::pair<uint32_t, uintptr_t>> hot;
                        for (const auto &[offset, count] : samples[id])
                            hot.emplace_back(count, offset);
                        std::sort(hot.rbegin(), hot.rend());
                        std::fprintf(file, "thread %lu: %u samples%c", static_cast<unsigned long>(id), threads[t].first, 10);
                        for (size_t i = 0; i < hot.size() && i < 40u; ++i)
                            std::fprintf(file, "  %llx %u%c", static_cast<unsigned long long>(hot[i].second), hot[i].first, 10);
                    }
                    std::fclose(file);
                }
                samples.clear();
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
