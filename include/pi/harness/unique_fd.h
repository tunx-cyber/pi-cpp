#pragma once

#include <unistd.h>

#include <utility>

namespace pi
{

/** Exclusive ownership of a POSIX file descriptor. */
class UniqueFd
{
   public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) reset(std::exchange(other.fd_, -1));
        return *this;
    }
    int get() const noexcept { return fd_; }
    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

   private:
    int fd_;
};

}  // namespace pi
