// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/metrics/compositor_frame_reporter.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "base/cpu_reduction_experiment.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/strcat.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "base/trace_event/trace_id_helper.h"
#include "base/trace_event/typed_macros.h"
#include "base/tracing/protos/chrome_track_event.pbzero.h"
#include "cc/base/rolling_time_delta_history.h"
#include "cc/metrics/dropped_frame_counter.h"
#include "cc/metrics/event_latency_tracing_recorder.h"
#include "cc/metrics/frame_sequence_tracker.h"
#include "cc/metrics/latency_ukm_reporter.h"
#include "services/tracing/public/cpp/perfetto/macros.h"
#include "third_party/perfetto/protos/perfetto/trace/track_event/chrome_frame_reporter.pbzero.h"
#include "ui/events/types/event_type.h"

namespace cc {
namespace {

using StageType = CompositorFrameReporter::StageType;
using FrameReportType = CompositorFrameReporter::FrameReportType;
using BlinkBreakdown = CompositorFrameReporter::BlinkBreakdown;
using VizBreakdown = CompositorFrameReporter::VizBreakdown;
using FrameFinalState = FrameInfo::FrameFinalState;

constexpr int kFrameReportTypeCount =
    static_cast<int>(FrameReportType::kMaxValue) + 1;
constexpr int kStageTypeCount = static_cast<int>(StageType::kStageTypeCount);
constexpr int kAllBreakdownCount =
    static_cast<int>(VizBreakdown::kBreakdownCount) +
    static_cast<int>(BlinkBreakdown::kBreakdownCount);

constexpr int kVizBreakdownInitialIndex = kStageTypeCount;
constexpr int kBlinkBreakdownInitialIndex =
    kVizBreakdownInitialIndex + static_cast<int>(VizBreakdown::kBreakdownCount);

// For each possible FrameSequenceTrackerType there will be a UMA histogram
// plus one for general case.
constexpr int kFrameSequenceTrackerTypeCount =
    static_cast<int>(FrameSequenceTrackerType::kMaxType) + 1;

// Maximum number of partial update dependents a reporter can own. When a
// reporter with too many dependents is terminated, it will terminate all its
// dependents which will block the pipeline for a long time. Too many dependents
// also means too much memory usage.
constexpr size_t kMaxOwnedPartialUpdateDependents = 300u;

// Names for CompositorFrameReporter::FrameReportType, which should be
// updated in case of changes to the enum.
constexpr const char* kReportTypeNames[]{
    "", "MissedDeadlineFrame.", "DroppedFrame.", "CompositorOnlyFrame."};

static_assert(std::size(kReportTypeNames) == kFrameReportTypeCount,
              "Compositor latency report types has changed.");

// This value should be recalculated in case of changes to the number of values
// in CompositorFrameReporter::DroppedFrameReportType or in
// CompositorFrameReporter::StageType
constexpr int kStagesWithBreakdownCount = kStageTypeCount + kAllBreakdownCount;
constexpr int kMaxCompositorLatencyHistogramIndex =
    kFrameReportTypeCount *
    (kFrameSequenceTrackerTypeCount + kStagesWithBreakdownCount);

constexpr base::TimeDelta kCompositorLatencyHistogramMin =
    base::Microseconds(1);
constexpr base::TimeDelta kCompositorLatencyHistogramMax =
    base::Milliseconds(350);
constexpr int kCompositorLatencyHistogramBucketCount = 50;

constexpr int kEventLatencyEventTypeCount =
    static_cast<int>(EventMetrics::EventType::kMaxValue) + 1;
constexpr int kEventLatencyGestureTypeCount =
    std::max(static_cast<int>(ScrollEventMetrics::ScrollType::kMaxValue),
             static_cast<int>(PinchEventMetrics::PinchType::kMaxValue)) +
    1;
constexpr int kMaxEventLatencyHistogramIndex =
    kEventLatencyEventTypeCount * kEventLatencyGestureTypeCount;
constexpr base::TimeDelta kEventLatencyHistogramMin = base::Microseconds(1);
constexpr base::TimeDelta kEventLatencyHistogramMax = base::Seconds(5);
constexpr int kEventLatencyHistogramBucketCount = 100;

std::string GetCompositorLatencyHistogramName(
    FrameReportType report_type,
    FrameSequenceTrackerType frame_sequence_tracker_type,
    StageType stage_type,
    absl::optional<VizBreakdown> viz_breakdown,
    absl::optional<BlinkBreakdown> blink_breakdown) {
  DCHECK_LE(frame_sequence_tracker_type, FrameSequenceTrackerType::kMaxType);
  const char* tracker_type_name =
      FrameSequenceTracker::GetFrameSequenceTrackerTypeName(
          frame_sequence_tracker_type);
  DCHECK(tracker_type_name);
  bool impl_only_frame = report_type == FrameReportType::kCompositorOnlyFrame;
  return base::StrCat(
      {"CompositorLatency.", kReportTypeNames[static_cast<int>(report_type)],
       tracker_type_name, *tracker_type_name ? "." : "",
       CompositorFrameReporter::GetStageName(
           stage_type, viz_breakdown, blink_breakdown, impl_only_frame)});
}

std::string GetEventLatencyHistogramBaseName(
    const EventMetrics& event_metrics) {
  auto* scroll_metrics = event_metrics.AsScroll();
  auto* pinch_metrics = event_metrics.AsPinch();
  return base::StrCat(
      {"EventLatency.", event_metrics.GetTypeName(),
       scroll_metrics || pinch_metrics ? "." : "",
       scroll_metrics
           ? scroll_metrics->GetScrollTypeName()
           : pinch_metrics ? pinch_metrics->GetPinchTypeName() : ""});
}

constexpr char kTraceCategory[] =
    "cc,benchmark," TRACE_DISABLED_BY_DEFAULT("devtools.timeline.frame");

base::TimeTicks ComputeSafeDeadlineForFrame(const viz::BeginFrameArgs& args) {
  return args.frame_time + (args.interval * 1.5);
}

}  // namespace

// CompositorFrameReporter::ProcessedBlinkBreakdown::Iterator ==================

CompositorFrameReporter::ProcessedBlinkBreakdown::Iterator::Iterator(
    const ProcessedBlinkBreakdown* owner)
    : owner_(owner) {}

CompositorFrameReporter::ProcessedBlinkBreakdown::Iterator::~Iterator() =
    default;

bool CompositorFrameReporter::ProcessedBlinkBreakdown::Iterator::IsValid()
    const {
  return index_ < std::size(owner_->list_);
}

void CompositorFrameReporter::ProcessedBlinkBreakdown::Iterator::Advance() {
  DCHECK(IsValid());
  index_++;
}

BlinkBreakdown
CompositorFrameReporter::ProcessedBlinkBreakdown::Iterator::GetBreakdown()
    const {
  DCHECK(IsValid());
  return static_cast<BlinkBreakdown>(index_);
}

base::TimeDelta
CompositorFrameReporter::ProcessedBlinkBreakdown::Iterator::GetLatency() const {
  DCHECK(IsValid());
  return owner_->list_[index_];
}

// CompositorFrameReporter::ProcessedBlinkBreakdown ============================

CompositorFrameReporter::ProcessedBlinkBreakdown::ProcessedBlinkBreakdown(
    base::TimeTicks blink_start_time,
    base::TimeTicks begin_main_frame_start,
    const BeginMainFrameMetrics& blink_breakdown) {
  if (blink_start_time.is_null())
    return;

  list_[static_cast<int>(BlinkBreakdown::kHandleInputEvents)] =
      blink_breakdown.handle_input_events;
  list_[static_cast<int>(BlinkBreakdown::kAnimate)] = blink_breakdown.animate;
  list_[static_cast<int>(BlinkBreakdown::kStyleUpdate)] =
      blink_breakdown.style_update;
  list_[static_cast<int>(BlinkBreakdown::kLayoutUpdate)] =
      blink_breakdown.layout_update;
  list_[static_cast<int>(BlinkBreakdown::kPrepaint)] = blink_breakdown.prepaint;
  list_[static_cast<int>(BlinkBreakdown::kCompositingInputs)] =
      blink_breakdown.compositing_inputs;
  list_[static_cast<int>(BlinkBreakdown::kPaint)] = blink_breakdown.paint;
  list_[static_cast<int>(BlinkBreakdown::kCompositeCommit)] =
      blink_breakdown.composite_commit;
  list_[static_cast<int>(BlinkBreakdown::kUpdateLayers)] =
      blink_breakdown.update_layers;
  list_[static_cast<int>(BlinkBreakdown::kBeginMainSentToStarted)] =
      begin_main_frame_start - blink_start_time;
}

CompositorFrameReporter::ProcessedBlinkBreakdown::~ProcessedBlinkBreakdown() =
    default;

CompositorFrameReporter::ProcessedBlinkBreakdown::Iterator
CompositorFrameReporter::ProcessedBlinkBreakdown::CreateIterator() const {
  return Iterator(this);
}

// CompositorFrameReporter::ProcessedVizBreakdown::Iterator ====================

CompositorFrameReporter::ProcessedVizBreakdown::Iterator::Iterator(
    const ProcessedVizBreakdown* owner,
    bool skip_swap_start_to_swap_end)
    : owner_(owner), skip_swap_start_to_swap_end_(skip_swap_start_to_swap_end) {
  DCHECK(owner_);
}

CompositorFrameReporter::ProcessedVizBreakdown::Iterator::~Iterator() = default;

bool CompositorFrameReporter::ProcessedVizBreakdown::Iterator::IsValid() const {
  return index_ < std::size(owner_->list_) && owner_->list_[index_];
}

void CompositorFrameReporter::ProcessedVizBreakdown::Iterator::Advance() {
  DCHECK(IsValid());
  index_++;
  if (static_cast<VizBreakdown>(index_) == VizBreakdown::kSwapStartToSwapEnd &&
      skip_swap_start_to_swap_end_) {
    index_++;
  }
}

VizBreakdown
CompositorFrameReporter::ProcessedVizBreakdown::Iterator::GetBreakdown() const {
  DCHECK(IsValid());
  return static_cast<VizBreakdown>(index_);
}

base::TimeTicks
CompositorFrameReporter::ProcessedVizBreakdown::Iterator::GetStartTime() const {
  DCHECK(IsValid());
  return owner_->list_[index_]->first;
}

base::TimeTicks
CompositorFrameReporter::ProcessedVizBreakdown::Iterator::GetEndTime() const {
  DCHECK(IsValid());
  return owner_->list_[index_]->second;
}

base::TimeDelta
CompositorFrameReporter::ProcessedVizBreakdown::Iterator::GetDuration() const {
  DCHECK(IsValid());
  return owner_->list_[index_]->second - owner_->list_[index_]->first;
}

// CompositorFrameReporter::ProcessedVizBreakdown ==============================

CompositorFrameReporter::ProcessedVizBreakdown::ProcessedVizBreakdown(
    base::TimeTicks viz_start_time,
    const viz::FrameTimingDetails& viz_breakdown) {
  if (viz_start_time.is_null())
    return;

  // Check if `viz_breakdown` is set. Testing indicates that sometimes the
  // received_compositor_frame_timestamp can be earlier than the given
  // `viz_start_time`. Avoid reporting negative times.
  if (viz_breakdown.received_compositor_frame_timestamp.is_null() ||
      viz_breakdown.received_compositor_frame_timestamp < viz_start_time) {
    return;
  }
  list_[static_cast<int>(VizBreakdown::kSubmitToReceiveCompositorFrame)] =
      std::make_pair(viz_start_time,
                     viz_breakdown.received_compositor_frame_timestamp);

  if (viz_breakdown.draw_start_timestamp.is_null())
    return;
  list_[static_cast<int>(VizBreakdown::kReceivedCompositorFrameToStartDraw)] =
      std::make_pair(viz_breakdown.received_compositor_frame_timestamp,
                     viz_breakdown.draw_start_timestamp);

  if (viz_breakdown.swap_timings.is_null())
    return;
  list_[static_cast<int>(VizBreakdown::kStartDrawToSwapStart)] =
      std::make_pair(viz_breakdown.draw_start_timestamp,
                     viz_breakdown.swap_timings.swap_start);

  list_[static_cast<int>(VizBreakdown::kSwapStartToSwapEnd)] =
      std::make_pair(viz_breakdown.swap_timings.swap_start,
                     viz_breakdown.swap_timings.swap_end);

  list_[static_cast<int>(VizBreakdown::kSwapEndToPresentationCompositorFrame)] =
      std::make_pair(viz_breakdown.swap_timings.swap_end,
                     viz_breakdown.presentation_feedback.timestamp);
  swap_start_ = viz_breakdown.swap_timings.swap_start;

  if (viz_breakdown.presentation_feedback.ready_timestamp.is_null())
    return;
  buffer_ready_available_ = true;
  list_[static_cast<int>(VizBreakdown::kSwapStartToBufferAvailable)] =
      std::make_pair(viz_breakdown.swap_timings.swap_start,
                     viz_breakdown.presentation_feedback.available_timestamp);
  list_[static_cast<int>(VizBreakdown::kBufferAvailableToBufferReady)] =
      std::make_pair(viz_breakdown.presentation_feedback.available_timestamp,
                     viz_breakdown.presentation_feedback.ready_timestamp);
  list_[static_cast<int>(VizBreakdown::kBufferReadyToLatch)] =
      std::make_pair(viz_breakdown.presentation_feedback.ready_timestamp,
                     viz_breakdown.presentation_feedback.latch_timestamp);
  list_[static_cast<int>(VizBreakdown::kLatchToSwapEnd)] =
      std::make_pair(viz_breakdown.presentation_feedback.latch_timestamp,
                     viz_breakdown.swap_timings.swap_end);
}

CompositorFrameReporter::ProcessedVizBreakdown::~ProcessedVizBreakdown() =
    default;

CompositorFrameReporter::ProcessedVizBreakdown::Iterator
CompositorFrameReporter::ProcessedVizBreakdown::CreateIterator(
    bool skip_swap_start_to_swap_end_if_breakdown_available) const {
  return Iterator(this, skip_swap_start_to_swap_end_if_breakdown_available &&
                            buffer_ready_available_);
}

// CompositorFrameReporter =====================================================

CompositorFrameReporter::CompositorFrameReporter(
    const ActiveTrackers& active_trackers,
    const viz::BeginFrameArgs& args,
    bool should_report_metrics,
    SmoothThread smooth_thread,
    FrameInfo::SmoothEffectDrivingThread scrolling_thread,
    int layer_tree_host_id,
    const GlobalMetricsTrackers& trackers)
    : should_report_metrics_(should_report_metrics),
      args_(args),
      active_trackers_(active_trackers),
      scrolling_thread_(scrolling_thread),
      smooth_thread_(smooth_thread),
      layer_tree_host_id_(layer_tree_host_id),
      global_trackers_(trackers) {
  global_trackers_.dropped_frame_counter->OnBeginFrame(
      args, IsScrollActive(active_trackers_));
  DCHECK(IsScrollActive(active_trackers_) ||
         scrolling_thread_ == FrameInfo::SmoothEffectDrivingThread::kUnknown);
  if (scrolling_thread_ == FrameInfo::SmoothEffectDrivingThread::kCompositor) {
    DCHECK(smooth_thread_ == SmoothThread::kSmoothCompositor ||
           smooth_thread_ == SmoothThread::kSmoothBoth);
  } else if (scrolling_thread_ == FrameInfo::SmoothEffectDrivingThread::kMain) {
    DCHECK(smooth_thread_ == SmoothThread::kSmoothMain ||
           smooth_thread_ == SmoothThread::kSmoothBoth);
  }
}

// static
const char* CompositorFrameReporter::GetStageName(
    StageType stage_type,
    absl::optional<VizBreakdown> viz_breakdown,
    absl::optional<BlinkBreakdown> blink_breakdown,
    bool impl_only) {
  DCHECK(!viz_breakdown ||
         stage_type ==
             StageType::kSubmitCompositorFrameToPresentationCompositorFrame);
  DCHECK(!blink_breakdown ||
         stage_type == StageType::kSendBeginMainFrameToCommit);
  switch (stage_type) {
    case StageType::kBeginImplFrameToSendBeginMainFrame:
      return impl_only ? "BeginImplFrameToFinishImpl"
                       : "BeginImplFrameToSendBeginMainFrame";
    case StageType::kSendBeginMainFrameToCommit:
      if (!blink_breakdown) {
        return impl_only ? "SendBeginMainFrameToBeginMainAbort"
                         : "SendBeginMainFrameToCommit";
      }
      switch (*blink_breakdown) {
        case BlinkBreakdown::kHandleInputEvents:
          return "SendBeginMainFrameToCommit.HandleInputEvents";
        case BlinkBreakdown::kAnimate:
          return "SendBeginMainFrameToCommit.Animate";
        case BlinkBreakdown::kStyleUpdate:
          return "SendBeginMainFrameToCommit.StyleUpdate";
        case BlinkBreakdown::kLayoutUpdate:
          return "SendBeginMainFrameToCommit.LayoutUpdate";
        case BlinkBreakdown::kPrepaint:
          return "SendBeginMainFrameToCommit.Prepaint";
        case BlinkBreakdown::kCompositingInputs:
          return "SendBeginMainFrameToCommit.CompositingInputs";
        case BlinkBreakdown::kPaint:
          return "SendBeginMainFrameToCommit.Paint";
        case BlinkBreakdown::kCompositeCommit:
          return "SendBeginMainFrameToCommit.CompositeCommit";
        case BlinkBreakdown::kUpdateLayers:
          return "SendBeginMainFrameToCommit.UpdateLayers";
        case BlinkBreakdown::kBeginMainSentToStarted:
          return "SendBeginMainFrameToCommit.BeginMainSentToStarted";
        case BlinkBreakdown::kBreakdownCount:
          NOTREACHED();
          return "";
      }
    case StageType::kCommit:
      return "Commit";
    case StageType::kEndCommitToActivation:
      return "EndCommitToActivation";
    case StageType::kActivation:
      return "Activation";
    case StageType::kEndActivateToSubmitCompositorFrame:
      return impl_only ? "ImplFrameDoneToSubmitCompositorFrame"
                       : "EndActivateToSubmitCompositorFrame";
    case StageType::kSubmitCompositorFrameToPresentationCompositorFrame:
      if (!viz_breakdown)
        return "SubmitCompositorFrameToPresentationCompositorFrame";
      switch (*viz_breakdown) {
        case VizBreakdown::kSubmitToReceiveCompositorFrame:
          return "SubmitCompositorFrameToPresentationCompositorFrame."
                 "SubmitToReceiveCompositorFrame";
        case VizBreakdown::kReceivedCompositorFrameToStartDraw:
          return "SubmitCompositorFrameToPresentationCompositorFrame."
                 "ReceivedCompositorFrameToStartDraw";
        case VizBreakdown::kStartDrawToSwapStart:
          return "SubmitCompositorFrameToPresentationCompositorFrame."
                 "StartDrawToSwapStart";
        case VizBreakdown::kSwapStartToSwapEnd:
          return "SubmitCompositorFrameToPresentationCompositorFrame."
                 "SwapStartToSwapEnd";
        case VizBreakdown::kSwapEndToPresentationCompositorFrame:
          return "SubmitCompositorFrameToPresentationCompositorFrame."
                 "SwapEndToPresentationCompositorFrame";
        case VizBreakdown::kSwapStartToBufferAvailable:
          return "SubmitCompositorFrameToPresentationCompositorFrame."
                 "SwapStartToBufferAvailable";
        case VizBreakdown::kBufferAvailableToBufferReady:
          return "SubmitCompositorFrameToPresentationCompositorFrame."
                 "BufferAvailableToBufferReady";
        case VizBreakdown::kBufferReadyToLatch:
          return "SubmitCompositorFrameToPresentationCompositorFrame."
                 "BufferReadyToLatch";
        case VizBreakdown::kLatchToSwapEnd:
          return "SubmitCompositorFrameToPresentationCompositorFrame."
                 "LatchToSwapEnd";
        case VizBreakdown::kBreakdownCount:
          NOTREACHED();
          return "";
      }
    case StageType::kTotalLatency:
      return "TotalLatency";
    case StageType::kStageTypeCount:
      NOTREACHED();
      return "";
  }
}

// static
const char* CompositorFrameReporter::GetVizBreakdownName(
    VizBreakdown breakdown) {
  switch (breakdown) {
    case VizBreakdown::kSubmitToReceiveCompositorFrame:
      return "SubmitToReceiveCompositorFrame";
    case VizBreakdown::kReceivedCompositorFrameToStartDraw:
      return "ReceiveCompositorFrameToStartDraw";
    case VizBreakdown::kStartDrawToSwapStart:
      return "StartDrawToSwapStart";
    case VizBreakdown::kSwapStartToSwapEnd:
      return "Swap";
    case VizBreakdown::kSwapEndToPresentationCompositorFrame:
      return "SwapEndToPresentationCompositorFrame";
    case VizBreakdown::kSwapStartToBufferAvailable:
      return "SwapStartToBufferAvailable";
    case VizBreakdown::kBufferAvailableToBufferReady:
      return "BufferAvailableToBufferReady";
    case VizBreakdown::kBufferReadyToLatch:
      return "BufferReadyToLatch";
    case VizBreakdown::kLatchToSwapEnd:
      return "LatchToSwapEnd";
    case VizBreakdown::kBreakdownCount:
      NOTREACHED();
      return "";
  }
}

std::unique_ptr<CompositorFrameReporter>
CompositorFrameReporter::CopyReporterAtBeginImplStage() {
  // If |this| reporter is dependent on another reporter to decide about partial
  // update, then |this| should not have any such dependents.
  DCHECK(!partial_update_decider_);

  if (stage_history_.empty() ||
      stage_history_.front().stage_type !=
          StageType::kBeginImplFrameToSendBeginMainFrame ||
      (!did_finish_impl_frame() && !did_not_produce_frame_time_.has_value())) {
    return nullptr;
  }
  auto new_reporter = std::make_unique<CompositorFrameReporter>(
      active_trackers_, args_, should_report_metrics_, smooth_thread_,
      scrolling_thread_, layer_tree_host_id_, global_trackers_);
  new_reporter->did_finish_impl_frame_ = did_finish_impl_frame_;
  new_reporter->impl_frame_finish_time_ = impl_frame_finish_time_;
  new_reporter->main_frame_abort_time_ = main_frame_abort_time_;
  new_reporter->current_stage_.stage_type =
      StageType::kBeginImplFrameToSendBeginMainFrame;
  new_reporter->current_stage_.start_time = stage_history_.front().start_time;
  new_reporter->set_tick_clock(tick_clock_);

  // Set up the new reporter so that it depends on |this| for partial update
  // information.
  new_reporter->SetPartialUpdateDecider(this);

  return new_reporter;
}

CompositorFrameReporter::~CompositorFrameReporter() {
  TerminateReporter();
}

CompositorFrameReporter::StageData::StageData() = default;
CompositorFrameReporter::StageData::StageData(StageType stage_type,
                                              base::TimeTicks start_time,
                                              base::TimeTicks end_time)
    : stage_type(stage_type), start_time(start_time), end_time(end_time) {}
CompositorFrameReporter::StageData::StageData(const StageData&) = default;
CompositorFrameReporter::StageData::~StageData() = default;

void CompositorFrameReporter::StartStage(
    CompositorFrameReporter::StageType stage_type,
    base::TimeTicks start_time) {
  if (frame_termination_status_ != FrameTerminationStatus::kUnknown)
    return;
  EndCurrentStage(start_time);
  current_stage_.stage_type = stage_type;
  current_stage_.start_time = start_time;
  switch (stage_type) {
    case StageType::kSendBeginMainFrameToCommit:
      DCHECK(blink_start_time_.is_null());
      blink_start_time_ = start_time;
      break;
    case StageType::kSubmitCompositorFrameToPresentationCompositorFrame:
      DCHECK(viz_start_time_.is_null());
      viz_start_time_ = start_time;
      break;
    default:
      break;
  }
}

void CompositorFrameReporter::TerminateFrame(
    FrameTerminationStatus termination_status,
    base::TimeTicks termination_time) {
  // If the reporter is already terminated, (possibly as a result of no damage)
  // then we don't need to do anything here, otherwise the reporter will be
  // terminated.
  if (frame_termination_status_ != FrameTerminationStatus::kUnknown)
    return;
  frame_termination_status_ = termination_status;
  frame_termination_time_ = termination_time;
  EndCurrentStage(frame_termination_time_);
}

void CompositorFrameReporter::OnFinishImplFrame(base::TimeTicks timestamp) {
  DCHECK(!did_finish_impl_frame_);

  did_finish_impl_frame_ = true;
  impl_frame_finish_time_ = timestamp;
}

void CompositorFrameReporter::OnAbortBeginMainFrame(base::TimeTicks timestamp) {
  DCHECK(!main_frame_abort_time_.has_value());
  main_frame_abort_time_ = timestamp;
  impl_frame_finish_time_ = timestamp;
  // impl_frame_finish_time_ can be used for the end of BeginMain to Commit
  // stage.
}

void CompositorFrameReporter::OnDidNotProduceFrame(
    FrameSkippedReason skip_reason) {
  did_not_produce_frame_time_ = Now();
  frame_skip_reason_ = skip_reason;
}

void CompositorFrameReporter::EnableCompositorOnlyReporting() {
  EnableReportType(FrameReportType::kCompositorOnlyFrame);
}

void CompositorFrameReporter::SetBlinkBreakdown(
    std::unique_ptr<BeginMainFrameMetrics> blink_breakdown,
    base::TimeTicks begin_main_start) {
  DCHECK(blink_breakdown_.paint.is_zero());
  if (blink_breakdown)
    blink_breakdown_ = *blink_breakdown;
  else
    blink_breakdown_ = BeginMainFrameMetrics();

  DCHECK(begin_main_frame_start_.is_null());
  begin_main_frame_start_ = begin_main_start;
}

void CompositorFrameReporter::SetVizBreakdown(
    const viz::FrameTimingDetails& viz_breakdown) {
  DCHECK(viz_breakdown_.received_compositor_frame_timestamp.is_null());
  viz_breakdown_ = viz_breakdown;
}

void CompositorFrameReporter::AddEventsMetrics(
    EventMetrics::List events_metrics) {
  events_metrics_.insert(events_metrics_.end(),
                         std::make_move_iterator(events_metrics.begin()),
                         std::make_move_iterator(events_metrics.end()));
}

EventMetrics::List CompositorFrameReporter::TakeEventsMetrics() {
  EventMetrics::List result = std::move(events_metrics_);
  events_metrics_.clear();
  return result;
}

void CompositorFrameReporter::TerminateReporter() {
  if (frame_termination_status_ == FrameTerminationStatus::kUnknown)
    TerminateFrame(FrameTerminationStatus::kUnknown, Now());

  processed_blink_breakdown_ = std::make_unique<ProcessedBlinkBreakdown>(
      blink_start_time_, begin_main_frame_start_, blink_breakdown_);
  processed_viz_breakdown_ =
      std::make_unique<ProcessedVizBreakdown>(viz_start_time_, viz_breakdown_);

  DCHECK_EQ(current_stage_.start_time, base::TimeTicks());
  const FrameInfo frame_info = GenerateFrameInfo();
  switch (frame_info.final_state) {
    case FrameFinalState::kDropped:
      EnableReportType(FrameReportType::kDroppedFrame);
      break;

    case FrameFinalState::kNoUpdateDesired:
      // If this reporter was cloned, and the cloned reporter was marked as
      // containing 'partial update' (i.e. missing desired updates from the
      // main-thread), but this reporter terminated with 'no damage', then reset
      // the 'partial update' flag from the cloned reporter (as well as other
      // depending reporters).
      while (!partial_update_dependents_.empty()) {
        auto dependent = partial_update_dependents_.front();
        if (dependent)
          dependent->set_has_partial_update(false);
        partial_update_dependents_.pop();
      }
      break;

    case FrameFinalState::kPresentedAll:
    case FrameFinalState::kPresentedPartialNewMain:
    case FrameFinalState::kPresentedPartialOldMain:
      EnableReportType(FrameReportType::kNonDroppedFrame);
      if (ComputeSafeDeadlineForFrame(args_) < frame_termination_time_)
        EnableReportType(FrameReportType::kMissedDeadlineFrame);
      break;
  }

  ReportCompositorLatencyTraceEvents(frame_info);
  if (TestReportType(FrameReportType::kNonDroppedFrame))
    ReportEventLatencyTraceEvents();

  // Only report compositor latency histograms if the frame was produced.
  if (should_report_metrics_ && report_types_.any()) {
    DCHECK(stage_history_.size());
    DCHECK_EQ(SumOfStageHistory(), stage_history_.back().end_time -
                                       stage_history_.front().start_time);
    stage_history_.emplace_back(StageType::kTotalLatency,
                                stage_history_.front().start_time,
                                stage_history_.back().end_time);

    ReportCompositorLatencyHistograms();
    // Only report event latency histograms if the frame was presented.
    if (TestReportType(FrameReportType::kNonDroppedFrame))
      ReportEventLatencyHistograms();
  }

  auto* dropped_frame_counter = global_trackers_.dropped_frame_counter;
  if (dropped_frame_counter) {
    if (TestReportType(FrameReportType::kDroppedFrame)) {
      dropped_frame_counter->AddDroppedFrame();
    } else {
      if (has_partial_update_)
        dropped_frame_counter->AddPartialFrame();
      else
        dropped_frame_counter->AddGoodFrame();
    }

    dropped_frame_counter->OnEndFrame(args_, frame_info);
  }

  if (discarded_partial_update_dependents_count_ > 0)
    UMA_HISTOGRAM_CUSTOM_COUNTS(
        "Graphics.Smoothness.Diagnostic.DiscardedDependentCount",
        discarded_partial_update_dependents_count_, 1, 1000, 50);
}

void CompositorFrameReporter::EndCurrentStage(base::TimeTicks end_time) {
  if (current_stage_.start_time == base::TimeTicks())
    return;
  current_stage_.end_time = end_time;
  stage_history_.push_back(current_stage_);
  current_stage_.start_time = base::TimeTicks();
}

void CompositorFrameReporter::ReportCompositorLatencyHistograms() const {
  static base::CpuReductionExperimentFilter filter;
  if (!filter.ShouldLogHistograms())
    return;
  for (const StageData& stage : stage_history_) {
    ReportStageHistogramWithBreakdown(stage);

    if (stage.stage_type == StageType::kTotalLatency) {
      for (size_t type = 0; type < active_trackers_.size(); ++type) {
        if (active_trackers_.test(type)) {
          // Report stage breakdowns.
          ReportStageHistogramWithBreakdown(
              stage, static_cast<FrameSequenceTrackerType>(type));
        }
      }
    }
  }

  if (global_trackers_.latency_ukm_reporter) {
    global_trackers_.latency_ukm_reporter->ReportCompositorLatencyUkm(
        report_types_, stage_history_, active_trackers_,
        *processed_blink_breakdown_, *processed_viz_breakdown_);
  }

  for (size_t type = 0; type < report_types_.size(); ++type) {
    if (!report_types_.test(type))
      continue;
    FrameReportType report_type = static_cast<FrameReportType>(type);
    UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type", report_type);
    bool any_active_interaction = false;
    for (size_t fst_type = 0; fst_type < active_trackers_.size(); ++fst_type) {
      const auto tracker_type = static_cast<FrameSequenceTrackerType>(fst_type);
      if (!active_trackers_.test(fst_type) ||
          tracker_type == FrameSequenceTrackerType::kCustom ||
          tracker_type == FrameSequenceTrackerType::kMaxType) {
        continue;
      }
      any_active_interaction = true;
      switch (tracker_type) {
        case FrameSequenceTrackerType::kCompositorAnimation:
          UMA_HISTOGRAM_ENUMERATION(
              "CompositorLatency.Type.CompositorAnimation", report_type);
          break;
        case FrameSequenceTrackerType::kMainThreadAnimation:
          UMA_HISTOGRAM_ENUMERATION(
              "CompositorLatency.Type.MainThreadAnimation", report_type);
          break;
        case FrameSequenceTrackerType::kPinchZoom:
          UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type.PinchZoom",
                                    report_type);
          break;
        case FrameSequenceTrackerType::kRAF:
          UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type.RAF", report_type);
          break;
        case FrameSequenceTrackerType::kTouchScroll:
          UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type.TouchScroll",
                                    report_type);
          break;
        case FrameSequenceTrackerType::kVideo:
          UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type.Video",
                                    report_type);
          break;
        case FrameSequenceTrackerType::kWheelScroll:
          UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type.WheelScroll",
                                    report_type);
          break;
        case FrameSequenceTrackerType::kScrollbarScroll:
          UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type.ScrollbarScroll",
                                    report_type);
          break;
        case FrameSequenceTrackerType::kCanvasAnimation:
          UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type.CanvasAnimation",
                                    report_type);
          break;
        case FrameSequenceTrackerType::kJSAnimation:
          UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type.JSAnimation",
                                    report_type);
          break;
        case FrameSequenceTrackerType::kCustom:
        case FrameSequenceTrackerType::kMaxType:
          NOTREACHED();
          break;
      }
    }
    if (any_active_interaction) {
      UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type.AnyInteraction",
                                report_type);
    } else {
      UMA_HISTOGRAM_ENUMERATION("CompositorLatency.Type.NoInteraction",
                                report_type);
    }
  }
}

