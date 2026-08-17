#include "support/test_workspace.h"
#include "session/not_found_error.h"
#include "workspace/builtins.h"
#include "workspace/workspace_runtime.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace cha {
namespace {

TemporarySessionSeed welcome_seed() {
    return {{std::string(entrance_id), std::string(welcome_id)}, std::string(welcome_name)};
}

void add_writer_forum(const test::TestWorkspace& fixture) {
    fixture.add_character("writer", "Writer");
    fixture.add_forum("writers", "Writers", "writer");
}

TEST(WorkspaceRuntime, PublishesACompleteReplacementGeneration) {
    test::TestWorkspace fixture;
    WorkspaceRuntime runtime(fixture.root(), welcome_seed());
    const auto before = runtime.snapshot();

    fixture.add_persona("writer_persona", "Writer Persona");
    add_writer_forum(fixture);
    runtime.reload();

    const auto after = runtime.snapshot();
    EXPECT_NE(after, before);
    EXPECT_NE(after->model->find_character("writer"), nullptr);
    EXPECT_NE(after->model->find_persona("writer_persona"), nullptr);
    EXPECT_NE(after->model->find_forum("writers"), nullptr);
    EXPECT_NO_THROW((void)after->sessions->create("writers", "Draft"));

    // The prior generation remains a valid immutable view while a request or
    // live session still retains it.
    EXPECT_EQ(before->model->find_character("writer"), nullptr);
    EXPECT_THROW((void)before->sessions->list("writers"), ForumNotFoundError);
}

TEST(WorkspaceRuntime, KeepsTheCurrentGenerationWhenReloadValidationFails) {
    test::TestWorkspace fixture;
    WorkspaceRuntime runtime(fixture.root(), welcome_seed());
    const auto before = runtime.snapshot();

    fixture.write_character_config("display_name =\n");
    EXPECT_THROW(runtime.reload(), std::exception);

    const auto after = runtime.snapshot();
    EXPECT_EQ(after, before);
    EXPECT_NE(after->model->find_character("guide"), nullptr);
}

} // namespace
} // namespace cha
