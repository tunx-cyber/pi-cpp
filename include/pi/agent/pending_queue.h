#pragma once

#include <deque>
#include <mutex>
#include <vector>

#include "pi/agent/types.h"

namespace pi
{

/** 待注入消息队列（steering）：每次 drain 取一条，保持注入时机可预测。 */
class PendingMessageQueue
{
   public:
    PendingMessageQueue() = default;

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

   private:
    mutable std::mutex mutex_;
    std::deque<AgentMessage> messages_;
};

}  // namespace pi
