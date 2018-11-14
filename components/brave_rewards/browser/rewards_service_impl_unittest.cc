/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/files/scoped_temp_dir.h"
#include "brave/components/brave_rewards/browser/rewards_service_factory.h"
#include "brave/components/brave_rewards/browser/rewards_service_impl.h"
#include "brave/components/brave_rewards/browser/test_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "testing/gtest/include/gtest/gtest.h"

// npm run test -- brave_unit_tests --filter=RewardsServiceImplTest.*

//using testing::_;
//using testing::AtLeast;
using namespace brave_rewards;

class RewardsServiceTest : public testing::Test {
 public:
  RewardsServiceTest() :
      thread_bundle_(content::TestBrowserThreadBundle::IO_MAINLOOP) {
  }
  ~RewardsServiceTest() override {}

 protected:
  void SetUp() override {
    EXPECT_TRUE(temp_dir_.CreateUniqueTempDir());
    // register the factory

    profile_ = CreateBraveRewardsProfile(temp_dir_.GetPath());
    EXPECT_TRUE(profile_.get() != NULL);


    rewards_service_ = static_cast<RewardsServiceImpl*>(
        RewardsServiceFactory::GetInstance()->GetForProfile(profile()));

    EXPECT_TRUE(rewards_service_ != NULL);
  }

  void TearDown() override {
    profile_.reset();
  }


  Profile* profile() { return profile_.get(); }
  RewardsServiceImpl* rewards_service() { return rewards_service_; }

 private:
  std::unique_ptr<Profile> profile_;
  content::TestBrowserThreadBundle thread_bundle_;
  RewardsServiceImpl* rewards_service_;
  base::ScopedTempDir temp_dir_;
};


TEST_F(RewardsServiceTest, HandleFlags) {

}