void CompositorFrameReporter::ReportStageHistogramWithBreakdown(
    const CompositorFrameReporter::StageData& stage,
    FrameSequenceTrackerType frame_sequence_tracker_type) const {
  base::TimeDelta stage_delta = stage.end_time - stage.start_time;
  ReportCompositorLatencyHistogram(
      frame_sequence_tracker_type, stage.stage_type,
      /*viz_breakdown=*/absl::nullopt,
      /*blink_breakdown=*/absl::nullopt, stage_delta);
  switch (stage.stage_type) {
    case StageType::kSendBeginMainFrameToCommit:
      ReportCompositorLatencyBlinkBreakdowns(frame_sequence_tracker_type);
      break;
    case StageType::kSubmitCompositorFrameToPresentationCompositorFrame:
      ReportCompositorLatencyVizBreakdowns(frame_sequence_tracker_type);
      break;
    default:
      break;
  }
}

void CompositorFrameReporter::ReportCompositorLatencyBlinkBreakdowns(
    FrameSequenceTrackerType frame_sequence_tracker_type) const {
  DCHECK(processed_blink_breakdown_);
  for (auto it = processed_blink_breakdown_->CreateIterator(); it.IsValid();
       it.Advance()) {
    ReportCompositorLatencyHistogram(
        frame_sequence_tracker_type, StageType::kSendBeginMainFrameToCommit,
        /*viz_breakdown=*/absl::nullopt, it.GetBreakdown(), it.GetLatency());
  }
}

