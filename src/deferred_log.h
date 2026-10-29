#pragma once

// Evidence text made on a worker thread and written to the mod log by the
// script thread on a later tick. Written inside the recording stop's one tick,
// the POSE_V2_CAPTURE_STOP lines (~2.4 KB of iostream formatting per pose
// record), the speech lines of the observer ring (up to 1024) and the clip
// pointer audit cost from a few ms to over 100 ms.
//
// The text is the same, only later. A job's text goes through the log
// filter with the quiet state of the tick that asked for it (Submit keeps
// replay::log::Quiet(); Debug is read once at load), so the same lines are
// kept or dropped as when they were asked for. Jobs are written in the
// order they were asked for, whole lines, at most a byte budget per tick.
//
// Threads: Submit and Poll on the script thread only. The work runs on a
// std::thread of its own, like the clip read and save workers (script.cpp):
// the CRT start pins this module while it runs (an unload waits for it),
// and a job destroyed with the module detaches its finished thread. The
// work gets its inputs by value: no engine memory, no natives, nothing the
// script thread changes. A thread that cannot start runs the work inline.
//
// Pure apart from the threads: unit tested (tests/stop_path_tests.cpp).

#include "log_policy.h"
#include "thread_names.h"

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <Windows.h> // WaitForSingleObject
#endif

namespace replay::deferredlog
{
    class Queue
    {
    public:
        Queue() = default;
        Queue(const Queue&) = delete;
        Queue& operator=(const Queue&) = delete;

        // Starts `work` (a copyable callable returning std::string of whole
        // '\n'-terminated lines) on a thread of its own.
        void Submit(std::function<std::string()> work)
        {
            auto job = std::make_unique<Job>();
            job->quiet = replay::log::Quiet();
            job->work = std::move(work);
            Job* raw = job.get();
            m_jobs.push_back(std::move(job));
            try
            {
                raw->worker = std::thread([raw]() {
                    replay::threads::Scope name("deferred_log");
                    Run(*raw);
                });
            }
            catch (const std::system_error&)
            {
                Run(*raw); // no thread: the same text, now
            }
        }

        // Script thread: the finished jobs' text, in the order they were
        // asked for, through `write(text, quiet)`; whole lines, at most
        // `budgetBytes` per call (0: no limit; a single longer line still
        // goes whole when nothing was written yet); stops at the first job
        // not finished. Returns the bytes written.
        template <typename Write>
        std::size_t Poll(Write&& write, std::size_t budgetBytes)
        {
            std::size_t written = 0;
            while (!m_jobs.empty())
            {
                Job& job = *m_jobs.front();
                if (!job.done.load(std::memory_order_acquire))
                    break;
                if (job.worker.joinable())
                {
                    // `done` is set before the thread really ends (its exit
                    // takes the loader lock, which a module load on another
                    // thread can hold for long): join only a thread already
                    // ended, else the next Poll tries again, so the tick never
                    // waits.
#ifdef _WIN32
                    if (WaitForSingleObject(static_cast<HANDLE>(job.worker.native_handle()), 0) != WAIT_OBJECT_0)
                        break;
#endif
                    job.worker.join();
                }
                const std::size_t room = budgetBytes == 0 ? std::string::npos
                    : (budgetBytes > written ? budgetBytes - written : 0);
                const std::size_t take = Take(job.text, job.offset, room, written == 0);
                if (take > 0)
                {
                    write(std::string_view(job.text).substr(job.offset, take), job.quiet);
                    job.offset += take;
                    written += take;
                }
                if (job.offset < job.text.size())
                    break; // the budget is spent: the rest on the next call
                m_jobs.pop_front();
            }
            return written;
        }

        // Every job waited for and written, no budget (tests).
        template <typename Write>
        std::size_t Drain(Write&& write)
        {
            for (const std::unique_ptr<Job>& job : m_jobs)
            {
                if (job->worker.joinable())
                    job->worker.join();
            }
            return Poll(write, 0);
        }

        std::size_t Pending() const { return m_jobs.size(); }

        // The bytes of `text` from `offset` that go now: all of it when it
        // fits `room`, else up to its last line end within `room`; with
        // `force`, at least the first line.
        static std::size_t Take(const std::string& text, std::size_t offset, std::size_t room, bool force)
        {
            const std::size_t left = text.size() - offset;
            if (left <= room)
                return left;
            if (room > 0)
            {
                const std::size_t end = text.rfind('\n', offset + room - 1);
                if (end != std::string::npos && end >= offset)
                    return end + 1 - offset;
            }
            if (!force)
                return 0;
            const std::size_t end = text.find('\n', offset);
            return end == std::string::npos ? left : end + 1 - offset;
        }

    private:
        struct Job
        {
            std::function<std::string()> work;
            std::thread worker;
            std::atomic<bool> done{false};
            bool quiet = false;
            std::string text;
            std::size_t offset = 0;
            ~Job()
            {
                if (worker.joinable())
                    worker.detach(); // as ClipRead (script.cpp): a joinable std::thread here ends the process
            }
        };

        static void Run(Job& job)
        {
            try
            {
                job.text = job.work();
            }
            catch (...)
            {
                job.text = "DEFERRED_LOG_ERROR,what=exception\n"; // bad_alloc: the lines are lost, the game goes on
            }
            job.work = nullptr; // the inputs go with the thread
            job.done.store(true, std::memory_order_release);
        }

        std::deque<std::unique_ptr<Job>> m_jobs;
    };

    // The mod's queue: the jobs of the recording stop; script.cpp polls it
    // once per tick.
    inline Queue& Log()
    {
        static Queue queue;
        return queue;
    }
}
