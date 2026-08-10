#include "pi/agent/pending_queue.h"

#include <gtest/gtest.h>

namespace pi
{
namespace
{

TEST(PendingQueueTest, OneAtATimeDrainsOldest)
{
    PendingMessageQueue queue(QueueMode::OneAtATime);
    queue.enqueue(Message::user("first"));
    queue.enqueue(Message::user("second"));
    EXPECT_TRUE(queue.has_items());

    auto drained = queue.drain();
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained[0].text_content(), "first");
    EXPECT_TRUE(queue.has_items());

    drained = queue.drain();
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained[0].text_content(), "second");
    EXPECT_FALSE(queue.has_items());
}

TEST(PendingQueueTest, AllModeDrainsEverything)
{
    PendingMessageQueue queue(QueueMode::All);
    queue.enqueue(Message::user("first"));
    queue.enqueue(Message::user("second"));
    queue.enqueue(Message::user("third"));

    auto drained = queue.drain();
    ASSERT_EQ(drained.size(), 3u);
    EXPECT_EQ(drained[0].text_content(), "first");
    EXPECT_EQ(drained[2].text_content(), "third");
    EXPECT_FALSE(queue.has_items());
}

TEST(PendingQueueTest, EmptyDrainReturnsEmpty)
{
    PendingMessageQueue queue;
    EXPECT_TRUE(queue.drain().empty());
}

TEST(PendingQueueTest, ClearRemovesMessages)
{
    PendingMessageQueue queue(QueueMode::All);
    queue.enqueue(Message::user("a"));
    queue.enqueue(Message::user("b"));
    queue.clear();
    EXPECT_FALSE(queue.has_items());
    EXPECT_TRUE(queue.drain().empty());
}

TEST(PendingQueueTest, ModeSwitchAffectsDrain)
{
    PendingMessageQueue queue(QueueMode::All);
    queue.enqueue(Message::user("a"));
    queue.enqueue(Message::user("b"));
    queue.set_mode(QueueMode::OneAtATime);
    auto drained = queue.drain();
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained[0].text_content(), "a");
}

}  // namespace
}  // namespace pi
