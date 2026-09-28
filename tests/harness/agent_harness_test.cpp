#include "pi/harness/agent_harness.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

#include "pi/harness/env.h"
#include "pi/harness/jsonl_repo.h"
#include "test_utils/scripted_transport.h"

namespace pi
{
namespace
{
class FailingFileSystem : public PosixFileSystem
{
   public:
    using PosixFileSystem::PosixFileSystem;
    bool fail_writes = false;
    Result<void, FileError> append_file(const std::string& path,
                                        const std::string& content) override
    {
        if (fail_writes)
            return Result<void, FileError>::err_value({FileErrorCode::Unknown, "disk full", path});
        return PosixFileSystem::append_file(path, content);
    }
};

class AgentHarnessTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        char path[] = "/tmp/pi_harness_test_XXXXXX";
        const auto* created = mkdtemp(path);
        ASSERT_NE(created, nullptr);
        root = created;
        fs = std::make_unique<FailingFileSystem>(root);
        JsonlSessionRepo repo(*fs, root);
        const auto session = repo.create(root);
        ASSERT_TRUE(session.ok);
        AgentHarnessOptions options;
        options.session = session.value;
        options.model = pi_test::scripted_model();
        options.compactionSettings.enabled = false;
        options.transport = std::make_shared<pi_test::ScriptedTransport>();
        harness = std::make_unique<AgentHarness>(std::move(options));
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    std::string root;
    std::unique_ptr<FailingFileSystem> fs;
    std::unique_ptr<AgentHarness> harness;
};

TEST_F(AgentHarnessTest, MessageWriteFailureIsVisibleAndAgentBecomesIdle)
{
    fs->fail_writes = true;
    EXPECT_THROW(harness->prompt("hello"), std::runtime_error);
    EXPECT_FALSE(harness->is_busy());
    harness->wait_for_idle();
    fs->fail_writes = false;
    EXPECT_NO_THROW(harness->prompt("retry"));
}

TEST_F(AgentHarnessTest, FailedConfigurationWriteDoesNotChangeRuntimeConfiguration)
{
    const auto previous_model = harness->model();
    auto next_model = previous_model;
    next_model.id = "replacement";
    fs->fail_writes = true;
    EXPECT_THROW(harness->set_model(next_model), std::runtime_error);
    EXPECT_EQ(harness->model().id, previous_model.id);
    EXPECT_THROW(harness->set_thinking_level(ThinkingLevel::High), std::runtime_error);
    EXPECT_EQ(harness->thinking_level(), ThinkingLevel::Off);
    AgentTool tool;
    tool.name = "test";
    EXPECT_THROW(harness->set_tools({tool}), std::runtime_error);
    EXPECT_TRUE(harness->tools().empty());
}
}  // namespace
}  // namespace pi
