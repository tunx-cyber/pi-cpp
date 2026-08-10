#pragma once

#include <deque>
#include <mutex>
#include <vector>

#include "pi/agent/types.h"

namespace pi
{

/** 待注入消息队列，镜像 pi 的 PendingMessageQueue。 */
class PendingMessageQueue
{
   public:
    explicit PendingMessageQueue(QueueMode mode = QueueMode::OneAtATime) : mode_(mode) {}

    void enqueue(AgentMessage message)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back(std::move(message));
    }

    bool has_items() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !messages_.empty();
    }

    std::vector<AgentMessage> drain()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mode_ == QueueMode::All)
        {
            std::vector<AgentMessage> drained(messages_.begin(), messages_.end());
            messages_.clear();
            return drained;
        }
        std::vector<AgentMessage> drained;
        if (!messages_.empty())
        {
            drained.push_back(std::move(messages_.front()));
            messages_.pop_front();
        }
        return drained;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
    }

    void set_mode(QueueMode mode)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = mode;
    }

    QueueMode mode()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return mode_;
    }

   private:
    mutable std::mutex mutex_;
    std::deque<AgentMessage> messages_;
    QueueMode mode_;
};

}  // namespace pi
