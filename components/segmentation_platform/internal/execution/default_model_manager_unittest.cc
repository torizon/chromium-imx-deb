// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/segmentation_platform/internal/execution/default_model_manager.h"

#include <memory>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/test/gmock_callback_support.h"
#include "base/test/task_environment.h"
#include "components/segmentation_platform/internal/database/test_segment_info_database.h"
#include "components/segmentation_platform/internal/execution/mock_model_provider.h"
#include "components/segmentation_platform/public/model_provider.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

using base::test::RunOnceCallback;
using optimization_guide::proto::OptimizationTarget;
using testing::_;

namespace segmentation_platform {

class DefaultModelManagerTest : public testing::Test {
 public:
  DefaultModelManagerTest() : model_provider_factory_(&model_provider_data_) {}
  ~DefaultModelManagerTest() override = default;

  MockModelProvider& FindHandler(
      optimization_guide::proto::OptimizationTarget segment_id) {
    return *(*model_provider_data_.default_model_providers.find(segment_id))
                .second;
  }

  void OnGetAllSegments(
      std::unique_ptr<SegmentInfoDatabase::SegmentInfoList> entries) {
    get_all_segment_result_.swap(entries);
  }

  const SegmentInfoDatabase::SegmentInfoList& get_all_segment_result() const {
    return *get_all_segment_result_;
  }

  base::test::TaskEnvironment task_environment_;
  test::TestSegmentInfoDatabase segment_database_;
  TestModelProviderFactory::Data model_provider_data_;
  TestModelProviderFactory model_provider_factory_;
  std::unique_ptr<DefaultModelManager> default_model_manager_;
  std::unique_ptr<SegmentInfoDatabase::SegmentInfoList> get_all_segment_result_;
  base::WeakPtrFactory<DefaultModelManagerTest> weak_ptr_factory_{this};
};

TEST_F(DefaultModelManagerTest, BasicTest) {
  const auto segment_1 =
      OptimizationTarget::OPTIMIZATION_TARGET_SEGMENTATION_NEW_TAB;
  const auto segment_2 =
      OptimizationTarget::OPTIMIZATION_TARGET_SEGMENTATION_SHARE;
  const auto segment_3 =
      OptimizationTarget::OPTIMIZATION_TARGET_SEGMENTATION_VOICE;
  const auto segment_4 =
      OptimizationTarget::OPTIMIZATION_TARGET_SEGMENTATION_QUERY_TILES;

  // Set some model versions.
  const int model_version_db = 4;
  const int model_version_default = 5;

  // Initialize DB and default models with 1 and 2 segments respectively.
  model_provider_data_.segments_supporting_default_model = {segment_1,
                                                            segment_2};
  default_model_manager_ = std::make_unique<DefaultModelManager>(
      &model_provider_factory_,
      model_provider_data_.segments_supporting_default_model);

  // Set up models 1 and 3 in DB.
  proto::SegmentInfo* segment_1_from_db =
      segment_database_.FindOrCreateSegment(segment_1);
  segment_1_from_db->set_model_version(model_version_db);
  proto::SegmentInfo* segment_3_from_db =
      segment_database_.FindOrCreateSegment(segment_3);
  segment_3_from_db->set_model_version(model_version_db);

  // Set up default models 1 and 2.
  proto::SegmentationModelMetadata metadata_1;
  EXPECT_CALL(FindHandler(segment_1), InitAndFetchModel(_))
      .WillOnce(
          RunOnceCallback<0>(segment_1, metadata_1, model_version_default));
  proto::SegmentationModelMetadata metadata_2;
  EXPECT_CALL(FindHandler(segment_2), InitAndFetchModel(_))
      .WillOnce(
          RunOnceCallback<0>(segment_2, metadata_2, model_version_default));

  // Query models.
  default_model_manager_->GetAllSegmentInfoFromBothModels(
      {segment_1, segment_2}, &segment_database_,
      base::BindOnce(&DefaultModelManagerTest::OnGetAllSegments,
                     weak_ptr_factory_.GetWeakPtr()));
  task_environment_.RunUntilIdle();

  // Verify that model exists from both sources in order: segment_1 from db,
  // segment_1 from model, segment_2 from model.
  EXPECT_EQ(3u, get_all_segment_result().size());
  EXPECT_EQ(segment_1, get_all_segment_result()[0].first);
  EXPECT_EQ(model_version_db,
            get_all_segment_result()[0].second.model_version());
  EXPECT_EQ(segment_1, get_all_segment_result()[1].first);
  EXPECT_EQ(model_version_default,
            get_all_segment_result()[1].second.model_version());
  EXPECT_EQ(segment_2, get_all_segment_result()[2].first);
  EXPECT_EQ(model_version_default,
            get_all_segment_result()[2].second.model_version());

  // Query again, this time with a segment ID that doesn't exist in either
  // sources.
  default_model_manager_->GetAllSegmentInfoFromBothModels(
      {segment_4}, &segment_database_,
      base::BindOnce(&DefaultModelManagerTest::OnGetAllSegments,
                     weak_ptr_factory_.GetWeakPtr()));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0u, get_all_segment_result().size());

  // Query for a model only available in the default model.
  EXPECT_CALL(FindHandler(segment_2), InitAndFetchModel(_))
      .WillOnce(
          RunOnceCallback<0>(segment_2, metadata_2, model_version_default));
  default_model_manager_->GetAllSegmentInfoFromBothModels(
      {segment_2}, &segment_database_,
      base::BindOnce(&DefaultModelManagerTest::OnGetAllSegments,
                     weak_ptr_factory_.GetWeakPtr()));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(1u, get_all_segment_result().size());
  EXPECT_EQ(segment_2, get_all_segment_result()[0].first);

  // Query for a model only available in the database.
  default_model_manager_->GetAllSegmentInfoFromBothModels(
      {segment_3}, &segment_database_,
      base::BindOnce(&DefaultModelManagerTest::OnGetAllSegments,
                     weak_ptr_factory_.GetWeakPtr()));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(1u, get_all_segment_result().size());
  EXPECT_EQ(segment_3, get_all_segment_result()[0].first);
}

}  // namespace segmentation_platform
