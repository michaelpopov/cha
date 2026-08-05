#include "application/chat_application.h"
#include "support/test_notifier.h"
#include "support/test_workspace.h"
#include "ui/text/application_dispatcher.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(ApplicationDispatcher, HelpIsHandledWithoutEnteringTheTranscript) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::NoopNotifier notifier;
    ChatApplication application(workspace, notifier);
    ApplicationDispatcher dispatcher(application);

    const ApplicationResult result = dispatcher.handle("/help");
    ASSERT_TRUE(result.list);
    EXPECT_TRUE(result.input_consumed);
    EXPECT_EQ(application.controller().transcript().entries().size(), 0U);
    EXPECT_EQ(result.list->rows.size(), 17U);
}

TEST(ApplicationDispatcher, HelpRemainsAvailableDuringGeneration) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::NoopNotifier notifier;
    ChatApplication application(workspace, notifier);
    ApplicationDispatcher dispatcher(application);
    (void)application.controller().submit_prompt(
        application.selected_author_key(), "Keep working");
    ASSERT_TRUE(application.controller().is_generating());
    const std::size_t transcript_size =
        application.controller().transcript().entries().size();

    const ApplicationResult result = dispatcher.handle("/help");
    ASSERT_TRUE(result.list);
    EXPECT_TRUE(result.input_consumed);
    EXPECT_EQ(result.list->rows.size(), 17U);
    EXPECT_TRUE(application.controller().is_generating());
    EXPECT_EQ(application.controller().transcript().entries().size(),
        transcript_size);
}

TEST(ApplicationDispatcher, RoutesApplicationCommandsThroughTheCoordinator) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::NoopNotifier notifier;
    ChatApplication application(workspace, notifier);
    ApplicationDispatcher dispatcher(application);

    const ApplicationResult result = dispatcher.handle("/iam reader");
    EXPECT_TRUE(result.persona_changed);
    EXPECT_EQ(application.selected_persona(), "Reader");
}

} // namespace
} // namespace cha
