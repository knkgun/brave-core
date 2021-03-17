/* Copyright (c) 2021 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bat/ledger/internal/core/bat_ledger_context.h"

#include <string>

#include "bat/ledger/internal/core/bat_ledger_test.h"
#include "bat/ledger/internal/core/future.h"

namespace ledger {

class BATLedgerContextTest : public BATLedgerTest {};

struct TestSettings {
  std::string a;
  int b;

  static const TestSettings kProduction;
  static const TestSettings kStaging;
  static const TestSettings kDevelopment;
};

const TestSettings TestSettings::kProduction = {.a = "prod", .b = 1};
const TestSettings TestSettings::kStaging = {.a = "staging", .b = 2};
const TestSettings TestSettings::kDevelopment = {.a = "dev", .b = 3};

TEST_F(BATLedgerContextTest, GetSettings) {
  struct ResetEnv {
    ResetEnv() : env(ledger::_environment) {}
    ~ResetEnv() { ledger::_environment = env; }
    mojom::Environment env;
  } reset_env;

  ledger::_environment = mojom::Environment::PRODUCTION;
  auto& prod = context().GetSettings<TestSettings>();
  ASSERT_EQ(prod.a, "prod");
  ASSERT_EQ(prod.b, 1);

  ledger::_environment = mojom::Environment::STAGING;
  auto& staging = context().GetSettings<TestSettings>();
  ASSERT_EQ(staging.a, "staging");
  ASSERT_EQ(staging.b, 2);

  ledger::_environment = mojom::Environment::DEVELOPMENT;
  auto& dev = context().GetSettings<TestSettings>();
  ASSERT_EQ(dev.a, "dev");
  ASSERT_EQ(dev.b, 3);
}

class TestComponent : public BATLedgerContext::Object {
 public:
  static const char kContextKey[];

  TestComponent() = default;
  ~TestComponent() override = default;

  BATLedgerContext& GetContext() { return context(); }
};

const char TestComponent::kContextKey[] = "test-component";

TEST_F(BATLedgerContextTest, GetComponent) {
  BATLedgerContext context(GetTestLedgerClient());
  auto& component = context.Get<TestComponent>();
  EXPECT_EQ(&component.GetContext(), &context);
  EXPECT_EQ(&context.Get<TestComponent>(), &component);
}

TEST_F(BATLedgerContextTest, StartJob) {
  class Job : public BATLedgerContext::Object {
   public:
    Future<int> GetFuture() { return std::move(future_pair_.future); }
    void Start(int n) { future_pair_.resolver.Complete(n); }

   private:
    FuturePair<int> future_pair_;
  };

  int value = 0;
  context().StartJob<Job>(100).Then(
      base::BindLambdaForTesting([&value](int v) { value = v; }));

  task_environment()->RunUntilIdle();
  EXPECT_EQ(value, 100);
}

}  // namespace ledger