void CompositorFrameReporter::ReportCompositorLatencyVizBreakdowns(
    FrameSequenceTrackerType frame_sequence_tracker_type) const {
  DCHECK(processed_viz_breakdown_);
  for (auto it = processed_viz_breakdown_->CreateIterator(false); it.IsValid();
       it.Advance()) {
    ReportCompositorLatencyHistogram(
        frame_sequence_tracker_type,
        StageType::kSubmitCompositorFrameToPresentationCompositorFrame,
        it.GetBreakdown(), /*blink_breakdown=*/absl::nullopt, it.GetDuration());
  }
}

void CompositorFrameReporter::ReportCompositorLatencyHistogram(
    FrameSequenceTrackerType frame_sequence_tracker_type,
    StageType stage_type,
    absl::optional<VizBreakdown> viz_breakdown,
    absl::optional<BlinkBreakdown> blink_breakdown,
    base::TimeDelta time_delta) const {
  DCHECK(!viz_breakdown ||
         stage_type ==
             StageType::kSubmitCompositorFrameToPresentationCompositorFrame);
  DCHECK(!blink_breakdown ||
         stage_type == StageType::kSendBeginMainFrameToCommit);
  for (size_t type = 0; type < report_types_.size(); ++type) {
    if (!report_types_.test(type))
      continue;
    FrameReportType report_type = static_cast<FrameReportType>(type);
    const int report_type_index = static_cast<int>(report_type);
    const int frame_sequence_tracker_type_index =
        static_cast<int>(frame_sequence_tracker_type);
    const int stage_type_index =
        blink_breakdown
            ? kBlinkBreakdownInitialIndex + static_cast<int>(*blink_breakdown)
            : viz_breakdown
                  ? kVizBreakdownInitialIndex + static_cast<int>(*viz_breakdown)
                  : static_cast<int>(stage_type);
    const int histogram_index =
        (stage_type_index == static_cast<int>(StageType::kTotalLatency)
             ? kStagesWithBreakdownCount + frame_sequence_tracker_type_index
             : stage_type_index) *
            kFrameReportTypeCount +
        report_type_index;

    CHECK_LT(stage_type_index, kStagesWithBreakdownCount);
    CHECK_GE(stage_type_index, 0);
    CHECK_LT(report_type_index, kFrameReportTypeCount);
    CHECK_GE(report_type_index, 0);
    CHECK_LT(histogram_index, kMaxCompositorLatencyHistogramIndex);
    CHECK_GE(histogram_index, 0);

    // Note: There's a 1:1 mapping between `histogram_index` and the name
    // returned by `GetCompositorLatencyHistogramName()` which allows the use of
    // `STATIC_HISTOGRAM_POINTER_GROUP()` to cache histogram objects.
    STATIC_HISTOGRAM_POINTER_GROUP(
        GetCompositorLatencyHistogramName(
            report_type, frame_sequence_tracker_type, stage_type, viz_breakdown,
            blink_breakdown),
        histogram_index, kMaxCompositorLatencyHistogramIndex,
        AddTimeMicrosecondsGranularity(time_delta),
        base::Histogram::FactoryMicrosecondsTimeGet(
            GetCompositorLatencyHistogramName(
                report_type, frame_sequence_tracker_type, stage_type,
                viz_breakdown, blink_breakdown),
            kCompositorLatencyHistogramMin, kCompositorLatencyHistogramMax,
            kCompositorLatencyHistogramBucketCount,
            base::HistogramBase::kUmaTargetedHistogramFlag));
  }
}

