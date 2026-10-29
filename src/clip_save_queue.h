#pragma once

// Clip saves as transactions. Every save is a job that owns its snapshot of
// the clip (the cut window, the stopped recording, the edited clip with its
// markers) from the moment it is asked for until the file is written, so a
// clip being saved is never lost. A failed write keeps the job (held) and
// tries again later; the data is dropped only when written, when a newer save
// of the same file replaces it, or when that file is deleted. A load of
// a file with a job reads the job's snapshot: it is newer than the file.
//
// The queue is pure bookkeeping over an opaque payload: one job is written at
// a time, in the order asked, retried with a growing delay. No engine, no
// file access; unit tested in clip_file_tests.cpp.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace replay::savequeue
{
    // The delay before a failed job is written again: 5 s, 15 s, 30 s, then
    // every 60 s.
    inline std::uint64_t RetryDelayMs(std::uint32_t failures)
    {
        switch (failures)
        {
        case 0:
            return 0;
        case 1:
            return 5000;
        case 2:
            return 15000;
        case 3:
            return 30000;
        default:
            return 60000;
        }
    }

    enum class Outcome
    {
        Saved,      // written: the job is gone
        Held,       // failed: kept, retried after RetryDelayMs
        Superseded, // failed, but a newer job of the same file holds all of it
        Dropped,    // its file was deleted meanwhile
        Unknown     // no job being written with that id
    };

    template <typename Payload>
    class Queue
    {
    public:
        struct Job
        {
            std::uint64_t id = 0;
            std::wstring path;
            Payload payload{};
            std::uint32_t failures = 0;
            std::uint64_t retryAtMs = 0;
            std::string lastError;
            bool writing = false;
            bool dropped = false; // the file was deleted while it was written
        };

        // A save of `path`. A job of the same file that waits or is held is
        // replaced (the new snapshot is the whole clip, newer); one being
        // written stays, and this one follows it. Returns the job id.
        std::uint64_t Add(const std::wstring& path, Payload payload)
        {
            for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it)
            {
                if (it->path != path || it->writing)
                    continue;
                it->payload = std::move(payload);
                it->failures = 0;
                it->retryAtMs = 0;
                it->lastError.clear();
                it->id = ++m_lastId;
                // Keep the order asked: it moves behind the others.
                Job moved = std::move(*it);
                m_jobs.erase(it);
                m_jobs.push_back(std::move(moved));
                return m_lastId;
            }
            Job job;
            job.id = ++m_lastId;
            job.path = path;
            job.payload = std::move(payload);
            m_jobs.push_back(std::move(job));
            return m_lastId;
        }

        // The next job to write now (the oldest due one), marked as being
        // written; null while one is written or none is due.
        Job* Begin(std::uint64_t nowMs)
        {
            for (const Job& job : m_jobs)
                if (job.writing)
                    return nullptr;
            for (Job& job : m_jobs)
            {
                if (job.retryAtMs <= nowMs)
                {
                    job.writing = true;
                    return &job;
                }
            }
            return nullptr;
        }

        // The write of job `id` ended. On success the job goes; on failure
        // it is held for a retry, unless a newer job of the same file
        // replaced it or the file was deleted. `out` (may be null) gets the
        // job as it was (its payload moved out when it leaves the queue).
        Outcome Finish(std::uint64_t id, bool ok, const std::string& error, std::uint64_t nowMs, Job* out = nullptr)
        {
            for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it)
            {
                if (it->id != id || !it->writing)
                    continue;
                it->writing = false;
                bool newer = false;
                for (const Job& other : m_jobs)
                    newer = newer || (other.id != id && other.path == it->path);
                if (!ok)
                {
                    ++it->failures;
                    it->lastError = error;
                    it->retryAtMs = nowMs + RetryDelayMs(it->failures);
                }
                const Outcome outcome = it->dropped ? Outcome::Dropped
                    : ok                            ? Outcome::Saved
                    : newer                         ? Outcome::Superseded
                                                    : Outcome::Held;
                if (outcome == Outcome::Held)
                {
                    if (out)
                        *out = *it;
                    return outcome;
                }
                if (out)
                    *out = std::move(*it);
                m_jobs.erase(it);
                return outcome;
            }
            return Outcome::Unknown;
        }

        // The newest job of `path` (the content a load of that file must
        // show), null when none.
        const Job* Find(const std::wstring& path) const
        {
            const Job* found = nullptr;
            for (const Job& job : m_jobs)
                if (job.path == path && (!found || job.id > found->id))
                    found = &job;
            return found;
        }
        bool Holds(const std::wstring& path) const { return Find(path) != nullptr; }

        // The file was renamed: its jobs write the new name.
        void Rename(const std::wstring& from, const std::wstring& to)
        {
            for (Job& job : m_jobs)
                if (job.path == from)
                    job.path = to;
        }

        // The user deleted the file: its jobs go (one being written is
        // dropped when it ends; the caller deletes the file after it).
        std::size_t Drop(const std::wstring& path)
        {
            std::size_t dropped = 0;
            for (auto it = m_jobs.begin(); it != m_jobs.end();)
            {
                if (it->path != path)
                {
                    ++it;
                    continue;
                }
                ++dropped;
                if (it->writing)
                {
                    it->dropped = true;
                    ++it;
                }
                else
                    it = m_jobs.erase(it);
            }
            return dropped;
        }

        // The user asked for a save again (or disk space may be back): the
        // held jobs are due now.
        void RetryNow()
        {
            for (Job& job : m_jobs)
                if (!job.writing)
                    job.retryAtMs = 0;
        }

        // Jobs that failed at least once and wait for a retry.
        std::size_t HeldCount() const
        {
            std::size_t held = 0;
            for (const Job& job : m_jobs)
                held += job.failures > 0 ? 1 : 0;
            return held;
        }
        std::size_t Size() const { return m_jobs.size(); }
        bool Empty() const { return m_jobs.empty(); }
        std::deque<Job>& Jobs() { return m_jobs; }
        const std::deque<Job>& Jobs() const { return m_jobs; }

    private:
        std::deque<Job> m_jobs;
        std::uint64_t m_lastId = 0;
    };
}
