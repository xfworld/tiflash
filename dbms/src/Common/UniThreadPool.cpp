// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/UniThreadPool.h>
#include <Common/setThreadName.h>
#include <IO/IOThreadPool.h>
#include <Poco/Util/Application.h>
#include <Poco/Util/LayeredConfiguration.h>

#include <cassert>
#include <iostream>
#include <type_traits>

namespace DB::ErrorCodes
{
extern const int CANNOT_SCHEDULE_TASK;
extern const int LOGICAL_ERROR;
} // namespace DB::ErrorCodes

namespace CurrentMetrics
{
extern const Metric GlobalThread;
extern const Metric GlobalThreadActive;
extern const Metric LocalThread;
extern const Metric LocalThreadActive;
} // namespace CurrentMetrics

namespace DB
{
template <typename Thread>
ThreadPoolImpl<Thread>::ThreadPoolImpl(size_t max_threads_)
    : ThreadPoolImpl(max_threads_, max_threads_, max_threads_)
{}

template <typename Thread>
ThreadPoolImpl<Thread>::ThreadPoolImpl(
    size_t max_threads_,
    size_t max_free_threads_,
    size_t queue_size_,
    bool shutdown_on_exception_)
    : max_threads(max_threads_)
    , max_free_threads(max_free_threads_)
    , queue_size(queue_size_)
    , shutdown_on_exception(shutdown_on_exception_)
{}

template <typename Thread>
void ThreadPoolImpl<Thread>::setMaxThreads(size_t value)
{
    std::lock_guard lock(mutex);
    max_threads = value;
    /// We have to also adjust queue size, because it limits the number of scheduled and already running jobs in total.
    queue_size = std::max(queue_size, max_threads);
    jobs.reserve(queue_size);
}

template <typename Thread>
size_t ThreadPoolImpl<Thread>::getMaxThreads() const
{
    std::lock_guard lock(mutex);
    return max_threads;
}

template <typename Thread>
void ThreadPoolImpl<Thread>::setMaxFreeThreads(size_t value)
{
    std::lock_guard lock(mutex);
    max_free_threads = value;
}

template <typename Thread>
void ThreadPoolImpl<Thread>::setQueueSize(size_t value)
{
    std::lock_guard lock(mutex);
    queue_size = value;
    /// Reserve memory to get rid of allocations
    jobs.reserve(queue_size);
}

template <typename Thread>
size_t ThreadPoolImpl<Thread>::getQueueSize() const
{
    std::lock_guard lock(mutex);
    return queue_size;
}


template <typename Thread>
template <typename ReturnType>
ReturnType ThreadPoolImpl<Thread>::scheduleImpl(
    Job job,
    ssize_t priority,
    std::optional<uint64_t> wait_microseconds,
    bool propagate_opentelemetry_tracing_context)
{
    UNUSED(propagate_opentelemetry_tracing_context);

    auto on_error = [&](const std::string & reason) {
        if constexpr (std::is_same_v<ReturnType, void>)
        {
            if (first_exception)
            {
                std::exception_ptr exception;
                std::swap(exception, first_exception);
                std::rethrow_exception(exception);
            }
            throw DB::Exception(
                DB::ErrorCodes::CANNOT_SCHEDULE_TASK,
                "Cannot schedule a task: {} (threads={}, jobs={})",
                reason,
                threads.size(),
                scheduled_jobs);
        }
        else
            return false;
    };

    {
        std::unique_lock lock(mutex);

        auto pred = [this] {
            return !queue_size || scheduled_jobs < queue_size || shutdown;
        };

        if (wait_microseconds) /// Check for optional. Condition is true if the optional is set and the value is zero.
        {
            if (!job_finished.wait_for(lock, std::chrono::microseconds(*wait_microseconds), pred))
                return on_error(fmt::format("no free thread (timeout={})", *wait_microseconds));
        }
        else
            job_finished.wait(lock, pred);

        if (shutdown)
            return on_error("shutdown");

        /// We must not to allocate any memory after we emplaced a job in a queue.
        /// Because if an exception would be thrown, we won't notify a thread about job occurrence.

        /// Check if there are enough threads to process job.
        if (threads.size() < std::min(max_threads, scheduled_jobs + 1))
        {
            try
            {
                threads.emplace_front();
            }
            catch (...)
            {
                /// Most likely this is a std::bad_alloc exception
                return on_error("cannot allocate thread slot");
            }

            try
            {
                threads.front() = Thread([this, it = threads.begin()] { worker(it); });
            }
            catch (...)
            {
                threads.pop_front();
                return on_error("cannot allocate thread");
            }
        }

        jobs.emplace(std::move(job), priority);

        ++scheduled_jobs;
    }

    new_job_or_shutdown.notify_one();

    return static_cast<ReturnType>(true);
}

template <typename Thread>
void ThreadPoolImpl<Thread>::scheduleOrThrowOnError(Job job, ssize_t priority)
{
    scheduleImpl<void>(std::move(job), priority, std::nullopt);
}

template <typename Thread>
bool ThreadPoolImpl<Thread>::trySchedule(Job job, ssize_t priority, uint64_t wait_microseconds) noexcept
{
    return scheduleImpl<bool>(std::move(job), priority, wait_microseconds);
}

template <typename Thread>
void ThreadPoolImpl<Thread>::scheduleOrThrow(
    Job job,
    ssize_t priority,
    uint64_t wait_microseconds,
    bool propagate_opentelemetry_tracing_context)
{
    scheduleImpl<void>(std::move(job), priority, wait_microseconds, propagate_opentelemetry_tracing_context);
}

template <typename Thread>
std::future<void> ThreadPoolImpl<Thread>::scheduleWithFuture(Job job, uint64_t wait_timeout_us)
{
    auto task = std::make_shared<std::packaged_task<void()>>(std::move(job));
    scheduleImpl<void>([task]() { (*task)(); }, /*priority*/ 0, wait_timeout_us);
    return task->get_future();
}

template <typename Thread>
void ThreadPoolImpl<Thread>::wait()
{
    {
        std::unique_lock lock(mutex);
        /// Signal here just in case.
        /// If threads are waiting on condition variables, but there are some jobs in the queue
        /// then it will prevent us from deadlock.
        new_job_or_shutdown.notify_all();
        job_finished.wait(lock, [this] { return scheduled_jobs == 0; });

        if (first_exception)
        {
            std::exception_ptr exception;
            std::swap(exception, first_exception);
            std::rethrow_exception(exception);
        }
    }
}

template <typename Thread>
ThreadPoolImpl<Thread>::~ThreadPoolImpl()
{
    /// Note: should not use logger from here,
    /// because it can be an instance of GlobalThreadPool that is a global variable
    /// and the destruction order of global variables is unspecified.

    finalize();
}

template <typename Thread>
void ThreadPoolImpl<Thread>::finalize()
{
    {
        std::lock_guard lock(mutex);
        shutdown = true;
    }

    new_job_or_shutdown.notify_all();

    for (auto & thread : threads)
        thread.join();

    threads.clear();
}

template <typename Thread>
size_t ThreadPoolImpl<Thread>::active() const
{
    std::lock_guard lock(mutex);
    return scheduled_jobs;
}

template <typename Thread>
bool ThreadPoolImpl<Thread>::finished() const
{
    std::lock_guard lock(mutex);
    return shutdown;
}

template <typename Thread>
void ThreadPoolImpl<Thread>::worker(typename std::list<Thread>::iterator thread_it)
{
    CurrentMetrics::Increment metric_all_threads(
        std::is_same_v<Thread, std::thread> ? CurrentMetrics::GlobalThread : CurrentMetrics::LocalThread);

    while (true)
    {
        /// This is inside the loop to also reset previous thread names set inside the jobs.
        setThreadName("ThreadPool");

        Job job;
        bool need_shutdown = false;

        /// A copy of parent trace context
        // DB::OpenTelemetry::TracingContextOnThread parent_thead_trace_context;

        {
            std::unique_lock lock(mutex);
            new_job_or_shutdown.wait(lock, [this] { return shutdown || !jobs.empty(); });
            need_shutdown = shutdown;

            if (!jobs.empty())
            {
                /// boost::priority_queue does not provide interface for getting non-const reference to an element
                /// to prevent us from modifying its priority. We have to use const_cast to force move semantics on JobWithPriority::job.
                job = std::move(const_cast<Job &>(jobs.top().job));
                // parent_thead_trace_context = std::move(const_cast<DB::OpenTelemetry::TracingContextOnThread &>(jobs.top().thread_trace_context));
                jobs.pop();
            }
            else
            {
                /// shutdown is true, simply finish the thread.
                return;
            }
        }

        if (!need_shutdown)
        {
            try
            {
                CurrentMetrics::Increment metric_active_threads(
                    std::is_same_v<Thread, std::thread> ? CurrentMetrics::GlobalThreadActive
                                                        : CurrentMetrics::LocalThreadActive);

                job();


                /// job should be reset before decrementing scheduled_jobs to
                /// ensure that the Job destroyed before wait() returns.
                job = {};
            }
            catch (...)
            {
                /// job should be reset before decrementing scheduled_jobs to
                /// ensure that the Job destroyed before wait() returns.
                job = {};

                {
                    std::lock_guard lock(mutex);
                    if (!first_exception)
                        first_exception = std::current_exception(); // NOLINT
                    if (shutdown_on_exception)
                        shutdown = true;
                    --scheduled_jobs;
                }

                job_finished.notify_all();
                new_job_or_shutdown.notify_all();
                return;
            }
        }

        {
            std::lock_guard lock(mutex);
            --scheduled_jobs;

            // If shutdown is called, let the `finalize` clear all threads
            // Otherwise reduce the allocated threads number according to the running jobs and `max_free_threads`
            if (!shutdown && threads.size() > scheduled_jobs + max_free_threads)
            {
                thread_it->detach();
                threads.erase(thread_it);
                job_finished.notify_all();
                return;
            }
        }

        job_finished.notify_all();
    }
}

template class ThreadPoolImpl<std::thread>;
template class ThreadPoolImpl<ThreadFromGlobalPoolImpl<false>>;
template class ThreadFromGlobalPoolImpl<true>;

std::unique_ptr<GlobalThreadPool> GlobalThreadPool::the_instance;

void GlobalThreadPool::initialize(size_t max_threads, size_t max_free_threads, size_t queue_size)
{
    if (the_instance)
    {
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "The global thread pool is initialized twice");
    }

    the_instance.reset(
        new GlobalThreadPool(max_threads, max_free_threads, queue_size, false /*shutdown_on_exception*/));
}

void GlobalThreadPool::registerFinalizer(std::function<void()> fn)
{
    finalize_fns.push_back(fn);
}

GlobalThreadPool & GlobalThreadPool::instance()
{
    if (!the_instance)
    {
        // Allow implicit initialization. This is needed for old code that is
        // impractical to redo now, especially Arcadia users and unit tests.
        initialize();
    }

    return *the_instance;
}

GlobalThreadPool::~GlobalThreadPool() noexcept
{
    for (auto & fn : finalize_fns)
        fn();
}

} // namespace DB