void CompositorFrameReporter::ReportEventLatencyHistograms() const {
  const StageData& total_latency_stage = stage_history_.back();
  DCHECK_EQ(StageType::kTotalLatency, total_latency_stage.stage_type);

  const std::string total_latency_stage_name =
      GetStageName(StageType::kTotalLatency);
  const std::string total_latency_histogram_name =
      "EventLatency." + total_latency_stage_name;

  for (const auto& event_metrics : events_metrics_) {
    DCHECK(event_metrics);
    const std::string histogram_base_name =
        GetEventLatencyHistogramBaseName(*event_metrics);
    const int event_type_index = static_cast<int>(event_metrics->type());
    auto* scroll_metrics = event_metrics->AsScroll();
    auto* pinch_metrics = event_metrics->AsPinch();
    const int gesture_type_index =
        scroll_metrics
            ? static_cast<int>(scroll_metrics->scroll_type())
            : pinch_metrics ? static_cast<int>(pinch_metrics->pinch_type()) : 0;
    const int event_histogram_index =
        event_type_index * kEventLatencyGestureTypeCount + gesture_type_index;

    const base::TimeTicks generated_timestamp =
        event_metrics->GetDispatchStageTimestamp(
            EventMetrics::DispatchStage::kGenerated);
    DCHECK_LT(generated_timestamp, total_latency_stage.end_time);

    // Report total latency up to presentation for the event.
    const base::TimeDelta total_latency =
        total_latency_stage.end_time - generated_timestamp;
    const std::string event_total_latency_histogram_name =
        base::StrCat({histogram_base_name, ".", total_latency_stage_name});
    // Note: There's a 1:1 mapping between `event_histogram_index` and
    // `event_total_latency_histogram_name` which allows the use of
    // `STATIC_HISTOGRAM_POINTER_GROUP()` to cache histogram objects.
    STATIC_HISTOGRAM_POINTER_GROUP(
        event_total_latency_histogram_name, event_histogram_index,
        kMaxEventLatencyHistogramIndex,
        AddTimeMicrosecondsGranularity(total_latency),
        base::Histogram::FactoryMicrosecondsTimeGet(
            event_total_latency_histogram_name, kEventLatencyHistogramMin,
            kEventLatencyHistogramMax, kEventLatencyHistogramBucketCount,
            base::HistogramBase::kUmaTargetedHistogramFlag));

    // Also, report total latency up to presentation for all event types in an
    // aggregate histogram.
    UMA_HISTOGRAM_CUSTOM_MICROSECONDS_TIMES(
        total_latency_histogram_name, total_latency, kEventLatencyHistogramMin,
        kEventLatencyHistogramMax, kEventLatencyHistogramBucketCount);
  }

  if (global_trackers_.latency_ukm_reporter) {
    global_trackers_.latency_ukm_reporter->ReportEventLatencyUkm(
        events_metrics_, stage_history_, *processed_blink_breakdown_,
        *processed_viz_breakdown_);
  }
}

