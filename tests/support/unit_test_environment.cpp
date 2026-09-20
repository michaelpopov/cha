#include "util/environment.h"

#include <gtest/gtest.h>

namespace {

class UnitTestEnvironment : public testing::Environment {
public:
    void SetUp() override {
        // Empty test vaults must not import the developer's R2 credentials.
        // Migration tests set their own values after this setup. The live
        // integration-test binary deliberately does not use this environment.
        for (const char* name : {
                 "CHA_R2_URL", "CHA_R2_ACCESS_KEY_ID", "CHA_R2_SECRET_ACCESS_KEY"}) {
            ASSERT_TRUE(cha::unset_environment_variable(name)) << name;
        }
    }
};

[[maybe_unused]] const auto* environment =
    testing::AddGlobalTestEnvironment(new UnitTestEnvironment);

} // namespace
