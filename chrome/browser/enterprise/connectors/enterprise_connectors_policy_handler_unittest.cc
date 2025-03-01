// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/enterprise/connectors/enterprise_connectors_policy_handler.h"

#include <memory>
#include <tuple>

#include "base/json/json_reader.h"
#include "base/values.h"
#include "components/policy/core/browser/policy_error_map.h"
#include "components/policy/core/common/policy_map.h"
#include "components/policy/core/common/policy_types.h"
#include "components/policy/core/common/schema.h"
#include "components/prefs/pref_value_map.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace enterprise_connectors {

namespace {

const char kTestPref[] = "enterprise_connectors.test_pref";

const char kTestScopePref[] = "enterprise_connectors.scope.test_pref";

const char kPolicyName[] = "PolicyForTesting";

const char kSchema[] = R"(
      {
        "type": "object",
        "properties": {
          "PolicyForTesting": {
            "type": "array",
            "items": {
              "type": "object",
              "properties": {
                "service_provider": { "type": "string" },
                "enable": { "type": "boolean" },
              }
            }
          }
        }
      })";

constexpr char kEmptyPolicy[] = "";

constexpr char kValidPolicy[] = R"(
    [
      {
        "service_provider": "Google",
        "enable": true,
      },
      {
        "service_provider": "Alphabet",
        "enable": false,
      },
    ])";

// The enable field should be an boolean instead of a string.
constexpr char kInvalidPolicy[] = R"(
    [
      {
        "service_provider": "Google",
        "enable": "yes",
      },
      {
        "service_provider": "Alphabet",
        "enable": "no",
      },
    ])";

}  // namespace

class EnterpriseConnectorsPolicyHandlerTest
    : public testing::TestWithParam<
          std::tuple<const char*, const char*, policy::PolicySource>> {
 public:
  const char* policy_scope() const { return std::get<0>(GetParam()); }

  const char* policy() const { return std::get<1>(GetParam()); }

  policy::PolicySource source() const { return std::get<2>(GetParam()); }

  bool expect_valid_policy() const {
    if (policy() == kEmptyPolicy)
      return true;
    if (policy() == kInvalidPolicy)
      return false;
    return source() == policy::PolicySource::POLICY_SOURCE_CLOUD ||
           source() == policy::PolicySource::POLICY_SOURCE_CLOUD_FROM_ASH;
  }

  absl::optional<base::Value> policy_value() const {
    return base::JSONReader::Read(policy(), base::JSON_ALLOW_TRAILING_COMMAS);
  }
};

TEST_P(EnterpriseConnectorsPolicyHandlerTest, Test) {
  std::string error;
  policy::Schema validation_schema = policy::Schema::Parse(kSchema, &error);
  ASSERT_TRUE(error.empty());

  policy::PolicyMap policy_map;
  if (policy() != kEmptyPolicy) {
    policy_map.Set(kPolicyName, policy::PolicyLevel::POLICY_LEVEL_MANDATORY,
                   policy::PolicyScope::POLICY_SCOPE_MACHINE, source(),
                   policy_value(), nullptr);
  }

  auto handler = std::make_unique<EnterpriseConnectorsPolicyHandler>(
      kPolicyName, kTestPref, policy_scope(), validation_schema);
  policy::PolicyErrorMap errors;
  ASSERT_EQ(expect_valid_policy(),
            handler->CheckPolicySettings(policy_map, &errors));
  ASSERT_EQ(expect_valid_policy(), errors.empty());

  // Apply the pref and check it matches the policy.
  // Real code will not call ApplyPolicySettings if CheckPolicySettings returns
  // false, this is just to test that it applies the pref correctly.
  PrefValueMap prefs;
  base::Value* value_set_in_pref;
  int pref_scope = -1;
  handler->ApplyPolicySettings(policy_map, &prefs);

  bool policy_is_set = policy() != kEmptyPolicy;
  ASSERT_EQ(policy_is_set, prefs.GetValue(kTestPref, &value_set_in_pref));
  if (policy_scope())
    EXPECT_EQ(policy_is_set, prefs.GetInteger(policy_scope(), &pref_scope));

  auto* value_set_in_map = policy_map.GetValue(kPolicyName);
  if (value_set_in_map) {
    ASSERT_EQ(*value_set_in_map, *value_set_in_pref);
    if (policy_scope())
      ASSERT_EQ(policy::POLICY_SCOPE_MACHINE, pref_scope);
  } else {
    ASSERT_FALSE(policy_is_set);
    if (policy_scope())
      ASSERT_EQ(-1, pref_scope);
  }
}

INSTANTIATE_TEST_SUITE_P(
    EnterpriseConnectorsPolicyHandlerTest,
    EnterpriseConnectorsPolicyHandlerTest,
    testing::Combine(
        testing::Values(kTestScopePref, nullptr),
        testing::Values(kValidPolicy, kInvalidPolicy, kEmptyPolicy),
        testing::Values(policy::PolicySource::POLICY_SOURCE_CLOUD,
                        policy::PolicySource::POLICY_SOURCE_CLOUD_FROM_ASH,
                        policy::PolicySource::POLICY_SOURCE_ACTIVE_DIRECTORY,
                        policy::PolicySource::POLICY_SOURCE_PLATFORM)));

}  // namespace enterprise_connectors