void CompositorFrameReporter::ReportCompositorLatencyTraceEvents(
    const FrameInfo& info) const {
  if (stage_history_.empty())
    return;

  if (info.IsDroppedAffectingSmoothness()) {
    devtools_instrumentation::DidDropSmoothnessFrame(
        layer_tree_host_id_, args_.frame_time, args_.frame_id.sequence_number,
        has_partial_update_);
  }

  const auto trace_track =
      perfetto::Track(base::trace_event::GetNextGlobalTraceId());
  TRACE_EVENT_BEGIN(
      kTraceCategory, "PipelineReporter", trace_track, args_.frame_time,
      [&](perfetto::EventContext context) {
        using perfetto::protos::pbzero::ChromeFrameReporter;
        ChromeFrameReporter::State state;
        switch (info.final_state) {
          case FrameInfo::FrameFinalState::kPresentedAll:
            state = ChromeFrameReporter::STATE_PRESENTED_ALL;
            break;
          case FrameInfo::FrameFinalState::kPresentedPartialNewMain:
          case FrameInfo::FrameFinalState::kPresentedPartialOldMain:
            state = ChromeFrameReporter::STATE_PRESENTED_PARTIAL;
            break;
          case FrameInfo::FrameFinalState::kNoUpdateDesired:
            state = ChromeFrameReporter::STATE_NO_UPDATE_DESIRED;
            break;
          case FrameInfo::FrameFinalState::kDropped:
            state = ChromeFrameReporter::STATE_DROPPED;
            break;
        }

        auto* reporter = context.event()->set_chrome_frame_reporter();
        reporter->set_state(state);
        reporter->set_frame_source(args_.frame_id.source_id);
        reporter->set_frame_sequence(args_.frame_id.sequence_number);
        reporter->set_layer_tree_host_id(layer_tree_host_id_);
        reporter->set_has_missing_content(info.has_missing_content);
        if (info.IsDroppedAffectingSmoothness()) {
          DCHECK(state == ChromeFrameReporter::STATE_DROPPED ||
                 state == ChromeFrameReporter::STATE_PRESENTED_PARTIAL);
        }
        reporter->set_affects_smoothness(info.IsDroppedAffectingSmoothness());
        ChromeFrameReporter::ScrollState scroll_state;
        switch (info.scroll_thread) {
          case FrameInfo::SmoothEffectDrivingThread::kMain:
            scroll_state = ChromeFrameReporter::SCROLL_MAIN_THREAD;
            break;
          case FrameInfo::SmoothEffectDrivingThread::kCompositor:
            scroll_state = ChromeFrameReporter::SCROLL_COMPOSITOR_THREAD;
            break;
          case FrameInfo::SmoothEffectDrivingThread::kUnknown:
            scroll_state = ChromeFrameReporter::SCROLL_NONE;
            break;
        }
        reporter->set_scroll_state(scroll_state);
        reporter->set_has_main_animation(
            HasMainThreadAnimation(active_trackers_));
        reporter->set_has_compositor_animation(
            HasCompositorThreadAnimation(active_trackers_));

        bool has_smooth_input_main = false;
        for (const auto& event_metrics : events_metrics_) {
          has_smooth_input_main |= event_metrics->HasSmoothInputEvent();
        }
        reporter->set_has_smooth_input_main(has_smooth_input_main);

        // TODO(crbug.com/1086974): Set 'drop reason' if applicable.
      });

  for (const auto& stage : stage_history_) {
    if (stage.start_time >= frame_termination_time_)
      break;
    DCHECK_GE(stage.end_time, stage.start_time);
    if (stage.start_time == stage.end_time)
      continue;

    const char* stage_name = GetStageName(stage.stage_type);

    if (stage.stage_type == StageType::kSendBeginMainFrameToCommit) {
      TRACE_EVENT_BEGIN(
          kTraceCategory, perfetto::StaticString{stage_name}, trace_track,
          stage.start_time, [&](perfetto::EventContext context) {
            DCHECK(processed_blink_breakdown_);
            auto* reporter =
                context.event<perfetto::protos::pbzero::ChromeTrackEvent>()
                    ->set_send_begin_mainframe_to_commit_breakdown();
            for (auto it = processed_blink_breakdown_->CreateIterator();
                 it.IsValid(); it.Advance()) {
              int64_t latency = it.GetLatency().InMicroseconds();
              int curr_breakdown = static_cast<int>(it.GetBreakdown());
              switch (curr_breakdown) {
                case static_cast<int>(BlinkBreakdown::kHandleInputEvents):
                  reporter->set_handle_input_events_us(latency);
                  break;
                case static_cast<int>(BlinkBreakdown::kAnimate):
                  reporter->set_animate_us(latency);
                  break;
                case static_cast<int>(BlinkBreakdown::kStyleUpdate):
                  reporter->set_style_update_us(latency);
                  break;
                case static_cast<int>(BlinkBreakdown::kLayoutUpdate):
                  reporter->set_layout_update_us(latency);
                  break;
                case static_cast<int>(BlinkBreakdown::kPrepaint):
                  reporter->set_prepaint_us(latency);
                  break;
                case static_cast<int>(BlinkBreakdown::kCompositingInputs):
                  reporter->set_compositing_inputs_us(latency);
                  break;
                case static_cast<int>(BlinkBreakdown::kPaint):
                  reporter->set_paint_us(latency);
                  break;
                case static_cast<int>(BlinkBreakdown::kCompositeCommit):
                  reporter->set_composite_commit_us(latency);
                  break;
                case static_cast<int>(BlinkBreakdown::kUpdateLayers):
                  reporter->set_update_layers_us(latency);
                  break;
                case static_cast<int>(BlinkBreakdown::kBeginMainSentToStarted):
                  reporter->set_begin_main_sent_to_started_us(latency);
                  break;
                default:
                  break;
              }
            }
          });
    } else {
      TRACE_EVENT_BEGIN(kTraceCategory, perfetto::StaticString{stage_name},
                        trace_track, stage.start_time);
    }

    if (stage.stage_type ==
        StageType::kSubmitCompositorFrameToPresentationCompositorFrame) {
      DCHECK(processed_viz_breakdown_);
      for (auto it = processed_viz_breakdown_->CreateIterator(true);
           it.IsValid(); it.Advance()) {
        base::TimeTicks start_time = it.GetStartTime();
        base::TimeTicks end_time = it.GetEndTime();
        if (start_time >= end_time)
          continue;
        const char* breakdown_name = GetVizBreakdownName(it.GetBreakdown());
        TRACE_EVENT_BEGIN(kTraceCategory,
                          perfetto::StaticString{breakdown_name}, trace_track,
                          start_time);
        TRACE_EVENT_END(kTraceCategory, trace_track, end_time);
      }
    }
    TRACE_EVENT_END(kTraceCategory, trace_track, stage.end_time);
  }

  TRACE_EVENT_END(kTraceCategory, trace_track, frame_termination_time_);
}

