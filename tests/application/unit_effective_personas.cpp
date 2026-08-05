#include "application/effective_personas.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

TEST(EffectivePersonas, IncludesGuestButListsOnlyCustomPersonas) {
    cha::test::TestWorkspace fixture;
    cha::WorkspaceSnapshot snapshot(cha::Workspace(fixture.root()));
    cha::EffectivePersonas personas(snapshot);
    ASSERT_NE(personas.find("guest"), nullptr);
    EXPECT_EQ(personas.find("guest")->id, "builtin-guest");
    EXPECT_EQ(personas.custom_names(), std::vector<std::string>({"Reader"}));
}
