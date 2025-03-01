// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/browsing_topics/browsing_topics_service_impl.h"

#include <random>

#include "base/rand_util.h"
#include "components/browsing_topics/browsing_topics_calculator.h"
#include "components/browsing_topics/browsing_topics_page_load_data_tracker.h"
#include "components/browsing_topics/util.h"
#include "components/optimization_guide/content/browser/page_content_annotations_service.h"
#include "content/public/browser/browsing_topics_site_data_manager.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/mojom/browsing_topics/browsing_topics.mojom.h"

namespace browsing_topics {

namespace {

// Returns whether the topics should all be cleared given
// `browsing_topics_data_accessible_since` and `is_topic_allowed_by_settings`.
// Returns true if `browsing_topics_data_accessible_since` is greater than the
// last calculation time, or if any top topic is disallowed from the settings.
// The latter could happen if the topic became disallowed when
// `browsing_topics_state` was still loading (and we didn't get a chance to
// clear it). This is an unlikely edge case, so it's fine to over-delete.
bool ShouldClearTopicsOnStartup(
    const BrowsingTopicsState& browsing_topics_state,
    base::Time browsing_topics_data_accessible_since,
    base::RepeatingCallback<bool(const privacy_sandbox::CanonicalTopic&)>
        is_topic_allowed_by_settings) {
  DCHECK(!is_topic_allowed_by_settings.is_null());

  if (browsing_topics_state.epochs().empty())
    return false;

  // Here we rely on the fact that `browsing_topics_data_accessible_since` can
  // only be updated to base::Time::Now() due to data deletion. So we'll either
  // need to clear all topics data, or no-op. If this assumption no longer
  // holds, we'd need to iterate over all epochs, check their calculation time,
  // and selectively delete the epochs.
  if (browsing_topics_data_accessible_since >
      browsing_topics_state.epochs().back().calculation_time()) {
    return true;
  }

  for (const EpochTopics& epoch : browsing_topics_state.epochs()) {
    for (const TopicAndDomains& topic_and_domains :
         epoch.top_topics_and_observing_domains()) {
      if (!topic_and_domains.IsValid())
        continue;

      if (!is_topic_allowed_by_settings.Run(privacy_sandbox::CanonicalTopic(
              topic_and_domains.topic(), epoch.taxonomy_version()))) {
        return true;
      }
    }
  }

  return false;
}

struct StartupCalculateDecision {
  bool clear_topics_data = true;
  base::TimeDelta next_calculation_delay;
};

StartupCalculateDecision GetStartupCalculationDecision(
    const BrowsingTopicsState& browsing_topics_state,
    base::Time browsing_topics_data_accessible_since,
    base::RepeatingCallback<bool(const privacy_sandbox::CanonicalTopic&)>
        is_topic_allowed_by_settings) {
  // The topics have never been calculated. This could happen with a fresh
  // profile or the if the config has updated. In case of a config update, the
  // topics should have already been cleared when initializing the
  // `BrowsingTopicsState`.
  if (browsing_topics_state.next_scheduled_calculation_time().is_null()) {
    return StartupCalculateDecision{
        .clear_topics_data = false,
        .next_calculation_delay = base::TimeDelta()};
  }

  // This could happen when clear-on-exit is turned on and has caused the
  // cookies to be deleted on startup, of if a topic became disallowed when
  // `browsing_topics_state` was still loading.
  bool should_clear_topics_data = ShouldClearTopicsOnStartup(
      browsing_topics_state, browsing_topics_data_accessible_since,
      is_topic_allowed_by_settings);

  base::TimeDelta presumed_next_calculation_delay =
      browsing_topics_state.next_scheduled_calculation_time() -
      base::Time::Now();

  // The scheduled calculation time was reached before the startup.
  if (presumed_next_calculation_delay <= base::TimeDelta()) {
    return StartupCalculateDecision{
        .clear_topics_data = should_clear_topics_data,
        .next_calculation_delay = base::TimeDelta()};
  }

  // This could happen if the machine time has changed since the last
  // calculation. Recalculate immediately to align with the expected schedule
  // rather than potentially stop computing for a very long time.
  if (presumed_next_calculation_delay >=
      2 * blink::features::kBrowsingTopicsTimePeriodPerEpoch.Get()) {
    return StartupCalculateDecision{
        .clear_topics_data = should_clear_topics_data,
        .next_calculation_delay = base::TimeDelta()};
  }

  return StartupCalculateDecision{
      .clear_topics_data = should_clear_topics_data,
      .next_calculation_delay = presumed_next_calculation_delay};
}

}  // namespace

BrowsingTopicsServiceImpl::~BrowsingTopicsServiceImpl() = default;

BrowsingTopicsServiceImpl::BrowsingTopicsServiceImpl(
    const base::FilePath& profile_path,
    privacy_sandbox::PrivacySandboxSettings* privacy_sandbox_settings,
    history::HistoryService* history_service,
    content::BrowsingTopicsSiteDataManager* site_data_manager,
    optimization_guide::PageContentAnnotationsService* annotations_service)
    : privacy_sandbox_settings_(privacy_sandbox_settings),
      history_service_(history_service),
      site_data_manager_(site_data_manager),
      annotations_service_(annotations_service),
      browsing_topics_state_(
          profile_path,
          base::BindOnce(
              &BrowsingTopicsServiceImpl::OnBrowsingTopicsStateLoaded,
              base::Unretained(this))) {
  privacy_sandbox_settings_observation_.Observe(privacy_sandbox_settings);
  history_service_observation_.Observe(history_service);

  // Greedily request the model to be available to reduce the latency in later
  // topics calculation.
  annotations_service_->RequestAndNotifyWhenModelAvailable(
      optimization_guide::AnnotationType::kPageTopics, base::DoNothing());
}

std::vector<blink::mojom::EpochTopicPtr>
BrowsingTopicsServiceImpl::GetBrowsingTopicsForJsApi(
    const url::Origin& context_origin,
    content::RenderFrameHost* main_frame) {
  if (!browsing_topics_state_loaded_)
    return {};

  if (!privacy_sandbox_settings_->IsTopicsAllowed())
    return {};

  if (!privacy_sandbox_settings_->IsTopicsAllowedForContext(
          context_origin.GetURL(), main_frame->GetLastCommittedOrigin())) {
    return {};
  }

  std::string context_domain =
      net::registry_controlled_domains::GetDomainAndRegistry(
          context_origin.GetURL(),
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);

  HashedDomain hashed_context_domain = HashContextDomainForStorage(
      browsing_topics_state_.hmac_key(), context_domain);

  // Track the API usage context after the permissions check.
  BrowsingTopicsPageLoadDataTracker::GetOrCreateForPage(main_frame->GetPage())
      ->OnBrowsingTopicsApiUsed(hashed_context_domain, history_service_);

  std::string top_domain =
      net::registry_controlled_domains::GetDomainAndRegistry(
          main_frame->GetLastCommittedOrigin().GetURL(),
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);

  std::vector<blink::mojom::EpochTopicPtr> result_topics;
  for (const EpochTopics* epoch :
       browsing_topics_state_.EpochsForSite(top_domain)) {
    absl::optional<Topic> topic = epoch->TopicForSite(
        top_domain, hashed_context_domain, browsing_topics_state_.hmac_key());

    // Only add a non-empty topic to the result.
    if (!topic)
      continue;

    // Although a top topic can never be in the disallowed state, the returned
    // `topic` may be the random one. Thus we still need this check.
    if (!privacy_sandbox_settings_->IsTopicAllowed(
            privacy_sandbox::CanonicalTopic(*topic,
                                            epoch->taxonomy_version()))) {
      continue;
    }

    blink::mojom::EpochTopicPtr result_topic = blink::mojom::EpochTopic::New();
    result_topic->topic = topic.value().value();
    result_topic->config_version = base::StrCat(
        {"chrome.", base::NumberToString(
                        blink::features::kBrowsingTopicsConfigVersion.Get())});
    result_topic->model_version = base::NumberToString(epoch->model_version());
    result_topic->taxonomy_version =
        base::NumberToString(epoch->taxonomy_version());
    result_topic->version = base::StrCat({result_topic->config_version, ":",
                                          result_topic->taxonomy_version, ":",
                                          result_topic->model_version});
    result_topics.push_back(std::move(result_topic));
  }

  // Remove duplicate entries.
  std::sort(result_topics.begin(), result_topics.end());
  result_topics.erase(std::unique(result_topics.begin(), result_topics.end()),
                      result_topics.end());

  // Shuffle the entries.
  base::RandomShuffle(result_topics.begin(), result_topics.end());

  return result_topics;
}

std::vector<privacy_sandbox::CanonicalTopic>
BrowsingTopicsServiceImpl::GetTopicsForSiteForDisplay(
    const url::Origin& top_origin) const {
  if (!browsing_topics_state_loaded_)
    return {};

  std::string top_domain =
      net::registry_controlled_domains::GetDomainAndRegistry(
          top_origin.GetURL(),
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);

  std::vector<privacy_sandbox::CanonicalTopic> result;

  for (const EpochTopics* epoch :
       browsing_topics_state_.EpochsForSite(top_domain)) {
    absl::optional<Topic> topic = epoch->TopicForSiteForDisplay(
        top_domain, browsing_topics_state_.hmac_key());

    if (!topic)
      continue;

    // `epoch->TopicForSiteForDisplay()` shall only return a top topic, and a
    // top topic can never be in the disallowed state (i.e. it will be cleared
    // when it becomes diallowed).
    DCHECK(privacy_sandbox_settings_->IsTopicAllowed(
        privacy_sandbox::CanonicalTopic(*topic, epoch->taxonomy_version())));

    result.emplace_back(*topic, epoch->taxonomy_version());
  }

  return result;
}

std::vector<privacy_sandbox::CanonicalTopic>
BrowsingTopicsServiceImpl::GetTopTopicsForDisplay() const {
  if (!browsing_topics_state_loaded_)
    return {};

  std::vector<privacy_sandbox::CanonicalTopic> result;

  for (const EpochTopics& epoch : browsing_topics_state_.epochs()) {
    DCHECK_LE(epoch.padded_top_topics_start_index(),
              epoch.top_topics_and_observing_domains().size());

    for (size_t i = 0; i < epoch.padded_top_topics_start_index(); ++i) {
      const TopicAndDomains& topic_and_domains =
          epoch.top_topics_and_observing_domains()[i];

      if (!topic_and_domains.IsValid())
        continue;

      // A top topic can never be in the disallowed state (i.e. it will be
      // cleared when it becomes diallowed).
      DCHECK(privacy_sandbox_settings_->IsTopicAllowed(
          privacy_sandbox::CanonicalTopic(topic_and_domains.topic(),
                                          epoch.taxonomy_version())));

      result.emplace_back(topic_and_domains.topic(), epoch.taxonomy_version());
    }
  }

  return result;
}

void BrowsingTopicsServiceImpl::ClearTopic(
    const privacy_sandbox::CanonicalTopic& canonical_topic) {
  if (!browsing_topics_state_loaded_)
    return;

  browsing_topics_state_.ClearTopic(canonical_topic.topic_id(),
                                    canonical_topic.taxonomy_version());
}

void BrowsingTopicsServiceImpl::ClearTopicsDataForOrigin(
    const url::Origin& origin) {
  if (!browsing_topics_state_loaded_)
    return;

  std::string context_domain =
      net::registry_controlled_domains::GetDomainAndRegistry(
          origin.GetURL(),
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);

  HashedDomain hashed_context_domain = HashContextDomainForStorage(
      browsing_topics_state_.hmac_key(), context_domain);

  browsing_topics_state_.ClearContextDomain(hashed_context_domain);
  site_data_manager_->ClearContextDomain(hashed_context_domain);
}

void BrowsingTopicsServiceImpl::ClearAllTopicsData() {
  if (!browsing_topics_state_loaded_)
    return;

  browsing_topics_state_.ClearAllTopics();
  site_data_manager_->ExpireDataBefore(base::Time::Now());
}

std::unique_ptr<BrowsingTopicsCalculator>
BrowsingTopicsServiceImpl::CreateCalculator(
    privacy_sandbox::PrivacySandboxSettings* privacy_sandbox_settings,
    history::HistoryService* history_service,
    content::BrowsingTopicsSiteDataManager* site_data_manager,
    optimization_guide::PageContentAnnotationsService* annotations_service,
    BrowsingTopicsCalculator::CalculateCompletedCallback callback) {
  return std::make_unique<BrowsingTopicsCalculator>(
      privacy_sandbox_settings, history_service, site_data_manager,
      annotations_service, std::move(callback));
}

const BrowsingTopicsState& BrowsingTopicsServiceImpl::browsing_topics_state() {
  return browsing_topics_state_;
}

void BrowsingTopicsServiceImpl::ScheduleBrowsingTopicsCalculation(
    base::TimeDelta delay) {
  DCHECK(browsing_topics_state_loaded_);

  // `this` owns the timer, which is automatically cancelled on destruction, so
  // base::Unretained(this) is safe.
  schedule_calculate_timer_.Start(
      FROM_HERE, delay,
      base::BindOnce(&BrowsingTopicsServiceImpl::CalculateBrowsingTopics,
                     base::Unretained(this)));
}

void BrowsingTopicsServiceImpl::CalculateBrowsingTopics() {
  DCHECK(browsing_topics_state_loaded_);

  DCHECK(!topics_calculator_);

  // `this` owns `topics_calculator_` so `topics_calculator_` should not invoke
  // the callback once it's destroyed.
  topics_calculator_ = CreateCalculator(
      privacy_sandbox_settings_, history_service_, site_data_manager_,
      annotations_service_,
      base::BindOnce(
          &BrowsingTopicsServiceImpl::OnCalculateBrowsingTopicsCompleted,
          base::Unretained(this)));
}

void BrowsingTopicsServiceImpl::OnCalculateBrowsingTopicsCompleted(
    EpochTopics epoch_topics) {
  DCHECK(browsing_topics_state_loaded_);

  DCHECK(topics_calculator_);
  topics_calculator_.reset();

  browsing_topics_state_.AddEpoch(std::move(epoch_topics));
  browsing_topics_state_.UpdateNextScheduledCalculationTime();

  ScheduleBrowsingTopicsCalculation(
      blink::features::kBrowsingTopicsTimePeriodPerEpoch.Get());
}

void BrowsingTopicsServiceImpl::OnBrowsingTopicsStateLoaded() {
  DCHECK(!browsing_topics_state_loaded_);
  browsing_topics_state_loaded_ = true;

  base::Time browsing_topics_data_sccessible_since =
      privacy_sandbox_settings_->TopicsDataAccessibleSince();

  StartupCalculateDecision decision = GetStartupCalculationDecision(
      browsing_topics_state_, browsing_topics_data_sccessible_since,
      base::BindRepeating(
          &privacy_sandbox::PrivacySandboxSettings::IsTopicAllowed,
          base::Unretained(privacy_sandbox_settings_)));

  if (decision.clear_topics_data)
    browsing_topics_state_.ClearAllTopics();

  site_data_manager_->ExpireDataBefore(browsing_topics_data_sccessible_since);

  ScheduleBrowsingTopicsCalculation(decision.next_calculation_delay);
}

void BrowsingTopicsServiceImpl::Shutdown() {
  privacy_sandbox_settings_observation_.Reset();
  history_service_observation_.Reset();
}

void BrowsingTopicsServiceImpl::OnTopicsDataAccessibleSinceUpdated() {
  if (!browsing_topics_state_loaded_)
    return;

  // Here we rely on the fact that `browsing_topics_data_accessible_since` can
  // only be updated to base::Time::Now() due to data deletion. In this case, we
  // should just clear all topics.
  browsing_topics_state_.ClearAllTopics();
  site_data_manager_->ExpireDataBefore(
      privacy_sandbox_settings_->TopicsDataAccessibleSince());

  // Abort the outstanding topics calculation and restart immediately.
  if (topics_calculator_) {
    DCHECK(!schedule_calculate_timer_.IsRunning());

    topics_calculator_.reset();
    CalculateBrowsingTopics();
  }
}

void BrowsingTopicsServiceImpl::OnURLsDeleted(
    history::HistoryService* history_service,
    const history::DeletionInfo& deletion_info) {
  if (!browsing_topics_state_loaded_)
    return;

  // Ignore invalid time_range.
  if (!deletion_info.IsAllHistory() && !deletion_info.time_range().IsValid())
    return;

  for (size_t i = 0; i < browsing_topics_state_.epochs().size(); ++i) {
    const EpochTopics& epoch_topics = browsing_topics_state_.epochs()[i];

    if (epoch_topics.empty())
      continue;

    bool time_range_overlap =
        epoch_topics.calculation_time() >= deletion_info.time_range().begin() &&
        DeriveHistoryDataStartTime(epoch_topics.calculation_time()) <=
            deletion_info.time_range().end();

    if (time_range_overlap)
      browsing_topics_state_.ClearOneEpoch(i);
  }

  // If there's an outstanding topics calculation, abort and restart it.
  if (topics_calculator_) {
    DCHECK(!schedule_calculate_timer_.IsRunning());

    topics_calculator_.reset();
    CalculateBrowsingTopics();
  }
}

}  // namespace browsing_topics