void CompositorFrameReporter::ReportEventLatencyTraceEvents() const {
  // TODO(mohsen): This function is becoming large and there is concerns about
  // having this in the compositor critical path. crbug.com/1072740 is
  // considering doing the reporting off-thread, but as a short-term solution,
  // we should investigate whether we can skip this function entirely if tracing
  // is off and whether that has any positive impact or not.
  for (const auto& event_metrics : events_metrics_) {
    EventLatencyTracingRecorder::RecordEventLatencyTraceEvent(
        event_metrics.get(), frame_termination_time_, &stage_history_,
        processed_viz_breakdown_.get());
  }
}

base::TimeDelta CompositorFrameReporter::SumOfStageHistory() const {
  base::TimeDelta sum;
  for (const StageData& stage : stage_history_)
    sum += stage.end_time - stage.start_time;
  return sum;
}

base::TimeTicks CompositorFrameReporter::Now() const {
  return tick_clock_->NowTicks();
}

void CompositorFrameReporter::AdoptReporter(
    std::unique_ptr<CompositorFrameReporter> reporter) {
  // If |this| reporter is dependent on another reporter to decide about partial
  // update, then |this| should not have any such dependents.
  DCHECK(!partial_update_decider_);
  DCHECK(!partial_update_dependents_.empty());
  owned_partial_update_dependents_.push(std::move(reporter));
  DiscardOldPartialUpdateReporters();
}

