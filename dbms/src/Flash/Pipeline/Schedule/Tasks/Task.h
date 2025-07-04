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

#pragma once

#include <Common/Logger.h>
#include <Common/MemoryTracker.h>
#include <Flash/Executor/PipelineExecutorContext.h>
#include <Flash/Pipeline/Schedule/Tasks/TaskHelper.h>
#include <Flash/Pipeline/Schedule/Tasks/TaskProfileInfo.h>
#include <Flash/Pipeline/Schedule/Tasks/TaskTimer.h>
#include <memory.h>

namespace DB
{
/**
 *           CANCELLED/ERROR/FINISHED
 *                      ▲
 *                      │
 * ┌───────────────────────────────────────────────┐
 * │     ┌──────────►RUNNING◄──────────┐           │
 * │     │                             │           │
 * │     ▼                             ▼           │
 * │ WATITING/WAIT_FOR_NOTIFY◄────────►IO_IN/OUT   │
 * └───────────────────────────────────────────────┘
 */
enum class ExecTaskStatus
{
    WAIT_FOR_NOTIFY,
    WAITING,
    RUNNING,
    IO_IN,
    IO_OUT,
    FINISHED,
    ERROR,
    CANCELLED,
};

enum class NotifyType
{
    WAIT_ON_TABLE_SCAN_READ,
    WAIT_ON_SHARED_QUEUE_WRITE,
    WAIT_ON_SHARED_QUEUE_READ,
    WAIT_ON_SPILL_BUCKET_READ,
    WAIT_ON_GRPC_RECV_READ,
    WAIT_ON_TUNNEL_SENDER_WRITE,
    WAIT_ON_JOIN_BUILD_FINISH,
    WAIT_ON_JOIN_PROBE_FINISH,
    WAIT_ON_RESULT_QUEUE_WRITE,
    WAIT_ON_NOTHING,
};

class PipelineExecutorContext;

class Task
{
public:
    Task(
        PipelineExecutorContext & exec_context_,
        const String & req_id,
        ExecTaskStatus init_status = ExecTaskStatus::RUNNING);

    // Only used for unit test.
    explicit Task(PipelineExecutorContext & exec_context_);

    virtual ~Task();

    ExecTaskStatus getStatus() const { return task_status; }

    ExecTaskStatus execute();

    ExecTaskStatus executeIO();

    ExecTaskStatus await();

    void notify();

    // `finalize` must be called before destructuring.
    // `TaskHelper::FINALIZE_TASK` can help this.
    void finalize();

    ALWAYS_INLINE void beforeExec(TaskTimer * timer)
    {
        assert(nullptr == current_task_timer);
        current_task_timer = timer;
        startTraceMemory();
    }
    ALWAYS_INLINE static void afterExec()
    {
        current_task_timer = nullptr;
        endTraceMemory();
    }
    ALWAYS_INLINE void startTraceMemory()
    {
        assert(nullptr == current_memory_tracker);
        assert(0 == CurrentMemoryTracker::getLocalDeltaMemory());
        current_memory_tracker = mem_tracker_ptr;
    }
    ALWAYS_INLINE static void endTraceMemory()
    {
        CurrentMemoryTracker::submitLocalDeltaMemory();
        current_memory_tracker = nullptr;
    }

    const String & getQueryId() const;

    const String & getResourceGroupName() const;

    const KeyspaceID & getKeyspaceID() const;

    const PipelineExecutorContext & getQueryExecContext() { return exec_context; }

    void onErrorOccurred(const String & err_msg) { exec_context.onErrorOccurred(err_msg); }

    void setNotifyType(NotifyType type) { notify_type = type; }
    NotifyType getNotifyType() const { return notify_type; }

public:
    LoggerPtr log;

protected:
    virtual ExecTaskStatus executeImpl() = 0;
    virtual ExecTaskStatus executeIOImpl() { return ExecTaskStatus::RUNNING; }
    // Avoid allocating memory in `await` if possible.
    virtual ExecTaskStatus awaitImpl() { return ExecTaskStatus::RUNNING; }

    virtual ExecTaskStatus notifyImpl() { return ExecTaskStatus::RUNNING; }

    // Used to release held resources, just like `Event::finishImpl`.
    virtual void finalizeImpl() {}

private:
    inline void switchStatus(ExecTaskStatus to);

public:
    TaskProfileInfo profile_info;

    // level of multi-level feedback queue.
    size_t mlfq_level{0};

private:
    PipelineExecutorContext & exec_context;

    // To ensure that the memory tracker will not be destructed prematurely and prevent crashes due to accessing invalid memory tracker pointers.
    MemoryTrackerPtr mem_tracker_holder;
    // To reduce the overheads of `mem_tracker_holder.get()`
    MemoryTracker * mem_tracker_ptr;

    ExecTaskStatus task_status;
    NotifyType notify_type{NotifyType::WAIT_ON_NOTHING};

    bool is_finalized = false;
};
using TaskPtr = std::unique_ptr<Task>;
} // namespace DB
