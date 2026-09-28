#pragma once

#include <utility>

namespace pi
{

/** Runs a non-throwing cleanup exactly once, including during stack unwinding. */
template <typename F>
class ScopeExit
{
   public:
    explicit ScopeExit(F cleanup) : cleanup_(std::move(cleanup)) {}
    ~ScopeExit() noexcept { cleanup_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

   private:
    F cleanup_;
};

}  // namespace pi