void CompositorFrameReporter::SetPartialUpdateDecider(
    CompositorFrameReporter* decider) {
  DCHECK(decider);
  DCHECK(partial_update_dependents_.empty());
  has_partial_update_ = true;
  partial_update_decider_ = decider->GetWeakPtr();
  decider->partial_update_dependents_.push(GetWeakPtr());
}

void CompositorFrameReporter::DiscardOldPartialUpdateReporters() {
  DCHECK_LE(owned_partial_update_dependents_.size(),
            partial_update_dependents_.size());
  // Remove old owned partial update dependents if there are too many.
  while (owned_partial_update_dependents_.size() >
         kMaxOwnedPartialUpdateDependents) {
    auto& dependent = owned_partial_update_dependents_.front();
    dependent->set_has_partial_update(false);
    owned_partial_update_dependents_.pop();
    discarded_partial_update_dependents_count_++;
  }

  // Remove dependent reporters from the front of `partial_update_dependents_`
  // queue if they are already destroyed.
  while (!partial_update_dependents_.empty() &&
         !partial_update_dependents_.front()) {
    partial_update_dependents_.pop();
  }
}

base::WeakPtr<CompositorFrameReporter> CompositorFrameReporter::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

FrameInfo CompositorFrameReporter::GenerateFrameInfo() const {
  FrameFinalState final_state = FrameFinalState::kNoUpdateDesired;
  auto smooth_thread = smooth_thread_;
  auto scrolling_thread = scrolling_thread_;

  switch (frame_termination_status_) {
    case FrameTerminationStatus::kPresentedFrame:
      if (has_partial_update_) {
        final_state = is_accompanied_by_main_thread_update_
                          ? FrameFinalState::kPresentedPartialNewMain
                          : FrameFinalState::kPresentedPartialOldMain;
      } else {
        final_state = FrameFinalState::kPresentedAll;
      }
      break;

    case FrameTerminationStatus::kDidNotPresentFrame:
    case FrameTerminationStatus::kReplacedByNewReporter:
      final_state = FrameFinalState::kDropped;
      break;

    case FrameTerminationStatus::kDidNotProduceFrame: {
      const bool no_update_expected_from_main =
          frame_skip_reason_.has_value() &&
          frame_skip_reason() == FrameSkippedReason::kNoDamage;
      const bool no_update_expected_from_compositor =
          !has_partial_update_ && frame_skip_reason_.has_value() &&
          frame_skip_reason() == FrameSkippedReason::kWaitingOnMain;
      const bool draw_is_throttled =
          frame_skip_reason_.has_value() &&
          frame_skip_reason() == FrameSkippedReason::kDrawThrottled;

      if (!no_update_expected_from_main &&
          !no_update_expected_from_compositor) {
        final_state = FrameFinalState::kDropped;
      } else if (draw_is_throttled) {
        final_state = FrameFinalState::kDropped;
      } else {
        final_state = FrameFinalState::kNoUpdateDesired;
      }

      // If the compositor-thread is running an animation, and it ends with
      // 'did not produce frame', then that implies that the compositor
      // animation did not cause any visual changes. So for such cases, update
      // the `smooth_thread` for the FrameInfo created to exclude the compositor
      // thread. However, it is important to keep `final_state` unchanged,
      // because the main-thread update (if any) did get dropped.
      if (frame_skip_reason_.has_value() &&
          frame_skip_reason() == FrameSkippedReason::kWaitingOnMain) {
        if (smooth_thread == SmoothThread::kSmoothBoth) {
          smooth_thread = SmoothThread::kSmoothMain;
        } else if (smooth_thread == SmoothThread::kSmoothCompositor) {
          smooth_thread = SmoothThread::kSmoothNone;
        }

        if (scrolling_thread ==
            FrameInfo::SmoothEffectDrivingThread::kCompositor) {
          scrolling_thread = FrameInfo::SmoothEffectDrivingThread::kUnknown;
        }
      }

      break;
    }

    case FrameTerminationStatus::kUnknown:
      break;
  }

  FrameInfo info;
  info.final_state = final_state;
  info.smooth_thread = smooth_thread;
  info.scroll_thread = scrolling_thread;
  info.has_missing_content = has_missing_content_;

  if (frame_skip_reason_.has_value() &&
      frame_skip_reason() == FrameSkippedReason::kNoDamage) {
    // If the frame was explicitly skipped because of 'no damage', then that
    // means this frame contains the response ('no damage') from the
    // main-thread.
    info.main_thread_response = FrameInfo::MainThreadResponse::kIncluded;
  } else if (partial_update_dependents_.size() > 0) {
    // Only a frame containing a response from the main-thread can have
    // dependent reporters.
    info.main_thread_response = FrameInfo::MainThreadResponse::kIncluded;
  } else if (begin_main_frame_start_.is_null() ||
             (frame_skip_reason_.has_value() &&
              frame_skip_reason() == FrameSkippedReason::kWaitingOnMain)) {
    // If 'begin main frame' never started, or if it started, but it
    // had to be skipped because it was waiting on the main-thread,
    // then the main-thread update is missing from this reporter.
    info.main_thread_response = FrameInfo::MainThreadResponse::kMissing;
  } else {
    info.main_thread_response = FrameInfo::MainThreadResponse::kIncluded;
  }

  if (!stage_history_.empty()) {
    const auto& stage = stage_history_.back();
    if (stage.stage_type == StageType::kTotalLatency) {
      DCHECK_EQ(frame_termination_time_ - args_.frame_time,
                stage.end_time - stage.start_time);
      info.total_latency = frame_termination_time_ - args_.frame_time;
    }
  }

  return info;
}

}  // namespace cc
