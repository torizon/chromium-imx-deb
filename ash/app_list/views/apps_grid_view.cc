// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/app_list/views/apps_grid_view.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ash/app_list/app_list_metrics.h"
#include "ash/app_list/app_list_util.h"
#include "ash/app_list/app_list_view_delegate.h"
#include "ash/app_list/model/app_list_folder_item.h"
#include "ash/app_list/model/app_list_item.h"
#include "ash/app_list/model/app_list_model.h"
#include "ash/app_list/paged_view_structure.h"
#include "ash/app_list/views/app_drag_icon_proxy.h"
#include "ash/app_list/views/app_list_a11y_announcer.h"
#include "ash/app_list/views/app_list_drag_and_drop_host.h"
#include "ash/app_list/views/app_list_folder_controller.h"
#include "ash/app_list/views/app_list_folder_view.h"
#include "ash/app_list/views/app_list_item_view.h"
#include "ash/app_list/views/app_list_view_util.h"
#include "ash/app_list/views/apps_grid_context_menu.h"
#include "ash/app_list/views/apps_grid_view_focus_delegate.h"
#include "ash/app_list/views/ghost_image_view.h"
#include "ash/app_list/views/pulsing_block_view.h"
#include "ash/app_list/views/search_box_view.h"
#include "ash/app_list/views/search_result_tile_item_view.h"
#include "ash/constants/ash_features.h"
#include "ash/public/cpp/app_list/app_list_config.h"
#include "ash/public/cpp/app_list/app_list_features.h"
#include "ash/public/cpp/app_list/app_list_switches.h"
#include "ash/public/cpp/metrics_util.h"
#include "ash/strings/grit/ash_strings.h"
#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/cxx17_backports.h"
#include "base/guid.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/user_metrics.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/aura/window.h"
#include "ui/aura/window_event_dispatcher.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/events/event.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/geometry/transform_util.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/geometry/vector2d_conversions.h"
#include "ui/strings/grit/ui_strings.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/animation/animation_builder.h"
#include "ui/views/animation/bounds_animator.h"
#include "ui/views/border.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/view_observer.h"
#include "ui/views/widget/widget.h"
#include "ui/wm/core/coordinate_conversion.h"

namespace ash {

namespace {

// Distance a drag needs to be from the app grid to be considered 'outside', at
// which point we rearrange the apps to their pre-drag configuration, as a drop
// then would be canceled. We have a buffer to make it easier to drag apps to
// other pages.
constexpr int kDragBufferPx = 20;

// Time delay before shelf starts to handle icon drag operation,
// such as shelf icons re-layout.
constexpr base::TimeDelta kShelfHandleIconDragDelay = base::Milliseconds(500);

// The drag and drop proxy should get scaled by this factor.
constexpr float kDragAndDropProxyScale = 1.2f;

// Delays in milliseconds to show re-order preview.
constexpr int kReorderDelay = 120;

// Delays in milliseconds to show folder item reparent UI.
constexpr int kFolderItemReparentDelay = 50;

// Maximum vertical and horizontal spacing between tiles.
constexpr int kMaximumTileSpacing = 96;

// Maximum horizontal spacing between tiles for productivity launcher.
constexpr int kMaximumHorizontalTileSpacingForProductivityLauncher = 128;

// The ratio of the slide offset to the tile size.
constexpr float kFadeAnimationOffsetRatio = 0.25f;

// The time duration of the fade in animation used for apps grid reorder.
constexpr base::TimeDelta kFadeInAnimationDuration = base::Milliseconds(400);

// The time duration of the fade out animation used for apps grid reorder.
constexpr base::TimeDelta kFadeOutAnimationDuration = base::Milliseconds(100);

// Constants for folder item view relocation animation - the animation runs
// after closing a folder view if the shown folder item view location within the
// apps grid changed while the folder view was open.
// The folder view animates in the old folder item location, then the folder
// item view animates out at the old location, other items move into their
// correct spot, and after a delay, the folder item view animates into its new
// location.
//
// The duration of the folder item view fade out animation.
constexpr base::TimeDelta kFolderItemFadeOutDuration = base::Milliseconds(100);

// The duraction of the folder item view fade in animation.
constexpr base::TimeDelta kFolderItemFadeInDuration = base::Milliseconds(300);

// The delay for starting the folder item view fade in after the item view was
// faded out.
constexpr base::TimeDelta kFolderItemFadeInDelay = base::Milliseconds(300);

// RowMoveAnimationDelegate is used when moving an item into a different row.
// Before running the animation, the item's layer is re-created and kept in
// the original position, then the item is moved to just before its target
// position and opacity set to 0. When the animation runs, this delegate moves
// the layer and fades it out while fading in the item at the same time.
class RowMoveAnimationDelegate : public views::AnimationDelegateViews {
 public:
  RowMoveAnimationDelegate(views::View* view,
                           ui::Layer* layer,
                           const gfx::Vector2d& offset)
      : views::AnimationDelegateViews(view),
        view_(view),
        layer_(layer),
        offset_(offset) {}

  RowMoveAnimationDelegate(const RowMoveAnimationDelegate&) = delete;
  RowMoveAnimationDelegate& operator=(const RowMoveAnimationDelegate&) = delete;

  ~RowMoveAnimationDelegate() override = default;

  // views::AnimationDelegateViews:
  void AnimationProgressed(const gfx::Animation* animation) override {
    view_->layer()->SetOpacity(animation->GetCurrentValue());
    view_->layer()->ScheduleDraw();

    if (layer_) {
      layer_->SetOpacity(1 - animation->GetCurrentValue());

      gfx::Transform transform;
      transform.Translate(animation->CurrentValueBetween(0, offset_.x()),
                          animation->CurrentValueBetween(0, offset_.y()));
      layer_->SetTransform(transform);
      layer_->ScheduleDraw();
    }
  }
  void AnimationEnded(const gfx::Animation* animation) override {
    if (layer_)
      view_->layer()->SetOpacity(1.0f);
  }
  void AnimationCanceled(const gfx::Animation* animation) override {
    if (layer_)
      view_->layer()->SetOpacity(1.0f);
  }

 private:
  // The view that needs to be wrapped. Owned by views hierarchy.
  views::View* view_;

  std::unique_ptr<ui::Layer> layer_;
  const gfx::Vector2d offset_;
};

bool IsOEMFolderItem(AppListItem* item) {
  return IsFolderItem(item) &&
         (static_cast<AppListFolderItem*>(item))->folder_type() ==
             AppListFolderItem::FOLDER_TYPE_OEM;
}

// Returns the relative horizontal position of a point compared to a rect. -1
// means the point is outside on the left side of the rect. 0 means the point is
// within the rect. 1 means it's on the right side of the rect.
int CompareHorizontalPointPositionToRect(gfx::Point point, gfx::Rect bounds) {
  if (point.x() > bounds.right())
    return 1;
  if (point.x() < bounds.x())
    return -1;
  return 0;
}

}  // namespace

bool GridIndex::IsValid() const {
  return page >= 0 && slot >= 0;
}

std::string GridIndex::ToString() const {
  std::stringstream ss;
  ss << "Page: " << page << ", Slot: " << slot;
  return ss.str();
}

// static
constexpr int AppsGridView::kDefaultAnimationDuration;

// AppsGridView::VisibleItemIndexRange -----------------------------------------

AppsGridView::VisibleItemIndexRange::VisibleItemIndexRange() = default;

AppsGridView::VisibleItemIndexRange::VisibleItemIndexRange(
    int input_first_index,
    int input_last_index)
    : first_index(input_first_index), last_index(input_last_index) {}

AppsGridView::VisibleItemIndexRange::~VisibleItemIndexRange() = default;

// AppsGridView::FolderIconItemHider -------------------------------------------

// Class used to hide an icon depicting an app list item from an folder item
// icon image (which contains images of top app items in the folder).
// Used during drag icon drop animation to hide the dragged item from the folder
// icon (if the item is being dropped into a folder) while the drag icon is
// still visible.
// It gracefully handles the folder item getting deleted before the
// `FolderIconItemHider` instance gets reset, so it should be safe to use in
// asynchronous manner without extra folder item existence checks.
class AppsGridView::FolderIconItemHider : public AppListItemObserver {
 public:
  FolderIconItemHider(AppListFolderItem* folder_item,
                      AppListItem* item_icon_to_hide)
      : folder_item_(folder_item) {
    // Notify the folder item that `item_icon_to_hide` is being dragged, so the
    // dragged item is ignored while generating the folder icon image. This
    // effectively hides the drag item image from the overall folder icon.
    folder_item_->NotifyOfDraggedItem(item_icon_to_hide);
    folder_item_observer_.Observe(folder_item_);
  }

  ~FolderIconItemHider() override {
    if (folder_item_)
      folder_item_->NotifyOfDraggedItem(nullptr);
  }

  // AppListItemObserver:
  void ItemBeingDestroyed() override {
    folder_item_ = nullptr;
    folder_item_observer_.Reset();
  }

 private:
  AppListFolderItem* folder_item_;

  base::ScopedObservation<AppListItem, AppListItemObserver>
      folder_item_observer_{this};
};

// Class that while in scope hides a drag view in such way that the drag view
// keeps receiving mouse/gesture events. Used to hide the dragged view while a
// drag icon proxy for the drag item is shown. It gracefully handles the case
// where it outlives the hidden dragged view, so it should be safe to be used
// asynchronously without extra view existence checks.
class AppsGridView::DragViewHider : public views::ViewObserver {
 public:
  explicit DragViewHider(AppListItemView* drag_view) : drag_view_(drag_view) {
    DCHECK(drag_view_->layer());
    drag_view_->layer()->SetOpacity(0.0f);
    view_observer_.Observe(drag_view_);
  }

  ~DragViewHider() override {
    if (drag_view_ && drag_view_->layer())
      drag_view_->layer()->SetOpacity(1.0f);
  }

  // views::ViewObserver:
  void OnViewIsDeleting(views::View* view) override {
    drag_view_ = nullptr;
    view_observer_.Reset();
  }

  const views::View* drag_view() const { return drag_view_; }

 private:
  AppListItemView* drag_view_;

  base::ScopedObservation<views::View, views::ViewObserver> view_observer_{
      this};
};

// Class used by AppsGridView to track whether app list model is being updated
// by the AppsGridView (by setting `updating_model_`). While this is in scope:
// (1) Do not cancel in progress drag due to app list model changes, and
// (2) Delay `view_structure_` sanitization until the app list model update
// finishes, and
// (3) Ignore apps grid layout
class AppsGridView::ScopedModelUpdate {
 public:
  explicit ScopedModelUpdate(AppsGridView* apps_grid_view)
      : apps_grid_view_(apps_grid_view),
        initial_grid_size_(apps_grid_view_->GetTileGridSize()) {
    DCHECK(!apps_grid_view_->updating_model_);
    apps_grid_view_->updating_model_ = true;

    // One model update may elicit multiple changes on apps grid layout. For
    // example, moving one item out of a folder may empty the parent folder then
    // have the folder deleted. Therefore ignore layout when `ScopedModelUpdate`
    // is in the scope to avoid handling temporary layout.
    DCHECK(!apps_grid_view_->ignore_layout_);
    apps_grid_view_->ignore_layout_ = true;

    view_structure_sanitize_lock_ =
        apps_grid_view_->view_structure_.GetSanitizeLock();
  }
  ScopedModelUpdate(const ScopedModelUpdate&) = delete;
  ScopedModelUpdate& operator=(const ScopedModelUpdate&) = delete;
  ~ScopedModelUpdate() {
    DCHECK(apps_grid_view_->updating_model_);
    apps_grid_view_->updating_model_ = false;

    DCHECK(apps_grid_view_->ignore_layout_);
    apps_grid_view_->ignore_layout_ = false;

    // Perform update for the final layout.
    apps_grid_view_->ScheduleLayout(initial_grid_size_);
  }

 private:
  AppsGridView* const apps_grid_view_;
  const gfx::Size initial_grid_size_;
  std::unique_ptr<PagedViewStructure::ScopedSanitizeLock>
      view_structure_sanitize_lock_;
};

AppsGridView::AppsGridView(AppListA11yAnnouncer* a11y_announcer,
                           AppListViewDelegate* app_list_view_delegate,
                           AppsGridViewFolderDelegate* folder_delegate,
                           AppListFolderController* folder_controller,
                           AppsGridViewFocusDelegate* focus_delegate)
    : folder_delegate_(folder_delegate),
      folder_controller_(folder_controller),
      a11y_announcer_(a11y_announcer),
      app_list_view_delegate_(app_list_view_delegate),
      focus_delegate_(focus_delegate) {
  DCHECK(a11y_announcer_);
  DCHECK(app_list_view_delegate_);
  // Top-level grids must have a folder controller.
  if (!folder_delegate_)
    DCHECK(folder_controller_);

  SetPaintToLayer(ui::LAYER_NOT_DRAWN);

  items_container_ = AddChildView(std::make_unique<views::View>());
  items_container_->SetPaintToLayer();
  items_container_->layer()->SetFillsBoundsOpaquely(false);
  bounds_animator_ = std::make_unique<views::BoundsAnimator>(
      items_container_, /*use_transforms=*/true);
  bounds_animator_->AddObserver(this);
  bounds_animator_->SetAnimationDuration(base::Milliseconds(300));
  if (features::IsProductivityLauncherEnabled()) {
    GetViewAccessibility().OverrideRole(ax::mojom::Role::kGroup);
    GetViewAccessibility().OverrideName(
        l10n_util::GetStringUTF16(IDS_ALL_APPS_INDICATOR));
  }

  context_menu_ = std::make_unique<AppsGridContextMenu>();
  set_context_menu_controller(context_menu_.get());
}

AppsGridView::~AppsGridView() {
  bounds_animator_->RemoveObserver(this);

  // Coming here |drag_view_| should already be canceled since otherwise the
  // drag would disappear after the app list got animated away and closed,
  // which would look odd.
  DCHECK(!drag_item_);
  if (drag_item_)
    EndDrag(true);

  if (model_)
    model_->RemoveObserver(this);

  if (item_list_)
    item_list_->RemoveObserver(this);

  // Cancel animations now, otherwise RemoveAllChildViews() may call back to
  // ViewHierarchyChanged() during removal, which can lead to double deletes
  // (because ViewHierarchyChanged() may attempt to delete a view that is part
  // way through deletion). Note that cancelling animations may cause
  // AppListItemView to Layout(), which may call back into this object.
  bounds_animator_->Cancel();

  // Abort reorder animation before `view_model_` is cleared.
  MaybeAbortReorderAnimation();

  view_model_.Clear();
  RemoveAllChildViews();

  // `OnBoundsAnimatorDone`, which uses `bounds_animator_`, is called on
  // `drag_icon_proxy_` destruction. Reset `drag_icon_proxy_` early, while
  // `bounds_animator_` is still around.
  folder_to_open_after_drag_icon_animation_.clear();
  drag_icon_proxy_.reset();
}

void AppsGridView::Init() {
  UpdateBorder();
}

void AppsGridView::UpdateAppListConfig(const AppListConfig* app_list_config) {
  app_list_config_ = app_list_config;

  // The app list item view icon sizes depend on the app list config, so they
  // have to be refreshed.
  for (int i = 0; i < view_model_.view_size(); ++i)
    view_model_.view_at(i)->UpdateAppListConfig(app_list_config);

  if (current_ghost_view_)
    CreateGhostImageView();
}

void AppsGridView::SetFixedTilePadding(int horizontal_padding,
                                       int vertical_padding) {
  has_fixed_tile_padding_ = true;
  horizontal_tile_padding_ = horizontal_padding;
  vertical_tile_padding_ = vertical_padding;
}

gfx::Size AppsGridView::GetTotalTileSize(int page) const {
  gfx::Rect rect(GetTileViewSize());
  rect.Inset(GetTilePadding(page));
  return rect.size();
}

gfx::Size AppsGridView::GetMinimumTileGridSize(int cols,
                                               int rows_per_page) const {
  const gfx::Size tile_size = GetTileViewSize();
  return gfx::Size(tile_size.width() * cols,
                   tile_size.height() * rows_per_page);
}

gfx::Size AppsGridView::GetMaximumTileGridSize(int cols,
                                               int rows_per_page) const {
  const gfx::Size tile_size = GetTileViewSize();
  const int max_horizontal_spacing =
      features::IsProductivityLauncherEnabled()
          ? kMaximumHorizontalTileSpacingForProductivityLauncher
          : kMaximumTileSpacing;

  return gfx::Size(
      tile_size.width() * cols + max_horizontal_spacing * (cols - 1),
      tile_size.height() * rows_per_page +
          kMaximumTileSpacing * (rows_per_page - 1));
}

void AppsGridView::ResetForShowApps() {
  CancelDragWithNoDropAnimation();

  layer()->SetOpacity(1.0f);
  SetVisible(true);

  // The number of non-page-break-items should be the same as item views.
  if (item_list_) {
    int item_count = 0;
    for (size_t i = 0; i < item_list_->item_count(); ++i) {
      if (!item_list_->item_at(i)->is_page_break())
        ++item_count;
    }
    CHECK_EQ(item_count, view_model_.view_size());
  }
}

void AppsGridView::CancelDragWithNoDropAnimation() {
  EndDrag(/*cancel=*/true);
  drag_view_hider_.reset();
  folder_icon_item_hider_.reset();
  folder_to_open_after_drag_icon_animation_.clear();
  drag_icon_proxy_.reset();
}

void AppsGridView::DisableFocusForShowingActiveFolder(bool disabled) {
  for (const auto& entry : view_model_.entries())
    entry.view->SetEnabled(!disabled);

  // Ignore the grid view in accessibility tree so that items inside it will not
  // be accessed by ChromeVox.
  SetViewIgnoredForAccessibility(this, disabled);
}

void AppsGridView::SetModel(AppListModel* model) {
  if (model_)
    model_->RemoveObserver(this);

  model_ = model;
  if (model_)
    model_->AddObserver(this);

  Update();
}

void AppsGridView::SetItemList(AppListItemList* item_list) {
  DCHECK_GT(cols_, 0);
  DCHECK(app_list_config_);

  if (item_list_)
    item_list_->RemoveObserver(this);
  item_list_ = item_list;
  if (item_list_)
    item_list_->AddObserver(this);
  Update();
}

bool AppsGridView::IsInFolder() const {
  return !!folder_delegate_;
}

void AppsGridView::SetSelectedView(AppListItemView* view) {
  if (IsSelectedView(view) || IsDraggedView(view))
    return;

  GridIndex index = GetIndexOfView(view);
  if (IsValidIndex(index))
    SetSelectedItemByIndex(index);
}

void AppsGridView::ClearSelectedView() {
  selected_view_ = nullptr;
}

bool AppsGridView::IsSelectedView(const AppListItemView* view) const {
  return selected_view_ == view;
}

bool AppsGridView::InitiateDrag(AppListItemView* view,
                                const gfx::Point& location,
                                const gfx::Point& root_location,
                                base::OnceClosure drag_start_callback,
                                base::OnceClosure drag_end_callback) {
  DCHECK(view);
  if (drag_item_ || pulsing_blocks_model_.view_size())
    return false;
  DVLOG(1) << "Initiate drag";

  drag_start_callback_ = std::move(drag_start_callback);
  drag_end_callback_ = std::move(drag_end_callback);

  // Finalize previous drag icon animation if it's still in progress.
  drag_view_hider_.reset();
  folder_icon_item_hider_.reset();
  folder_to_open_after_drag_icon_animation_.clear();
  drag_icon_proxy_.reset();

  for (const auto& entry : view_model_.entries())
    static_cast<AppListItemView*>(entry.view)->EnsureLayer();
  drag_view_ = view;
  drag_item_ = view->item();

  // Dragged view should have focus. This also fixed the issue
  // https://crbug.com/834682.
  drag_view_->RequestFocus();
  drag_view_init_index_ = GetIndexOfView(drag_view_);
  reorder_placeholder_ = drag_view_init_index_;
  ExtractDragLocation(root_location, &drag_start_grid_view_);
  return true;
}

void AppsGridView::StartDragAndDropHostDragAfterLongPress() {
  TryStartDragAndDropHostDrag(TOUCH);
}

void AppsGridView::TryStartDragAndDropHostDrag(Pointer pointer) {
  // Stopping the animation may have invalidated our drag view due to the
  // view hierarchy changing.
  if (!drag_item_)
    return;

  drag_pointer_ = pointer;

  if (!dragging_for_reparent_item_)
    StartDragAndDropHostDrag();

  if (drag_start_callback_)
    std::move(drag_start_callback_).Run();
}

bool AppsGridView::UpdateDragFromItem(bool is_touch,
                                      const ui::LocatedEvent& event) {
  if (!drag_item_)
    return false;  // Drag canceled.

  gfx::Point drag_point_in_grid_view;
  ExtractDragLocation(event.root_location(), &drag_point_in_grid_view);
  const Pointer pointer = is_touch ? TOUCH : MOUSE;
  UpdateDrag(pointer, drag_point_in_grid_view);
  if (!IsDragging())
    return false;

  // If a drag and drop host is provided, see if the drag operation needs to be
  // forwarded.
  gfx::Point drag_point_in_screen = event.root_location();
  ::wm::ConvertPointToScreen(GetWidget()->GetNativeWindow()->GetRootWindow(),
                             &drag_point_in_screen);

  DispatchDragEventToDragAndDropHost(drag_point_in_screen);
  if (drag_icon_proxy_)
    drag_icon_proxy_->UpdatePosition(drag_point_in_screen);
  return true;
}

void AppsGridView::UpdateDrag(Pointer pointer, const gfx::Point& point) {
  if (folder_delegate_)
    UpdateDragStateInsideFolder(pointer, point);

  if (!drag_item_)
    return;  // Drag canceled.

  // If folder is currently open from the grid, delay drag updates until the
  // folder finishes closing.
  if (open_folder_info_) {
    // Only handle pointers that initiated the drag - e.g. ignore drag events
    // that come from touch if a mouse drag is currently in progress.
    if (drag_pointer_ == pointer)
      last_drag_point_ = point;
    return;
  }

  gfx::Vector2d drag_vector(point - drag_start_grid_view_);

  if (ExceededDragThreshold(drag_vector)) {
    if (!IsDragging())
      TryStartDragAndDropHostDrag(pointer);
    MaybeStartCardifiedView();
  }

  if (drag_pointer_ != pointer)
    return;

  last_drag_point_ = point;
  const GridIndex last_drop_target = drop_target_;
  DropTargetRegion last_drop_target_region = drop_target_region_;
  UpdateDropTargetRegion();

  MaybeStartPageFlip();

  bool is_scrolling = MaybeAutoScroll();
  if (is_scrolling) {
    // Don't do reordering while auto-scrolling, otherwise there is too much
    // motion during the drag.
    reorder_timer_.Stop();
    // Reset the previous drop target.
    if (last_drop_target_region == ON_ITEM)
      SetAsFolderDroppingTarget(last_drop_target, false);
    return;
  }

  if (last_drop_target != drop_target_ ||
      last_drop_target_region != drop_target_region_) {
    if (last_drop_target_region == ON_ITEM)
      SetAsFolderDroppingTarget(last_drop_target, false);
    if (drop_target_region_ == ON_ITEM && DraggedItemCanEnterFolder() &&
        DropTargetIsValidFolder()) {
      reorder_timer_.Stop();
      MaybeCreateFolderDroppingAccessibilityEvent();
      SetAsFolderDroppingTarget(drop_target_, true);
      BeginHideCurrentGhostImageView();
    } else if ((drop_target_region_ == ON_ITEM ||
                drop_target_region_ == NEAR_ITEM) &&
               !folder_delegate_) {
      // If the drag changes regions from |BETWEEN_ITEMS| to |NEAR_ITEM| the
      // timer should reset, so that we gain the extra time from hovering near
      // the item
      if (last_drop_target_region == BETWEEN_ITEMS)
        reorder_timer_.Stop();
      reorder_timer_.Start(FROM_HERE, base::Milliseconds(kReorderDelay * 5),
                           this, &AppsGridView::OnReorderTimer);
    } else if (drop_target_region_ != NO_TARGET) {
      // If none of the above cases evaluated true, then all of the possible
      // drop regions should result in a fast reorder.
      reorder_timer_.Start(FROM_HERE, base::Milliseconds(kReorderDelay), this,
                           &AppsGridView::OnReorderTimer);
    }
  }
}

void AppsGridView::EndDrag(bool cancel) {
  DVLOG(1) << "EndDrag cancel=" << cancel;

  // EndDrag was called before if |drag_view_| is nullptr.
  if (!drag_item_)
    return;

  AppListItem* drag_item = drag_item_;

  // Whether an icon was actually dragged (and not just clicked).
  const bool was_dragging = IsDragging();

  // Coming here a drag and drop was in progress.
  const bool landed_in_drag_and_drop_host =
      forward_events_to_drag_and_drop_host_;

  // The ID of the folder to which the item gets dropped. It will get set when
  // the item is moved to a folder.
  std::string target_folder_id;

  if (forward_events_to_drag_and_drop_host_) {
    DCHECK(!IsDraggingForReparentInRootLevelGridView());
    forward_events_to_drag_and_drop_host_ = false;
    // Pass the drag icon proxy on to the drag and drop host, so the drag and
    // drop host handles the animation to drop the icon proxy into correct spot.
    drag_and_drop_host_->EndDrag(cancel, std::move(drag_icon_proxy_));

    if (IsDraggingForReparentInHiddenGridView()) {
      EndDragForReparentInHiddenFolderGridView();
      folder_delegate_->DispatchEndDragEventForReparent(
          true /* events_forwarded_to_drag_drop_host */,
          cancel /* cancel_drag */, std::move(drag_icon_proxy_));
      return;
    }
  } else {
    if (IsDraggingForReparentInHiddenGridView()) {
      EndDragForReparentInHiddenFolderGridView();
      // Forward the EndDrag event to the root level grid view.
      folder_delegate_->DispatchEndDragEventForReparent(
          false /* events_forwarded_to_drag_drop_host */,
          cancel /* cancel_drag */, std::move(drag_icon_proxy_));
      return;
    }

    if (IsDraggingForReparentInRootLevelGridView()) {
      // An EndDrag can be received during a reparent via a model change. This
      // is always a cancel and needs to be forwarded to the folder.
      DCHECK(cancel);
      if (reparent_drag_cancellation_)
        std::move(reparent_drag_cancellation_).Run();
      return;
    }

    if (!cancel && was_dragging) {
      // Regular drag ending path, ie, not for reparenting.
      UpdateDropTargetRegion();
      if (drop_target_region_ == ON_ITEM && DraggedItemCanEnterFolder() &&
          DropTargetIsValidFolder()) {
        bool is_new_folder = false;
        if (MoveItemToFolder(drag_item_, drop_target_, kMoveByDragIntoFolder,
                             &target_folder_id, &is_new_folder)) {
          MaybeCreateFolderDroppingAccessibilityEvent();
          if (is_new_folder && features::IsProductivityLauncherEnabled()) {
            folder_to_open_after_drag_icon_animation_ = target_folder_id;
            SetOpenFolderInfo(target_folder_id, drop_target_,
                              reorder_placeholder_);
          }

          // If item drag created a folder, layout the grid to ensure the
          // created folder's bounds are correct. Note that `open_folder_info_`
          // affects ideal item bounds, so `Layout()` needs to be callsed after
          // `SetOpenFolderInfo()`.
          Layout();
        }
      } else if (IsValidReorderTargetIndex(drop_target_)) {
        // Ensure reorder event has already been announced by the end of drag.
        MaybeCreateDragReorderAccessibilityEvent();
        MoveItemInModel(drag_item_, drop_target_);
        RecordAppMovingTypeMetrics(folder_delegate_ ? kReorderByDragInFolder
                                                    : kReorderByDragInTopLevel);
      }
    }
  }

  // Issue 439055: MoveItemToFolder() can sometimes delete |drag_view_|
  if (drag_view_ && landed_in_drag_and_drop_host) {
    // Move the item directly to the target location, avoiding the
    // "zip back" animation if the user was pinning it to the shelf.
    int i = drop_target_.slot;
    gfx::Rect bounds = view_model_.ideal_bounds(i);
    drag_view_->SetBoundsRect(bounds);
    drag_view_hider_.reset();
  }

  SetAsFolderDroppingTarget(drop_target_, false);

  ClearDragState();
  UpdatePaging();
  if (GetWidget()) {
    // Normally Layout() cancels any animations. At this point there may be a
    // pending Layout(), force it now so that one isn't triggered part way
    // through the animation. Further, ignore this layout so that the position
    // isn't reset.
    DCHECK(!ignore_layout_);
    base::AutoReset<bool> auto_reset(&ignore_layout_, true);
    GetWidget()->LayoutRootViewIfNecessary();
  }
  if (cardified_state_)
    MaybeEndCardifiedView();
  else
    AnimateToIdealBounds();

  if (!cancel)
    view_structure_.SaveToMetadata();

  if (!cancel) {
    // Select the page where dragged item is dropped. Avoid doing so when the
    // dragged item ends up in a folder.
    const int model_index = GetModelIndexOfItem(drag_item);
    if (model_index < view_model_.view_size())
      EnsureViewVisible(view_structure_.GetIndexFromModelIndex(model_index));
  }

  // Hide the |current_ghost_view_| for item drag that started
  // within |apps_grid_view_|.
  BeginHideCurrentGhostImageView();
  if (was_dragging)
    SetFocusAfterEndDrag();  // Maybe focus the search box.

  AnimateDragIconToTargetPosition(drag_item, target_folder_id);
}

AppListItemView* AppsGridView::GetItemViewForItem(const std::string& item_id) {
  const AppListItem* const item = item_list_->FindItem(item_id);
  if (!item)
    return nullptr;

  return GetItemViewAt(GetModelIndexOfItem(item));
}

AppListItemView* AppsGridView::GetItemViewAt(int index) const {
  if (index < 0 || index >= view_model_.view_size())
    return nullptr;
  return view_model_.view_at(index);
}

void AppsGridView::InitiateDragFromReparentItemInRootLevelGridView(
    Pointer pointer,
    AppListItemView* original_drag_view,
    const gfx::Point& drag_point,
    base::OnceClosure cancellation_callback) {
  DVLOG(1) << __FUNCTION__;
  DCHECK(original_drag_view && !drag_view_);
  DCHECK(!dragging_for_reparent_item_);

  // Since the item is new, its placeholder is conceptually at the back of the
  // entire apps grid.
  reorder_placeholder_ = view_structure_.GetLastTargetIndex();

  for (const auto& entry : view_model_.entries())
    static_cast<AppListItemView*>(entry.view)->EnsureLayer();

  drag_pointer_ = pointer;
  drag_item_ = original_drag_view->item();
  drag_start_grid_view_ = drag_point;
  // Set the flag in root level grid view.
  dragging_for_reparent_item_ = true;
  reparent_drag_cancellation_ = std::move(cancellation_callback);
}

void AppsGridView::UpdateDragFromReparentItem(Pointer pointer,
                                              const gfx::Point& drag_point) {
  // Note that if a cancel ocurrs while reparenting, the |drag_view_| in both
  // root and folder grid views is cleared, so the check in UpdateDragFromItem()
  // for |drag_view_| being nullptr (in the folder grid) is sufficient.
  DCHECK(drag_item_);
  DCHECK(IsDraggingForReparentInRootLevelGridView());

  UpdateDrag(pointer, drag_point);
}

void AppsGridView::SetOpenFolderInfo(const std::string& folder_id,
                                     const GridIndex& target_folder_position,
                                     const GridIndex& position_to_skip) {
  GridIndex expected_folder_position = target_folder_position;
  // If the target view is positioned after `position_to_skip`, move the
  // target one slot earlier, as `position_to_skip` is assumed about to be
  // emptied.
  if (position_to_skip.IsValid() &&
      position_to_skip < expected_folder_position &&
      expected_folder_position.slot > 0) {
    --expected_folder_position.slot;
  }

  open_folder_info_ = {.item_id = folder_id,
                       .grid_index = expected_folder_position};
}

void AppsGridView::ShowFolderForView(AppListItemView* folder_view,
                                     bool new_folder) {
  DCHECK(open_folder_info_);

  // Guard against invalid folder view.
  if (!folder_view || !folder_view->is_folder()) {
    open_folder_info_.reset();
    return;
  }

  folder_controller_->ShowFolderForItemView(
      folder_view,
      /*focus_name_input=*/new_folder,
      base::BindOnce(&AppsGridView::FolderHidden, weak_factory_.GetWeakPtr(),
                     folder_view->item()->id()));
}

void AppsGridView::FolderHidden(const std::string& item_id) {
  if (!open_folder_info_ || open_folder_info_->item_id != item_id)
    return;

  // Find the folder item location in the app list model to determine whether
  // the item view location changed while the folder was closed (in which case
  // the folder location change should be animated).
  AppListItemView* item_view = nullptr;
  int model_index = -1;
  for (int i = 0; i < view_model_.view_size(); ++i) {
    AppListItemView* view = view_model_.view_at(i);
    if (view == drag_view_)
      continue;

    ++model_index;
    if (view->item()->id() == item_id) {
      item_view = view;
      break;
    }
  }

  // If the item view is gone, or the location in the grid did not change,
  // the folder item should not be animated - immediately update apps grid state
  // for folder hide.
  if (!item_view || view_structure_.GetIndexFromModelIndex(model_index) ==
                        open_folder_info_->grid_index) {
    open_folder_info_.reset();
    OnFolderHideAnimationDone();
    return;
  }

  // When folder animates out, remaining items will animate to their ideal
  // bounds - ensure their layers are created (and marked not to fill bounds
  // opaquely).
  for (int i = 0; i < view_model_.view_size(); ++i)
    view_model_.view_at(i)->EnsureLayer();

  // Animate the folder item view out from its original location.
  reordering_folder_view_ = item_view;
  views::AnimationBuilder animation;
  animation.OnEnded(base::BindOnce(&AppsGridView::AnimateFolderItemViewIn,
                                   weak_factory_.GetWeakPtr()));
  animation.OnAborted(base::BindOnce(&AppsGridView::AnimateFolderItemViewIn,
                                     weak_factory_.GetWeakPtr()));

  gfx::Transform scale;
  scale.Scale(0.5, 0.5);
  scale = gfx::TransformAboutPivot(item_view->GetLocalBounds().CenterPoint(),
                                   scale);
  animation.Once()
      .SetDuration(kFolderItemFadeOutDuration)
      .SetTransform(item_view->layer(), scale, gfx::Tween::FAST_OUT_LINEAR_IN)
      .SetOpacity(item_view->layer(), 0.0f, gfx::Tween::FAST_OUT_LINEAR_IN);
}

void AppsGridView::AnimateFolderItemViewIn() {
  // Once folder item view fades out, animate remaining items into their target
  // location, and schedule the folder item view fade-in (note that
  // `AnimateToIdealBounds()` updates `reordering_folder_view_` bounds without
  // animation).
  open_folder_info_.reset();
  AnimateToIdealBounds();

  if (!reordering_folder_view_)
    return;

  views::AnimationBuilder()
      .OnEnded(base::BindOnce(&AppsGridView::OnFolderHideAnimationDone,
                              weak_factory_.GetWeakPtr()))
      .OnAborted(base::BindOnce(&AppsGridView::OnFolderHideAnimationDone,
                                weak_factory_.GetWeakPtr()))
      .Once()
      .At(kFolderItemFadeInDelay)
      .SetDuration(kFolderItemFadeInDuration)
      .SetTransform(reordering_folder_view_.value()->layer(), gfx::Transform(),
                    gfx::Tween::ACCEL_LIN_DECEL_100_3)
      .SetOpacity(reordering_folder_view_.value()->layer(), 1.0f,
                  gfx::Tween::ACCEL_LIN_DECEL_100_3);
}

void AppsGridView::OnFolderHideAnimationDone() {
  reordering_folder_view_.reset();
  OnBoundsAnimatorDone(nullptr);
  if (IsDraggingForReparentInRootLevelGridView()) {
    MaybeStartCardifiedView();
    UpdateDrag(drag_pointer_, last_drag_point_);
  }
}

bool AppsGridView::IsDragging() const {
  return drag_pointer_ != NONE;
}

bool AppsGridView::IsDraggedView(const AppListItemView* view) const {
  return drag_item_ == view->item();
}

void AppsGridView::ClearDragState() {
  current_ghost_location_ = GridIndex();
  last_folder_dropping_a11y_event_location_ = GridIndex();
  last_reorder_a11y_event_location_ = GridIndex();
  drop_target_region_ = NO_TARGET;
  drag_pointer_ = NONE;
  drop_target_ = GridIndex();
  reorder_placeholder_ = GridIndex();
  drag_start_grid_view_ = gfx::Point();

  // Drag may end before |host_drag_start_timer_| gets fired.
  if (host_drag_start_timer_.IsRunning())
    host_drag_start_timer_.AbandonAndStop();

  if (folder_item_reparent_timer_.IsRunning())
    folder_item_reparent_timer_.Stop();

  MaybeStopPageFlip();
  StopAutoScroll();

  drag_view_ = nullptr;
  drag_item_ = nullptr;
  drag_out_of_folder_container_ = false;
  dragging_for_reparent_item_ = false;
  extra_page_opened_ = false;
  reparent_drag_cancellation_.Reset();

  drag_start_callback_.Reset();
  if (drag_end_callback_)
    std::move(drag_end_callback_).Run();
}

void AppsGridView::SetDragAndDropHostOfCurrentAppList(
    ApplicationDragAndDropHost* drag_and_drop_host) {
  drag_and_drop_host_ = drag_and_drop_host;
}

bool AppsGridView::IsAnimatingView(AppListItemView* view) {
  return bounds_animator_->IsAnimating(view);
}

gfx::Size AppsGridView::CalculatePreferredSize() const {
  return GetTileGridSize();
}

bool AppsGridView::GetDropFormats(
    int* formats,
    std::set<ui::ClipboardFormatType>* format_types) {
  // TODO(koz): Only accept a specific drag type for app shortcuts.
  *formats = OSExchangeData::FILE_NAME;
  return true;
}

bool AppsGridView::CanDrop(const OSExchangeData& data) {
  return true;
}

int AppsGridView::OnDragUpdated(const ui::DropTargetEvent& event) {
  return ui::DragDropTypes::DRAG_MOVE;
}

void AppsGridView::UpdateControlVisibility(AppListViewState app_list_state,
                                           bool is_in_drag) {
  const bool fullscreen_or_in_drag =
      is_in_drag || app_list_state == AppListViewState::kFullscreenAllApps ||
      app_list_state == AppListViewState::kFullscreenSearch;
  SetVisible(fullscreen_or_in_drag);
}

bool AppsGridView::OnKeyPressed(const ui::KeyEvent& event) {
  // The user may press VKEY_CONTROL before an arrow key when intending to do an
  // app move with control+arrow.
  if (event.key_code() == ui::VKEY_CONTROL)
    return true;

  if (selected_view_ && IsArrowKeyEvent(event) && event.IsControlDown()) {
    HandleKeyboardAppOperations(event.key_code(), event.IsShiftDown());
    return true;
  }

  // Let the FocusManager handle Left/Right keys.
  if (!IsUnhandledUpDownKeyEvent(event))
    return false;

  const bool arrow_up = event.key_code() == ui::VKEY_UP;
  return HandleVerticalFocusMovement(arrow_up);
}

bool AppsGridView::OnKeyReleased(const ui::KeyEvent& event) {
  if (event.IsControlDown() || !handling_keyboard_move_)
    return false;

  handling_keyboard_move_ = false;
  RecordAppMovingTypeMetrics(folder_delegate_ ? kReorderByKeyboardInFolder
                                              : kReorderByKeyboardInTopLevel);
  return false;
}

void AppsGridView::ViewHierarchyChanged(
    const views::ViewHierarchyChangedDetails& details) {
  if (!details.is_add && details.parent == items_container_) {
    // The view being delete should not have reference in |view_model_|.
    CHECK_EQ(-1, view_model_.GetIndexOfView(details.child));

    if (selected_view_ == details.child)
      selected_view_ = nullptr;

    if (drag_view_ == details.child)
      drag_view_ = nullptr;

    if (features::IsProductivityLauncherEnabled()) {
      if (current_ghost_view_ == details.child)
        current_ghost_view_ = nullptr;
      if (last_ghost_view_ == details.child)
        last_ghost_view_ = nullptr;
    }

    if (reordering_folder_view_ && *reordering_folder_view_ == details.child)
      reordering_folder_view_.reset();

    bounds_animator_->StopAnimatingView(details.child);
  }
}

bool AppsGridView::EventIsBetweenOccupiedTiles(const ui::LocatedEvent* event) {
  gfx::Point mirrored_point(GetMirroredXInView(event->location().x()),
                            event->location().y());
  return IsValidIndex(GetNearestTileIndexForPoint(mirrored_point));
}

void AppsGridView::Update() {
  UpdateBorder();

  // Abort reorder animation before `view_model_` is cleared.
  MaybeAbortReorderAnimation();

  view_model_.Clear();
  pulsing_blocks_model_.Clear();
  items_container_->RemoveAllChildViews();

  DCHECK(!selected_view_);
  DCHECK(!drag_view_);

  std::vector<AppListItemView*> item_views;
  if (item_list_ && item_list_->item_count()) {
    for (size_t i = 0; i < item_list_->item_count(); ++i) {
      // Skip "page break" items.
      if (item_list_->item_at(i)->is_page_break())
        continue;
      std::unique_ptr<AppListItemView> view = CreateViewForItemAtIndex(i);
      view_model_.Add(view.get(), view_model_.view_size());
      item_views.push_back(items_container_->AddChildView(std::move(view)));
    }
  }
  view_structure_.LoadFromMetadata();
  UpdateColsAndRowsForFolder();
  UpdatePaging();
  UpdatePulsingBlockViews();
  InvalidateLayout();

  // Icon load can change the item position in the view model, so don't iterate
  // over view model to get items to update.
  for (auto* item_view : item_views)
    item_view->InitializeIconLoader();

  if (!folder_delegate_)
    RecordPageMetrics();
}

void AppsGridView::UpdatePulsingBlockViews() {
  const int existing_items = item_list_ ? item_list_->item_count() : 0;
  const int tablet_page_size =
      SharedAppListConfig::instance().GetMaxNumOfItemsPerPage();
  // For scrolling app list, the "page size" is very large, so cap the number of
  // pulsing blocks to the size of the tablet mode page (~20 items).
  const int tiles_per_page = std::min(TilesPerPage(0), tablet_page_size);
  const int available_slots =
      tiles_per_page - (existing_items % tiles_per_page);
  const int desired =
      model_ && model_->status() == AppListModelStatus::kStatusSyncing
          ? available_slots
          : 0;

  if (pulsing_blocks_model_.view_size() == desired)
    return;

  while (pulsing_blocks_model_.view_size() > desired) {
    PulsingBlockView* view = pulsing_blocks_model_.view_at(0);
    pulsing_blocks_model_.Remove(0);
    delete view;
  }

  while (pulsing_blocks_model_.view_size() < desired) {
    auto view = std::make_unique<PulsingBlockView>(
        GetTotalTileSize(GetTotalPages() - 1), true);
    pulsing_blocks_model_.Add(view.get(), 0);
    items_container_->AddChildView(std::move(view));
  }
}

std::unique_ptr<AppListItemView> AppsGridView::CreateViewForItemAtIndex(
    size_t index) {
  // The |drag_view_| might be pending for deletion, therefore |view_model_|
  // may have one more item than |item_list_|.
  DCHECK_LE(index, item_list_->item_count());
  auto view = std::make_unique<AppListItemView>(
      app_list_config_, this, item_list_->item_at(index),
      app_list_view_delegate_, AppListItemView::Context::kAppsGridView);
  if (ItemViewsRequireLayers())
    view->EnsureLayer();
  if (cardified_state_)
    view->EnterCardifyState();
  return view;
}

void AppsGridView::SetSelectedItemByIndex(const GridIndex& index) {
  if (GetIndexOfView(selected_view_) == index)
    return;

  AppListItemView* new_selection = GetViewAtIndex(index);
  if (!new_selection)
    return;  // Keep current selection.

  if (selected_view_)
    selected_view_->SchedulePaint();

  EnsureViewVisible(index);
  selected_view_ = new_selection;
  selected_view_->SchedulePaint();
  selected_view_->NotifyAccessibilityEvent(ax::mojom::Event::kFocus, true);
  if (selected_view_->HasNotificationBadge()) {
    a11y_announcer_->AnnounceItemNotificationBadge(
        selected_view_->title()->GetText());
  }
}

GridIndex AppsGridView::GetIndexOfView(const AppListItemView* view) const {
  const int model_index = view_model_.GetIndexOfView(view);
  if (model_index == -1)
    return GridIndex();

  return view_structure_.GetIndexFromModelIndex(model_index);
}

AppListItemView* AppsGridView::GetViewAtIndex(const GridIndex& index) const {
  if (!IsValidIndex(index))
    return nullptr;

  const int model_index = view_structure_.GetModelIndexFromIndex(index);
  return GetItemViewAt(model_index);
}

int AppsGridView::TilesPerPage(int page) const {
  const int max_rows = GetMaxRowsInPage(page);

  // In folders, the grid size depends on the number of items in the page.
  if (IsInFolder()) {
    // Leave room for at least one item.
    if (!view_model()->view_size())
      return 1;

    int rows = (view_model()->view_size() - 1) / cols() + 1;
    return std::min(max_rows, rows) * cols();
  }

  return max_rows * cols();
}

void AppsGridView::SetMaxColumnsInternal(int max_cols) {
  if (max_cols_ == max_cols)
    return;

  max_cols_ = max_cols;

  if (IsInFolder()) {
    UpdateColsAndRowsForFolder();
  } else {
    cols_ = max_cols_;
  }
}

void AppsGridView::SetIdealBoundsForViewToGridIndex(
    int view_index_in_model,
    const GridIndex& view_grid_index) {
  gfx::Rect tile_bounds = GetExpectedTileBounds(view_grid_index);
  tile_bounds.Offset(CalculateTransitionOffset(view_grid_index.page));
  if (view_index_in_model < view_model_.view_size()) {
    view_model_.set_ideal_bounds(view_index_in_model, tile_bounds);
  } else {
    pulsing_blocks_model_.set_ideal_bounds(
        view_index_in_model - view_model_.view_size(), tile_bounds);
  }
}

void AppsGridView::CalculateIdealBounds() {
  if (view_structure_.mode() == PagedViewStructure::Mode::kPartialPages) {
    CalculateIdealBoundsForPageStructureWithPartialPages();
    return;
  }

  AppListItemView* view_with_locked_position = nullptr;
  if (open_folder_info_)
    view_with_locked_position = GetItemViewForItem(open_folder_info_->item_id);

  std::set<GridIndex> reserved_slots;
  reserved_slots.insert(reorder_placeholder_);
  if (open_folder_info_) {
    reserved_slots.insert(open_folder_info_->grid_index);
  }

  const int total_views =
      view_model_.view_size() + pulsing_blocks_model_.view_size();
  int slot_index = 0;
  for (int i = 0; i < total_views; ++i) {
    if (i < view_model_.view_size() && view_model_.view_at(i) == drag_view_) {
      continue;
    }

    if (i < view_model_.view_size() &&
        view_model_.view_at(i) == view_with_locked_position) {
      SetIdealBoundsForViewToGridIndex(i, open_folder_info_->grid_index);
      continue;
    }

    GridIndex view_index = view_structure_.GetIndexFromModelIndex(slot_index);

    // Leaves a blank space in the grid for the current reorder placeholder.
    while (reserved_slots.count(view_index)) {
      ++slot_index;
      view_index = view_structure_.GetIndexFromModelIndex(slot_index);
    }

    SetIdealBoundsForViewToGridIndex(i, view_index);
    ++slot_index;
  }
}

void AppsGridView::CalculateIdealBoundsForPageStructureWithPartialPages() {
  DCHECK(!IsInFolder());
  DCHECK_EQ(view_structure_.mode(), PagedViewStructure::Mode::kPartialPages);

  // |view_structure_| should only be updated at the end of drag. So make a
  // copy of it and only change the copy for calculating the ideal bounds of
  // each item view.
  PagedViewStructure copied_view_structure(this);
  // Allow empty pages in the copied view structure so an app list page does
  // not get removed when dragging the last item in the page.
  copied_view_structure.AllowEmptyPages();

  {
    // Delay page overflow sanitization until both drag view was removed, and
    // reorder placeholder was added to the view structure.
    std::unique_ptr<PagedViewStructure::ScopedSanitizeLock> sanitize_lock =
        copied_view_structure.GetSanitizeLock();
    copied_view_structure.LoadFromOther(view_structure_);

    // Remove the item view being dragged.
    if (drag_view_)
      copied_view_structure.Remove(drag_view_);

    // Leave a blank space in the grid for the current reorder placeholder.
    if (IsValidIndex(reorder_placeholder()))
      copied_view_structure.Add(nullptr, reorder_placeholder());
  }

  // Convert visual index to ideal bounds.
  const auto& pages = copied_view_structure.pages();
  int model_index = 0;
  for (size_t i = 0; i < pages.size(); ++i) {
    auto& page = pages[i];
    for (size_t j = 0; j < page.size(); ++j) {
      if (page[j] == nullptr)
        continue;

      // Skip the dragged view
      if (view_model()->view_at(model_index) == drag_view_)
        ++model_index;

      gfx::Rect tile_slot = GetExpectedTileBounds(GridIndex(i, j));
      tile_slot.Offset(CalculateTransitionOffset(i));
      view_model()->set_ideal_bounds(model_index, tile_slot);
      ++model_index;
    }
  }

  // All pulsing blocks come after item views.
  GridIndex pulsing_block_index = copied_view_structure.GetLastTargetIndex();
  for (int i = 0; i < pulsing_blocks_model().view_size(); ++i) {
    if (pulsing_block_index.slot == TilesPerPage(pulsing_block_index.page)) {
      ++pulsing_block_index.page;
      pulsing_block_index.slot = 0;
    }
    gfx::Rect tile_slot = GetExpectedTileBounds(pulsing_block_index);
    tile_slot.Offset(CalculateTransitionOffset(pulsing_block_index.page));
    pulsing_blocks_model().set_ideal_bounds(i, tile_slot);
    ++pulsing_block_index.slot;
  }
}

void AppsGridView::AnimateToIdealBounds() {
  gfx::Rect visible_bounds(GetVisibleBounds());
  gfx::Point visible_origin = visible_bounds.origin();
  ConvertPointToTarget(this, items_container_, &visible_origin);
  visible_bounds.set_origin(visible_origin);

  CalculateIdealBounds();
  for (int i = 0; i < view_model_.view_size(); ++i) {
    AppListItemView* view = GetItemViewAt(i);
    const gfx::Rect& target = view_model_.ideal_bounds(i);
    if (bounds_animator_->GetTargetBounds(view) == target)
      continue;

    const gfx::Rect& current = view->bounds();
    const bool current_visible = visible_bounds.Intersects(current);
    const bool target_visible = visible_bounds.Intersects(target);
    const bool visible = !IsViewHiddenForFolderReorder(view) &&
                         !IsViewHiddenForDrag(view) &&
                         (current_visible || target_visible);

    const int y_diff = target.y() - current.y();
    const int tile_size_height =
        GetTotalTileSize(view_structure_.GetIndexFromModelIndex(i).page)
            .height();
    if (visible && y_diff && y_diff % tile_size_height == 0) {
      AnimationBetweenRows(view, current_visible, current, target_visible,
                           target);
    } else if (visible || bounds_animator_->IsAnimating(view)) {
      bounds_animator_->AnimateViewTo(view, target);
      bounds_animator_->SetAnimationDelegate(view, nullptr);
    } else {
      view->SetBoundsRect(target);
    }
  }

  // Destroy layers created for drag if they're not longer necessary.
  if (!bounds_animator_->IsAnimating())
    OnBoundsAnimatorDone(bounds_animator_.get());
}

void AppsGridView::AnimationBetweenRows(AppListItemView* view,
                                        bool animate_current,
                                        const gfx::Rect& current,
                                        bool animate_target,
                                        const gfx::Rect& target) {
  // Determine page of |current| and |target|.
  const int current_page =
      CompareHorizontalPointPositionToRect(current.origin(), GetLocalBounds());
  const int target_page =
      CompareHorizontalPointPositionToRect(target.origin(), GetLocalBounds());

  std::unique_ptr<ui::Layer> layer;
  if (view->layer()) {
    if (animate_current) {
      layer = view->RecreateLayer();
      layer->SuppressPaint();

      view->layer()->SetFillsBoundsOpaquely(false);
      view->layer()->SetOpacity(0.f);
    }
  } else {
    view->EnsureLayer();
  }

  const gfx::Size total_tile_size = GetTotalTileSize(current_page);
  int dir = current_page < target_page ||
                    (current_page == target_page && current.y() < target.y())
                ? 1
                : -1;

  gfx::Rect target_in(target);
  if (animate_target)
    target_in.Offset(-dir * total_tile_size.width(), 0);
  bounds_animator_->StopAnimatingView(view);
  view->SetBoundsRect(target_in);
  bounds_animator_->AnimateViewTo(view, target);

  // Flip the direction for the layer move out animation if rtl mode is used.
  dir = base::i18n::IsRTL() ? -dir : dir;
  bounds_animator_->SetAnimationDelegate(
      view, std::make_unique<RowMoveAnimationDelegate>(
                view, layer.release(),
                gfx::Vector2d(dir * total_tile_size.width(), 0)));
}

void AppsGridView::ExtractDragLocation(const gfx::Point& root_location,
                                       gfx::Point* drag_point) {
  // Use root location of |event| instead of location in |drag_view_|'s
  // coordinates because |drag_view_| has a scale transform and location
  // could have integer round error and causes jitter.
  *drag_point = root_location;

  DCHECK(GetWidget());
  aura::Window::ConvertPointToTarget(
      GetWidget()->GetNativeWindow()->GetRootWindow(),
      GetWidget()->GetNativeWindow(), drag_point);
  views::View::ConvertPointFromWidget(this, drag_point);
}

void AppsGridView::UpdateDropTargetRegion() {
  DCHECK(drag_item_);

  gfx::Point point = last_drag_point_;
  point.set_x(GetMirroredXInView(point.x()));

  if (IsPointWithinDragBuffer(point)) {
    if (DragPointIsOverItem(point)) {
      drop_target_region_ = ON_ITEM;
      drop_target_ = GetNearestTileIndexForPoint(point);
      return;
    }

    UpdateDropTargetForReorder(point);
    drop_target_region_ = DragIsCloseToItem(point) ? NEAR_ITEM : BETWEEN_ITEMS;
    return;
  }

  // Reset the reorder target to the original position if the cursor is outside
  // the drag buffer or an item is dragged to a full page either from a folder
  // or another page.
  if (IsDraggingForReparentInRootLevelGridView()) {
    drop_target_region_ = NO_TARGET;
    return;
  }

  drop_target_ = drag_view_init_index_;
  drop_target_region_ = DragIsCloseToItem(point) ? NEAR_ITEM : BETWEEN_ITEMS;
}

bool AppsGridView::DropTargetIsValidFolder() {
  AppListItemView* target_view =
      GetViewDisplayedAtSlotOnCurrentPage(drop_target_.slot);
  if (!target_view)
    return false;

  AppListItem* target_item = target_view->item();

  // Items can only be dropped into non-folders (which have no children) or
  // folders that have fewer than the max allowed items.
  // The OEM folder does not allow drag/drop of other items into it.
  if (target_item->IsFolderFull() || IsOEMFolderItem(target_item))
    return false;

  if (!IsValidIndex(drop_target_))
    return false;

  return true;
}

bool AppsGridView::DragPointIsOverItem(const gfx::Point& point) {
  // The reorder placeholder shouldn't count as a unique item
  GridIndex nearest_tile_index(GetNearestTileIndexForPoint(point));
  if (!IsValidIndex(nearest_tile_index) ||
      nearest_tile_index == reorder_placeholder_) {
    return false;
  }

  int distance_to_tile_center =
      (point - GetExpectedTileBounds(nearest_tile_index).CenterPoint())
          .Length();
  if (distance_to_tile_center >
      (app_list_config_->folder_dropping_circle_radius() *
       (cardified_state_ ? GetAppsGridCardifiedScale() : 1.0f))) {
    return false;
  }

  return true;
}

void AppsGridView::AnimateDragIconToTargetPosition(
    AppListItem* drag_item,
    const std::string& target_folder_id) {
  // If drag icon proxy had not been created, just reshow the drag view.
  if (!drag_icon_proxy_) {
    OnDragIconDropDone();
    return;
  }

  AppListItemView* target_folder_view =
      !target_folder_id.empty() ? GetItemViewForItem(target_folder_id)
                                : nullptr;

  // Calculate target item bounds.
  gfx::Rect drag_icon_drop_bounds;
  if (target_folder_id.empty()) {
    // Find the view for drag item, and use its ideal bounds to calculate target
    // drop bounds.
    for (int i = 0; i < view_model_.view_size(); ++i) {
      if (view_model_.view_at(i)->item() != drag_item)
        continue;

      auto* drag_view = view_model_.view_at(i);
      // Get icon bounds in the drag view coordinates.
      drag_icon_drop_bounds = drag_view->GetIconBounds();

      // Get the expected drag item view location.
      const gfx::Rect drag_view_ideal_bounds = view_model_.ideal_bounds(i);

      // Position target icon bounds relative to the ideal drag view bounds.
      drag_icon_drop_bounds.Offset(drag_view_ideal_bounds.x(),
                                   drag_view_ideal_bounds.y());
      break;
    }
  } else if (target_folder_view) {
    // Calculate target bounds of dragged item.
    drag_icon_drop_bounds =
        GetTargetIconRectInFolder(drag_item, target_folder_view);
  }

  // Unable to calculate target bounds - bail out and reshow the drag view.
  if (drag_icon_drop_bounds.IsEmpty()) {
    OnDragIconDropDone();
    return;
  }

  if (target_folder_view) {
    DCHECK(target_folder_view->is_folder());
    folder_icon_item_hider_ = std::make_unique<FolderIconItemHider>(
        static_cast<AppListFolderItem*>(target_folder_view->item()), drag_item);
  }

  drag_icon_drop_bounds =
      items_container_->GetMirroredRect(drag_icon_drop_bounds);
  // Convert target bounds to in screen coordinates expected by drag icon proxy.
  views::View::ConvertRectToScreen(items_container_, &drag_icon_drop_bounds);

  drag_icon_proxy_->AnimateToBoundsAndCloseWidget(
      drag_icon_drop_bounds, base::BindOnce(&AppsGridView::OnDragIconDropDone,
                                            base::Unretained(this)));
}

void AppsGridView::OnDragIconDropDone() {
  drag_view_hider_.reset();
  folder_icon_item_hider_.reset();
  drag_icon_proxy_.reset();
  OnBoundsAnimatorDone(nullptr);

  if (!folder_to_open_after_drag_icon_animation_.empty()) {
    AppListItemView* folder_view =
        GetItemViewForItem(folder_to_open_after_drag_icon_animation_);
    folder_to_open_after_drag_icon_animation_.clear();
    ShowFolderForView(folder_view, /*new_folder=*/true);
  }
}

bool AppsGridView::DraggedItemCanEnterFolder() {
  if (!IsFolderItem(drag_item_) && !folder_delegate_)
    return true;
  return false;
}

void AppsGridView::UpdateDropTargetForReorder(const gfx::Point& point) {
  gfx::Rect bounds = GetContentsBounds();
  bounds.Inset(GetTilePadding(GetSelectedPage()));
  GridIndex nearest_tile_index = GetNearestTileIndexForPoint(point);
  gfx::Point reorder_placeholder_center =
      GetExpectedTileBounds(reorder_placeholder_).CenterPoint();

  int x_offset_direction = 0;
  if (nearest_tile_index == reorder_placeholder_) {
    x_offset_direction = reorder_placeholder_center.x() <= point.x() ? -1 : 1;
  } else {
    x_offset_direction = reorder_placeholder_ < nearest_tile_index ? -1 : 1;
  }

  const gfx::Size total_tile_size = GetTotalTileSize(GetSelectedPage());
  int row = nearest_tile_index.slot / cols_;

  // Offset the target column based on the direction of the target. This will
  // result in earlier targets getting their reorder zone shifted backwards
  // and later targets getting their reorder zones shifted forwards.
  //
  // This makes reordering feel like the user is slotting items into the spaces
  // between apps.
  int x_offset = x_offset_direction *
                 (total_tile_size.width() / 2 -
                  app_list_config_->folder_dropping_circle_radius() *
                      (cardified_state_ ? GetAppsGridCardifiedScale() : 1.0f));
  const int selected_page = GetSelectedPage();
  int col = (point.x() - bounds.x() + x_offset -
             GetGridCenteringOffset(selected_page).x()) /
            total_tile_size.width();
  col = base::clamp(col, 0, cols_ - 1);
  drop_target_ =
      std::min(GridIndex(selected_page, row * cols_ + col),
               view_structure_.GetLastTargetIndexOfPage(selected_page));

  DCHECK(IsValidReorderTargetIndex(drop_target_))
      << drop_target_.ToString() << " selected page " << selected_page
      << " row " << row << " col " << col << " "
      << view_structure_.GetLastTargetIndexOfPage(drop_target_.page).ToString();
}

bool AppsGridView::DragIsCloseToItem(const gfx::Point& point) {
  DCHECK(drag_item_);

  GridIndex nearest_tile_index = GetNearestTileIndexForPoint(point);
  if (nearest_tile_index == reorder_placeholder_)
    return false;

  const int distance_to_tile_center =
      (point - GetExpectedTileBounds(nearest_tile_index).CenterPoint())
          .Length();

  // The minimum of |forty_percent_icon_spacing| and |double_icon_radius| is
  // chosen to give an acceptable spacing on displays of any resolution: when
  // items are very close together, using |forty_percent_icon_spacing| will
  // prevent overlap and leave a reasonable gap, whereas when icons are very far
  // apart, using |double_icon_radius| will prevent us from juding an overly
  // large region as 'nearby'
  const int forty_percent_icon_spacing =
      (app_list_config_->grid_tile_width() + horizontal_tile_padding_ * 2) *
      0.4 * (cardified_state_ ? GetAppsGridCardifiedScale() : 1.0f);
  const int double_icon_radius =
      app_list_config_->folder_dropping_circle_radius() * 2 *
      (cardified_state_ ? GetAppsGridCardifiedScale() : 1.0f);
  const int minimum_drag_distance_for_reorder =
      std::min(forty_percent_icon_spacing, double_icon_radius);

  if (distance_to_tile_center < minimum_drag_distance_for_reorder)
    return true;
  return false;
}

void AppsGridView::OnReorderTimer() {
  reorder_placeholder_ = drop_target_;
  MaybeCreateDragReorderAccessibilityEvent();
  AnimateToIdealBounds();
  CreateGhostImageView();
}

void AppsGridView::OnFolderItemReparentTimer(Pointer pointer) {
  DCHECK(folder_delegate_);
  if (drag_out_of_folder_container_ && drag_view_) {
    folder_delegate_->ReparentItem(pointer, drag_view_, last_drag_point_);

    // Set the flag in the folder's grid view.
    dragging_for_reparent_item_ = true;

    // Do not observe any data change since it is going to be hidden.
    item_list_->RemoveObserver(this);
    item_list_ = nullptr;
  }
}

void AppsGridView::UpdateDragStateInsideFolder(Pointer pointer,
                                               const gfx::Point& drag_point) {
  if (IsUnderOEMFolder())
    return;

  if (IsDraggingForReparentInHiddenGridView()) {
    // Dispatch drag event to root level grid view for re-parenting folder
    // folder item purpose.
    DispatchDragEventForReparent(pointer, drag_point);
    return;
  }

  // Calculate if the drag_view_ is dragged out of the folder's container
  // ink bubble.
  bool is_item_dragged_out_of_folder =
      folder_delegate_->IsDragPointOutsideOfFolder(drag_point);
  if (is_item_dragged_out_of_folder) {
    if (!drag_out_of_folder_container_) {
      folder_item_reparent_timer_.Start(
          FROM_HERE, base::Milliseconds(kFolderItemReparentDelay),
          base::BindOnce(&AppsGridView::OnFolderItemReparentTimer,
                         base::Unretained(this), pointer));
      drag_out_of_folder_container_ = true;
    }
  } else {
    folder_item_reparent_timer_.Stop();
    drag_out_of_folder_container_ = false;
  }
}

bool AppsGridView::IsDraggingForReparentInRootLevelGridView() const {
  return (!folder_delegate_ && dragging_for_reparent_item_);
}

bool AppsGridView::IsDraggingForReparentInHiddenGridView() const {
  return (folder_delegate_ && dragging_for_reparent_item_);
}

gfx::Rect AppsGridView::GetTargetIconRectInFolder(
    AppListItem* drag_item,
    AppListItemView* folder_item_view) {
  const gfx::Rect view_ideal_bounds =
      view_model_.ideal_bounds(view_model_.GetIndexOfView(folder_item_view));
  const gfx::Rect icon_ideal_bounds =
      folder_item_view->GetIconBoundsForTargetViewBounds(
          app_list_config_, view_ideal_bounds,
          folder_item_view->GetIconImage().size(), /*icon_scale=*/1.0f);
  AppListFolderItem* folder_item =
      static_cast<AppListFolderItem*>(folder_item_view->item());
  return folder_item->GetTargetIconRectInFolderForItem(
      *app_list_config_, drag_item, icon_ideal_bounds);
}

bool AppsGridView::IsUnderOEMFolder() {
  if (!folder_delegate_)
    return false;

  return folder_delegate_->IsOEMFolder();
}

void AppsGridView::HandleKeyboardAppOperations(ui::KeyboardCode key_code,
                                               bool folder) {
  DCHECK(selected_view_);

  if (folder) {
    if (folder_delegate_)
      folder_delegate_->HandleKeyboardReparent(selected_view_, key_code);
    else
      HandleKeyboardFoldering(key_code);
  } else {
    HandleKeyboardMove(key_code);
  }
}

void AppsGridView::HandleKeyboardFoldering(ui::KeyboardCode key_code) {
  const GridIndex source_index = GetIndexOfView(selected_view_);
  const GridIndex target_index = GetTargetGridIndexForKeyboardMove(key_code);
  if (!CanMoveSelectedToTargetForKeyboardFoldering(target_index))
    return;

  const std::u16string moving_view_title = selected_view_->title()->GetText();
  AppListItemView* target_view =
      GetViewDisplayedAtSlotOnCurrentPage(target_index.slot);
  const std::u16string target_view_title = target_view->title()->GetText();
  const bool target_view_is_folder = target_view->is_folder();

  std::string folder_id;
  bool is_new_folder = false;
  if (MoveItemToFolder(selected_view_->item(), target_index,
                       kMoveByKeyboardIntoFolder, &folder_id, &is_new_folder)) {
    a11y_announcer_->AnnounceKeyboardFoldering(
        moving_view_title, target_view_title, target_view_is_folder);
    AppListItemView* folder_view = GetItemViewForItem(folder_id);
    if (folder_view) {
      if (is_new_folder && features::IsProductivityLauncherEnabled()) {
        SetOpenFolderInfo(folder_id, target_index, source_index);
        ShowFolderForView(folder_view, /*new_folder=*/true);
      } else {
        folder_view->RequestFocus();
      }
    }

    // Layout the grid to ensure the created folder's bounds are correct.
    // Note that `open_folder_info_` affects ideal item bounds, so `Layout()`
    // needs to be callsed after `SetOpenFolderInfo()`.
    Layout();
  }
}

bool AppsGridView::CanMoveSelectedToTargetForKeyboardFoldering(
    const GridIndex& target_index) const {
  DCHECK(selected_view_);

  // To folder an item, the item must be moved into the folder, not the folder
  // moved over the item.
  const AppListItem* selected_item = selected_view_->item();
  if (selected_item->is_folder())
    return false;

  // Do not allow foldering across pages because the destination folder cannot
  // be seen.
  if (target_index.page != GetIndexOfView(selected_view_).page)
    return false;

  return true;
}

bool AppsGridView::HandleVerticalFocusMovement(bool arrow_up) {
  views::View* focused = GetFocusManager()->GetFocusedView();
  if (focused->GetClassName() != AppListItemView::kViewClassName)
    return false;

  const GridIndex source_index =
      GetIndexOfView(static_cast<const AppListItemView*>(focused));
  int target_page = source_index.page;
  int target_row = source_index.slot / cols_ + (arrow_up ? -1 : 1);
  int target_col = source_index.slot % cols_;

  if (target_row < 0) {
    // Move focus to the last row of previous page if target row is negative.
    --target_page;

    // |target_page| may be invalid which makes |target_row| invalid, but
    // |target_row| will not be used if |target_page| is invalid.
    target_row = (GetNumberOfItemsOnPage(target_page) - 1) / cols_;
  } else if (target_row > (GetNumberOfItemsOnPage(target_page) - 1) / cols_) {
    // Move focus to the first row of next page if target row is beyond range.
    ++target_page;
    target_row = 0;
  }

  if (target_page < 0) {
    // Move focus up outside the apps grid if target page is negative.
    if (focus_delegate_ &&
        focus_delegate_->MoveFocusUpFromAppsGrid(target_col)) {
      // The delegate handled the focus move.
      return true;
    }
    // Move focus backwards from the first item in the grid.
    views::View* v = GetFocusManager()->GetNextFocusableView(
        view_model_.view_at(0), /*starting_widget=*/nullptr, /*reverse=*/true,
        /*dont_loop=*/false);
    DCHECK(v);
    v->RequestFocus();
    return true;
  }

  if (target_page >= GetTotalPages()) {
    // Move focus down outside the apps grid if target page is beyond range.
    views::View* v = GetFocusManager()->GetNextFocusableView(
        view_model_.view_at(view_model_.view_size() - 1),
        /*starting_widget=*/nullptr, /*reverse=*/false,
        /*dont_loop=*/false);
    DCHECK(v);
    v->RequestFocus();
    return true;
  }

  GridIndex target_index(target_page, target_row * cols_ + target_col);

  // Ensure the focus is within the range of the target page.
  target_index.slot =
      std::min(GetNumberOfItemsOnPage(target_page) - 1, target_index.slot);
  if (IsValidIndex(target_index)) {
    GetViewAtIndex(target_index)->RequestFocus();
    return true;
  }
  return false;
}

void AppsGridView::UpdateColsAndRowsForFolder() {
  if (!folder_delegate_)
    return;

  const int item_count = item_list_ ? item_list_->item_count() : 0;

  // Ensure that there is always at least one column.
  if (item_count == 0) {
    cols_ = 1;
  } else {
    int preferred_cols = std::sqrt(item_list_->item_count() - 1) + 1;
    cols_ = base::clamp(preferred_cols, 1, max_cols_);
  }

  PreferredSizeChanged();
}

void AppsGridView::DispatchDragEventForReparent(Pointer pointer,
                                                const gfx::Point& drag_point) {
  folder_delegate_->DispatchDragEventForReparent(pointer, drag_point);
}

void AppsGridView::EndDragFromReparentItemInRootLevel(
    AppListItemView* original_parent_item_view,
    bool events_forwarded_to_drag_drop_host,
    bool cancel_drag,
    std::unique_ptr<AppDragIconProxy> drag_icon_proxy) {
  DCHECK(!IsInFolder());
  DCHECK_NE(-1, view_model_.GetIndexOfView(original_parent_item_view));

  // EndDrag was called before if |drag_view_| is nullptr.
  if (!drag_item_)
    return;

  drag_icon_proxy_ = std::move(drag_icon_proxy);

  AppListItem* drag_item = drag_item_;

  DCHECK(IsDraggingForReparentInRootLevelGridView());
  bool cancel_reparent = cancel_drag || drop_target_region_ == NO_TARGET;

  // The ID of the folder to which the item gets dropped. It will get set when
  // the item is moved to a folder. It will be set the to original folder ID if
  // reparent is canceled.
  std::string target_folder_id;

  // Cache the original item folder id, as model updates may destroy the
  // original folder item.
  const std::string original_folder_id =
      original_parent_item_view->item()->id();

  if (!events_forwarded_to_drag_drop_host && !cancel_reparent) {
    UpdateDropTargetRegion();
    if (drop_target_region_ == ON_ITEM && DropTargetIsValidFolder() &&
        DraggedItemCanEnterFolder()) {
      bool is_new_folder = false;
      if (MoveItemToFolder(drag_item, drop_target_, kMoveIntoAnotherFolder,
                           &target_folder_id, &is_new_folder)) {
        // Announce folder dropping event before end of drag of reparented item.
        MaybeCreateFolderDroppingAccessibilityEvent();
        // If move to folder created a folder, layout the grid to ensure the
        // created folder's bounds are correct.
        Layout();
        if (is_new_folder && features::IsProductivityLauncherEnabled()) {
          folder_to_open_after_drag_icon_animation_ = target_folder_id;
          SetOpenFolderInfo(target_folder_id, drop_target_,
                            reorder_placeholder_);
        }
      } else {
        cancel_reparent = true;
      }
    } else if (drop_target_region_ != NO_TARGET &&
               IsValidReorderTargetIndex(drop_target_)) {
      ReparentItemForReorder(drag_item, drop_target_);
      RecordAppMovingTypeMetrics(kMoveByDragOutOfFolder);
      // Announce accessibility event before the end of drag for reparented
      // item.
      MaybeCreateDragReorderAccessibilityEvent();
    } else {
      NOTREACHED();
    }
  }

  if (cancel_reparent)
    target_folder_id = original_folder_id;

  SetAsFolderDroppingTarget(drop_target_, false);

  UpdatePaging();
  ClearDragState();

  if (cardified_state_)
    MaybeEndCardifiedView();
  else
    AnimateToIdealBounds();

  if (!cancel_reparent)
    view_structure_.SaveToMetadata();

  // Hide the |current_ghost_view_| after completed drag from within
  // folder to |apps_grid_view_|.
  BeginHideCurrentGhostImageView();
  SetFocusAfterEndDrag();  // Maybe focus the search box.

  AnimateDragIconToTargetPosition(drag_item, target_folder_id);
}

void AppsGridView::EndDragForReparentInHiddenFolderGridView() {
  SetAsFolderDroppingTarget(drop_target_, false);
  ClearDragState();

  // Hide |current_ghost_view_| in the hidden folder grid view.
  BeginHideCurrentGhostImageView();
}

void AppsGridView::HandleKeyboardReparent(
    AppListItemView* reparented_view,
    AppListItemView* original_parent_item_view,
    ui::KeyboardCode key_code) {
  DCHECK(key_code == ui::VKEY_LEFT || key_code == ui::VKEY_RIGHT ||
         key_code == ui::VKEY_UP || key_code == ui::VKEY_DOWN);
  DCHECK(!folder_delegate_);
  DCHECK_NE(-1, view_model_.GetIndexOfView(original_parent_item_view));

  // Set |original_parent_item_view| selected so |target_index| will be
  // computed relative to the open folder.
  SetSelectedView(original_parent_item_view);
  const GridIndex target_index = GetTargetGridIndexForKeyboardReparent(
      GetIndexOfView(original_parent_item_view), key_code);
  ReparentItemForReorder(reparented_view->item(), target_index);

  view_structure_.SaveToMetadata();

  // Update paging because the move could have resulted in a
  // page getting created.
  UpdatePaging();

  Layout();
  EnsureViewVisible(target_index);
  GetViewAtIndex(target_index)->RequestFocus();
  AnnounceReorder(target_index);

  RecordAppMovingTypeMetrics(kMoveByKeyboardOutOfFolder);
}

void AppsGridView::UpdatePagedViewStructure() {
  view_structure_.SaveToMetadata();
}

bool AppsGridView::IsTabletMode() const {
  return app_list_view_delegate_->IsInTabletMode();
}

views::AnimationBuilder AppsGridView::FadeOutVisibleItemsForReorder(
    ReorderAnimationCallback done_callback) {
  // The caller of this function is responsible for aborting the old reorder
  // process before starting a new one.
  DCHECK(!IsUnderReorderAnimation());

  // Cancel the active bounds animations on item views if any.
  bounds_animator_->Cancel();

  reorder_animation_status_ = AppListReorderAnimationStatus::kFadeOutAnimation;
  reorder_animation_tracker_.emplace(
      layer()->GetCompositor()->RequestNewThroughputTracker());
  reorder_animation_tracker_->Start(metrics_util::ForSmoothness(
      base::BindRepeating(&ReportReorderAnimationSmoothness, IsTabletMode())));

  views::AnimationBuilder animation_builder;
  reorder_animation_abort_handle_ = animation_builder.GetAbortHandle();

  if (fade_out_start_closure_for_test_)
    animation_builder.OnStarted(std::move(fade_out_start_closure_for_test_));

  animation_builder
      .OnEnded(base::BindOnce(&AppsGridView::OnFadeOutAnimationEnded,
                              weak_factory_.GetWeakPtr(), done_callback,
                              /*abort=*/false))
      .OnAborted(base::BindOnce(&AppsGridView::OnFadeOutAnimationEnded,
                                weak_factory_.GetWeakPtr(), done_callback,
                                /*abort=*/true))
      .Once()
      .SetDuration(kFadeOutAnimationDuration)
      .SetOpacity(layer(), 0.f, gfx::Tween::LINEAR);
  return animation_builder;
}

views::AnimationBuilder AppsGridView::FadeInVisibleItemsForReorder(
    ReorderAnimationCallback done_callback) {
  DCHECK_EQ(AppListReorderAnimationStatus::kIntermediaryState,
            reorder_animation_status_);
  DCHECK(!bounds_animator_->IsAnimating());

  // When `AppsGridView::OnListItemMoved()` is called due to item reorder,
  // the layout updates asynchronously. Meanwhile, calculating the visible item
  // range needs the up-to-date layout. Therefore update the layout explicitly
  // before calculating `range`.
  if (needs_layout())
    Layout();

  reorder_animation_status_ = AppListReorderAnimationStatus::kFadeInAnimation;
  const absl::optional<VisibleItemIndexRange> range =
      GetVisibleItemIndexRange();

  // TODO(https://crbug.com/1289411): handle the case that `range` is null.
  DCHECK(range);

  // Only show the visible items during animation to reduce the cost of painting
  // that is triggered by view bounds changes due to reorder.
  for (int visible_view_index = range->first_index;
       visible_view_index <= range->last_index; ++visible_view_index) {
    view_model_.view_at(visible_view_index)->SetVisible(true);
  }

  views::AnimationBuilder animation_builder;
  reorder_animation_abort_handle_ = animation_builder.GetAbortHandle();
  animation_builder
      .OnEnded(base::BindOnce(&AppsGridView::OnFadeInAnimationEnded,
                              weak_factory_.GetWeakPtr(), done_callback,
                              /*abort=*/false))
      .OnAborted(base::BindOnce(&AppsGridView::OnFadeInAnimationEnded,
                                weak_factory_.GetWeakPtr(), done_callback,
                                /*abort=*/true))
      .Once()
      .SetDuration(kFadeInAnimationDuration)
      .SetOpacity(layer(), 1.f, gfx::Tween::ACCEL_5_70_DECEL_90);

  // Assume all the items matched by the indices in `range` are
  // placed on the same page.
  const int page_index =
      view_structure_.GetIndexFromModelIndex(range->first_index).page;
  const int base_offset =
      kFadeAnimationOffsetRatio * GetTotalTileSize(page_index).height();

  // The row of the first visible item.
  const int base_row = range->first_index / cols_;

  for (int visible_view_index = range->first_index;
       visible_view_index <= range->last_index; ++visible_view_index) {
    // Calculate translate offset for each view. NOTE: The items on the
    // different rows have different fade in offsets. The ratio between the
    // offset and `base_offset` is (relative_row_index + 2).
    const int relative_row_index = visible_view_index / cols_ - base_row;
    const int offset = (relative_row_index + 2) * base_offset;

    views::View* animated_view = GetItemViewAt(visible_view_index);
    PrepareForLayerAnimation(animated_view);

    // Create a slide animation on `animted_view` using `sequence_block`'s
    // existing time duration.
    SlideViewIntoPositionWithSequenceBlock(
        animated_view, offset,
        /*time_delta=*/absl::nullopt, gfx::Tween::ACCEL_5_70_DECEL_90,
        &animation_builder.GetCurrentSequence());
  }

  return animation_builder;
}

bool AppsGridView::IsAnimationRunningForTest() {
  return bounds_animator_->IsAnimating() ||
         bounds_animation_for_cardified_state_in_progress_ > 0;
}

void AppsGridView::CancelAnimationsForTest() {
  bounds_animator_->Cancel();
  drag_icon_proxy_.reset();

  const int total_views = view_model_.view_size();
  for (int i = 0; i < total_views; ++i) {
    if (view_model_.view_at(i)->layer())
      view_model_.view_at(i)->layer()->CompleteAllAnimations();
  }
}

bool AppsGridView::FireFolderItemReparentTimerForTest() {
  if (!folder_item_reparent_timer_.IsRunning())
    return false;
  folder_item_reparent_timer_.FireNow();
  return true;
}

bool AppsGridView::FireDragToShelfTimerForTest() {
  if (!host_drag_start_timer_.IsRunning())
    return false;
  host_drag_start_timer_.FireNow();
  return true;
}

void AppsGridView::AddReorderCallbackForTest(
    TestReorderDoneCallbackType done_callback) {
  DCHECK(done_callback);

  reorder_animation_callback_queue_for_test_.push(std::move(done_callback));
}

void AppsGridView::AddFadeOutAnimationStartClosureForTest(
    base::OnceClosure start_closure) {
  DCHECK(start_closure);
  DCHECK(!fade_out_done_closure_for_test_);

  fade_out_start_closure_for_test_ = std::move(start_closure);
}

void AppsGridView::AddFadeOutAnimationDoneClosureForTest(
    base::OnceClosure done_closure) {
  DCHECK(done_closure);
  DCHECK(!fade_out_done_closure_for_test_);

  fade_out_done_closure_for_test_ = std::move(done_closure);
}

bool AppsGridView::HasAnyWaitingReorderDoneCallbackForTest() const {
  return !reorder_animation_callback_queue_for_test_.empty();
}

void AppsGridView::StartDragAndDropHostDrag() {
  // When a drag and drop host is given, the item can be dragged out of the app
  // list window. In that case a proxy widget needs to be used.
  if (!drag_view_)
    return;

  // We have to hide the original item since the drag and drop host will do
  // the OS dependent code to "lift off the dragged item". Apply the scale
  // factor of this view's transform to the dragged view as well.
  DCHECK(!IsDraggingForReparentInRootLevelGridView());

  gfx::Point location_in_screen = drag_start_grid_view_;
  views::View::ConvertPointToScreen(this, &location_in_screen);

  const gfx::Point icon_location_in_screen =
      drag_view_->GetIconBoundsInScreen().CenterPoint();

  const bool use_blurred_background =
      drag_view_->item()->is_folder() && IsTabletMode();
  drag_icon_proxy_ = std::make_unique<AppDragIconProxy>(
      GetWidget()->GetNativeWindow()->GetRootWindow(),
      drag_view_->GetIconImage(), location_in_screen,
      location_in_screen - icon_location_in_screen,
      drag_view_->item()->is_folder() ? kDragAndDropProxyScale : 1.0f,
      use_blurred_background);
  drag_view_hider_ = std::make_unique<DragViewHider>(drag_view_);
}

void AppsGridView::DispatchDragEventToDragAndDropHost(
    const gfx::Point& location_in_screen_coordinates) {
  if (!drag_view_ || !drag_and_drop_host_)
    return;

  const bool should_host_handle_drag = drag_and_drop_host_->ShouldHandleDrag(
      drag_view_->item()->id(), location_in_screen_coordinates);

  if (!should_host_handle_drag) {
    if (host_drag_start_timer_.IsRunning())
      host_drag_start_timer_.AbandonAndStop();

    // The event was issued inside the app menu and we should get all events.
    if (forward_events_to_drag_and_drop_host_) {
      // The DnD host was previously called and needs to be informed that the
      // session returns to the owner.
      forward_events_to_drag_and_drop_host_ = false;
      // NOTE: Not passing the drag icon proxy to the drag and drop host because
      // the drag operation is still in progress, and remains being handled by
      // the apps grid view.
      drag_and_drop_host_->EndDrag(true, /*drag_icon_proxy=*/nullptr);
    }
    return;
  }

  if (IsFolderItem(drag_view_->item()))
    return;

  // NOTE: Drag events are forwarded to drag and drop host whenever drag and
  // drop host can handle them. At the time of writing, drag and drop host
  // bounds and apps grid view bounds are not expected to overlap - if that
  // changes, the logic for determining when to forward events to the host
  // should be re-evaluated.

  DCHECK(should_host_handle_drag);
  // If the drag and drop host is not already handling drag events, make sure a
  // drag and drop host start timer gets scheduled.
  if (!forward_events_to_drag_and_drop_host_) {
    if (!host_drag_start_timer_.IsRunning()) {
      host_drag_start_timer_.Start(FROM_HERE, kShelfHandleIconDragDelay, this,
                                   &AppsGridView::OnHostDragStartTimerFired);
      MaybeStopPageFlip();
      StopAutoScroll();
    }
    return;
  }

  DCHECK(forward_events_to_drag_and_drop_host_);
  if (!drag_and_drop_host_->Drag(location_in_screen_coordinates,
                                 drag_icon_proxy_->GetBoundsInScreen())) {
    // The host is not active any longer and we cancel the operation.
    forward_events_to_drag_and_drop_host_ = false;
    // NOTE: Not passing the drag icon proxy to the drag and drop host because
    // the drag operation is still in progress, and remains being handled by
    // the apps grid view.
    drag_and_drop_host_->EndDrag(true, /*drag_icon_proxy=*/nullptr);
  }
}

bool AppsGridView::IsMoveTargetOnNewPage(const GridIndex& target) const {
  // This is used to determine whether move should create a page break item,
  // which is only relevant for page structure with partial pages.
  DCHECK_EQ(view_structure_.mode(), PagedViewStructure::Mode::kPartialPages);

  return target.page == GetTotalPages() ||
         (target.page == GetTotalPages() - 1 &&
          view_structure_.GetLastTargetIndexOfPage(target.page).slot == 0);
}

void AppsGridView::EnsurePageBreakBeforeItem(const std::string& item_id) {
  DCHECK_EQ(view_structure_.mode(), PagedViewStructure::Mode::kPartialPages);

  size_t item_list_index = 0;
  if (item_list_->FindItemIndex(item_id, &item_list_index) &&
      item_list_index > 0 &&
      !item_list_->item_at(item_list_index - 1)->is_page_break()) {
    model_->AddPageBreakItemAfter(item_list_->item_at(item_list_index - 1));
  }
}

void AppsGridView::MoveItemInModel(AppListItem* item, const GridIndex& target) {
  const std::string item_id = item->id();

  size_t current_item_list_index = 0;
  bool found = item_list_->FindItemIndex(item_id, &current_item_list_index);
  CHECK(found);

  size_t target_item_list_index =
      view_structure_.GetTargetItemListIndexForMove(item, target);

  const bool moving_to_new_page =
      view_structure_.mode() == PagedViewStructure::Mode::kPartialPages &&
      IsMoveTargetOnNewPage(target);

  {
    ScopedModelUpdate update(this);
    item_list_->MoveItem(current_item_list_index, target_item_list_index);

    // If the item is being moved to a new page, ensure that it's preceded by a
    // page break.
    if (moving_to_new_page)
      EnsurePageBreakBeforeItem(item_id);
  }
}

bool AppsGridView::MoveItemToFolder(AppListItem* item,
                                    const GridIndex& target,
                                    AppListAppMovingType move_type,
                                    std::string* folder_id,
                                    bool* is_new_folder) {
  const std::string source_item_id = item->id();
  const std::string source_folder_id = item->folder_id();

  AppListItemView* target_view =
      GetViewDisplayedAtSlotOnCurrentPage(target.slot);
  DCHECK(target_view);
  const std::string target_view_item_id = target_view->item()->id();

  // An app is being reparented to its original folder. Just cancel the
  // reparent.
  if (target_view_item_id == source_folder_id)
    return false;

  *is_new_folder = !target_view->is_folder();

  {
    ScopedModelUpdate update(this);
    *folder_id = model_->MergeItems(target_view_item_id, source_item_id);
  }

  if (folder_id->empty()) {
    LOG(ERROR) << "Unable to merge into item id: " << target_view_item_id;
    return false;
  }

  RecordAppMovingTypeMetrics(move_type);
  return true;
}

void AppsGridView::ReparentItemForReorder(AppListItem* item,
                                          const GridIndex& target) {
  DCHECK(item->IsInFolder());

  const std::string item_id = item->id();
  const std::string source_folder_id = item->folder_id();
  int target_item_index =
      view_structure_.GetTargetItemListIndexForMove(item, target);

  // Move the item from its parent folder to top level item list. Calculate the
  // target position in the top level list.
  syncer::StringOrdinal target_position;
  if (target_item_index < static_cast<int>(item_list_->item_count()))
    target_position = item_list_->item_at(target_item_index)->position();

  const bool moving_to_new_page =
      view_structure_.mode() == PagedViewStructure::Mode::kPartialPages &&
      IsMoveTargetOnNewPage(target);

  {
    ScopedModelUpdate update(this);
    model_->MoveItemToRootAt(item, target_position);

    // If the item is being moved to a new page, ensure that it's preceded by a
    // page break.
    if (moving_to_new_page)
      EnsurePageBreakBeforeItem(item_id);
  }
}

void AppsGridView::CancelContextMenusOnCurrentPage() {
  GridIndex start_index(GetSelectedPage(), 0);
  if (!IsValidIndex(start_index))
    return;
  int start = view_structure_.GetModelIndexFromIndex(start_index);
  int end =
      std::min(view_model_.view_size(), start + TilesPerPage(start_index.page));
  for (int i = start; i < end; ++i)
    GetItemViewAt(i)->CancelContextMenu();
}

void AppsGridView::DeleteItemViewAtIndex(int index) {
  AppListItemView* item_view = GetItemViewAt(index);
  view_model_.Remove(index);
  view_structure_.Remove(item_view);
  if (item_view == drag_view_)
    drag_view_ = nullptr;
  if (open_folder_info_ &&
      open_folder_info_->item_id == item_view->item()->id()) {
    open_folder_info_.reset();
  }
  delete item_view;
}

bool AppsGridView::IsPointWithinDragBuffer(const gfx::Point& point) const {
  gfx::Rect rect(GetLocalBounds());
  rect.Inset(-kDragBufferPx, -kDragBufferPx, -kDragBufferPx, -kDragBufferPx);
  return rect.Contains(point);
}

void AppsGridView::ScheduleLayout(const gfx::Size& previous_grid_size) {
  if (GetTileGridSize() != previous_grid_size) {
    PreferredSizeChanged();  // Calls InvalidateLayout() internally.
  } else {
    InvalidateLayout();
  }
  DCHECK(needs_layout());
}

void AppsGridView::OnListItemAdded(size_t index, AppListItem* item) {
  const gfx::Size initial_grid_size = GetTileGridSize();

  if (!updating_model_)
    EndDrag(true);

  // Abort reorder animation before a view is added to `view_model_`.
  MaybeAbortReorderAnimation();

  if (!item->is_page_break()) {
    int model_index = GetTargetModelIndexFromItemIndex(index);
    AppListItemView* view = items_container_->AddChildViewAt(
        CreateViewForItemAtIndex(index), model_index);
    view_model_.Add(view, model_index);

    if (item == drag_item_) {
      drag_view_ = view;
      drag_view_hider_ = std::make_unique<DragViewHider>(drag_view_);
      drag_view_->RequestFocus();
    }
    view->InitializeIconLoader();
  }

  view_structure_.LoadFromMetadata();

  // If model update is in progress, paging should be updated when the operation
  // that caused the model update completes.
  if (!updating_model_) {
    UpdatePaging();
    UpdateColsAndRowsForFolder();
    UpdatePulsingBlockViews();
  }

  // Schedule a layout, since the grid items may need their bounds updated.
  ScheduleLayout(initial_grid_size);
}

void AppsGridView::OnListItemRemoved(size_t index, AppListItem* item) {
  const gfx::Size initial_grid_size = GetTileGridSize();

  if (!updating_model_)
    EndDrag(true);

  // Abort reorder animation before a view is deleted from `view_model_`.
  MaybeAbortReorderAnimation();

  if (!item->is_page_break())
    DeleteItemViewAtIndex(GetModelIndexOfItem(item));

  view_structure_.LoadFromMetadata();

  // If model update is in progress, paging should be updated when the operation
  // that caused the model update completes.
  if (!updating_model_) {
    UpdatePaging();
    UpdateColsAndRowsForFolder();
    UpdatePulsingBlockViews();
  }

  // Schedule a layout, since the grid items may need their bounds updated.
  ScheduleLayout(initial_grid_size);
}

void AppsGridView::OnListItemMoved(size_t from_index,
                                   size_t to_index,
                                   AppListItem* item) {
  // Abort reorder animation if the apps grid is updated by the user.
  if (!updating_model_) {
    MaybeAbortReorderAnimation();

    EndDrag(true);
  }

  if (item->is_page_break()) {
    LOG(ERROR) << "Page break item is moved: " << item->id();
  } else {
    // The item is updated in the item list but the view_model is not updated,
    // so get current model index by looking up view_model and predict the
    // target model index based on its current item index.
    int from_model_index = GetModelIndexOfItem(item);
    int to_model_index = GetTargetModelIndexFromItemIndex(to_index);
    view_model_.Move(from_model_index, to_model_index);
    items_container_->ReorderChildView(view_model_.view_at(to_model_index),
                                       to_model_index);
    items_container_->NotifyAccessibilityEvent(
        ax::mojom::Event::kChildrenChanged, true /* send_native_event */);
  }

  view_structure_.LoadFromMetadata();

  // If model update is in progress, paging should be updated when the operation
  // that caused the model update completes.
  if (!updating_model_) {
    UpdatePaging();
    UpdateColsAndRowsForFolder();
    UpdatePulsingBlockViews();
  }

  if (!updating_model_ && GetWidget() && GetWidget()->IsVisible() &&
      enable_item_move_animation_) {
    AnimateToIdealBounds();
  } else if (IsUnderReorderAnimation()) {
    // During reorder animation, multiple items could be moved subsequently so
    // use the asynchronous layout to reduce painting cost.
    InvalidateLayout();
  } else {
    Layout();
  }
}

void AppsGridView::OnAppListModelStatusChanged() {
  UpdatePulsingBlockViews();
  Layout();
  SchedulePaint();
}

void AppsGridView::OnBoundsAnimatorProgressed(views::BoundsAnimator* animator) {
}

void AppsGridView::OnBoundsAnimatorDone(views::BoundsAnimator* animator) {
  if (ItemViewsRequireLayers())
    return;

  for (const auto& entry : view_model_.entries())
    entry.view->DestroyLayer();
}

bool AppsGridView::ItemViewsRequireLayers() const {
  // Layers required for app list item move animations during drag (to make room
  // for the current placeholder).
  if (drag_item_ || drag_icon_proxy_)
    return true;

  // Bounds animations are in progress, which use layers to animate transforms.
  if (bounds_animation_for_cardified_state_in_progress_ ||
      (bounds_animator_ && bounds_animator_->IsAnimating())) {
    return true;
  }

  // Reorder animation animate app list item layers.
  if (IsUnderReorderAnimation())
    return true;

  // Folder position is changing after folder closure - this involves animating
  // folder item view layer out and in, and changing other view's bounds.
  if (reordering_folder_view_)
    return true;

  return false;
}

GridIndex AppsGridView::GetNearestTileIndexForPoint(
    const gfx::Point& point) const {
  gfx::Rect bounds = GetContentsBounds();
  const int current_page = GetSelectedPage();
  bounds.Inset(GetTilePadding(current_page));
  const gfx::Size total_tile_size = GetTotalTileSize(current_page);
  const gfx::Vector2d grid_offset = GetGridCenteringOffset(current_page);

  DCHECK_GT(total_tile_size.width(), 0);
  int col = base::clamp(
      (point.x() - bounds.x() - grid_offset.x()) / total_tile_size.width(), 0,
      cols_ - 1);

  DCHECK_GT(total_tile_size.height(), 0);
  int max_row = TilesPerPage(current_page) / cols_ - 1;
  int row = base::clamp(
      (point.y() - bounds.y() - grid_offset.y()) / total_tile_size.height(), 0,
      max_row);

  return GridIndex(current_page, row * cols_ + col);
}

gfx::Rect AppsGridView::GetExpectedTileBounds(const GridIndex& index) const {
  if (!cols_)
    return gfx::Rect();

  gfx::Rect bounds(GetContentsBounds());
  bounds.Inset(GetTilePadding(index.page));
  int row = index.slot / cols_;
  int col = index.slot % cols_;
  const gfx::Size total_tile_size = GetTotalTileSize(index.page);
  gfx::Rect tile_bounds(gfx::Point(bounds.x() + col * total_tile_size.width(),
                                   bounds.y() + row * total_tile_size.height()),
                        total_tile_size);

  tile_bounds.Offset(GetGridCenteringOffset(index.page));
  tile_bounds.Inset(-GetTilePadding(index.page));
  return tile_bounds;
}

bool AppsGridView::IsViewHiddenForDrag(const views::View* view) const {
  return drag_view_hider_ && drag_view_hider_->drag_view() == view;
}

bool AppsGridView::IsViewHiddenForFolderReorder(const views::View* view) const {
  return reordering_folder_view_ && *reordering_folder_view_ == view;
}

bool AppsGridView::IsUnderReorderAnimation() const {
  return reorder_animation_status_ != AppListReorderAnimationStatus::kEmpty;
}

void AppsGridView::MaybeAbortReorderAnimation() {
  switch (reorder_animation_status_) {
    case AppListReorderAnimationStatus::kEmpty:
    case AppListReorderAnimationStatus::kIntermediaryState:
      // No active reorder animation so nothing to do.
      break;
    case AppListReorderAnimationStatus::kFadeOutAnimation:
    case AppListReorderAnimationStatus::kFadeInAnimation:
      DCHECK(reorder_animation_abort_handle_);
      reorder_animation_abort_handle_.reset();
      break;
  }
}

AppListItemView* AppsGridView::GetViewDisplayedAtSlotOnCurrentPage(
    int slot) const {
  if (slot < 0)
    return nullptr;

  // Calculate the original bound of the tile at |index|.
  gfx::Rect tile_rect =
      GetExpectedTileBounds(GridIndex(GetSelectedPage(), slot));
  tile_rect.Offset(CalculateTransitionOffset(GetSelectedPage()));

  const auto& entries = view_model_.entries();
  const auto iter =
      std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.view->bounds() == tile_rect && entry.view != drag_view_;
      });
  return iter == entries.end() ? nullptr
                               : static_cast<AppListItemView*>(iter->view);
}

void AppsGridView::SetAsFolderDroppingTarget(const GridIndex& target_index,
                                             bool is_target_folder) {
  AppListItemView* target_view =
      GetViewDisplayedAtSlotOnCurrentPage(target_index.slot);
  if (target_view) {
    target_view->SetAsAttemptedFolderTarget(is_target_folder);
    if (is_target_folder)
      target_view->OnDraggedViewEnter();
    else
      target_view->OnDraggedViewExit();
  }
}

GridIndex AppsGridView::GetTargetGridIndexForKeyboardMove(
    ui::KeyboardCode key_code) const {
  DCHECK(key_code == ui::VKEY_LEFT || key_code == ui::VKEY_RIGHT ||
         key_code == ui::VKEY_UP || key_code == ui::VKEY_DOWN);
  DCHECK(selected_view_);

  const GridIndex source_index = GetIndexOfView(selected_view_);
  GridIndex target_index;
  if (key_code == ui::VKEY_LEFT || key_code == ui::VKEY_RIGHT) {
    // Define backward key for traversal based on RTL.
    const ui::KeyboardCode backward =
        base::i18n::IsRTL() ? ui::VKEY_RIGHT : ui::VKEY_LEFT;

    const int target_model_index = view_model_.GetIndexOfView(selected_view_) +
                                   ((key_code == backward) ? -1 : 1);

    // A forward move on the last item in |view_model_| should result in page
    // creation.
    if (target_model_index == view_model_.view_size()) {
      // Only grid structure with partial pages supports page creation by
      // keyboard move.
      if (view_structure_.mode() != PagedViewStructure::Mode::kPartialPages)
        return source_index;
      // If |source_index| is the last item in the grid on a page by itself,
      // moving right to a new page should be a no-op.
      if (view_structure_.items_on_page(source_index.page) == 1)
        return source_index;
      return GridIndex(GetTotalPages(), 0);
    }

    target_index = GetIndexOfView(
        static_cast<const AppListItemView*>(GetItemViewAt(std::min(
            std::max(0, target_model_index), view_model_.view_size() - 1))));
    if (view_structure_.mode() == PagedViewStructure::Mode::kPartialPages &&
        key_code == backward && target_index.page < source_index.page &&
        !view_structure_.IsFullPage(target_index.page)) {
      // Apps swap positions if the target page is the same as the
      // destination page, or the target page is full. If the page is not
      // full the app is dumped on the page. Increase the slot in this case
      // to account for the new available spot.
      ++target_index.slot;
    }
    return target_index;
  }

  // Handle the vertical move. Attempt to place the app in the same column.
  int target_page = source_index.page;
  int target_row =
      source_index.slot / cols_ + (key_code == ui::VKEY_UP ? -1 : 1);

  if (target_row < 0) {
    // The app will move to the last row of the previous page.
    --target_page;
    if (target_page < 0)
      return source_index;

    // When moving up, place the app in the last row.
    target_row = (GetNumberOfItemsOnPage(target_page) - 1) / cols_;
  } else if (target_row > (GetNumberOfItemsOnPage(target_page) - 1) / cols_) {
    // The app will move to the first row of the next page.
    ++target_page;
    if (folder_delegate_) {
      if (target_page >= GetTotalPages())
        return source_index;
    } else {
      if (target_page >= view_structure_.total_pages()) {
        // If |source_index| page only has one item, moving down to a new page
        // should be a no-op.
        if (view_structure_.items_on_page(source_index.page) == 1)
          return source_index;
        return GridIndex(target_page, 0);
      }
    }
    target_row = 0;
  }

  // The ideal slot shares a column with |source_index|.
  const int ideal_slot = target_row * cols_ + source_index.slot % cols_;
  if (folder_delegate_) {
    return GridIndex(
        target_page,
        std::min(GetNumberOfItemsOnPage(target_page) - 1, ideal_slot));
  }

  // If the app is being moved to a new page there is 1 extra slot available.
  const int last_slot_in_target_page =
      view_structure_.items_on_page(target_page) -
      (source_index.page != target_page ? 0 : 1);
  return GridIndex(target_page, std::min(last_slot_in_target_page, ideal_slot));
}

GridIndex AppsGridView::GetTargetGridIndexForKeyboardReparent(
    const GridIndex& folder_index,
    ui::KeyboardCode key_code) const {
  DCHECK(!folder_delegate_) << "Reparenting target calculations occur from the "
                               "root AppsGridView, not the folder AppsGridView";

  // A backward move means the item will be placed previous to the folder. To do
  // this without displacing other items, place the item in the folders slot.
  // The folder will then shift forward.
  const ui::KeyboardCode backward =
      base::i18n::IsRTL() ? ui::VKEY_RIGHT : ui::VKEY_LEFT;
  if (key_code == backward)
    return folder_index;

  GridIndex target_index = GetTargetGridIndexForKeyboardMove(key_code);

  // If the item is expected to be positioned after the parent view,
  // `GetTargetGridIndexForKeyboardMove()` may return folder index to indicate
  // no-op operation for move (e.g. if the folder is the last item), assuming
  // that there are no slots available. Reparent is an insertion operation, so
  // creating an extra trailing slot is allowed.
  if (target_index == folder_index &&
      (key_code != ui::VKEY_UP && key_code != backward)) {
    if (view_structure_.IsFullPage(target_index.page)) {
      return GridIndex(target_index.page + 1, 0);
    }
    return GridIndex(target_index.page, target_index.slot + 1);
  }

  // Ensure the item is placed on the same page as the folder when possible.
  if (target_index.page < folder_index.page)
    return folder_index;
  const int folder_page_size = TilesPerPage(folder_index.page);
  if (target_index.page > folder_index.page &&
      folder_index.slot + 1 < folder_page_size) {
    return GridIndex(folder_index.page, folder_index.slot + 1);
  }

  return target_index;
}

void AppsGridView::HandleKeyboardMove(ui::KeyboardCode key_code) {
  DCHECK(selected_view_);
  const GridIndex target_index = GetTargetGridIndexForKeyboardMove(key_code);
  const GridIndex starting_index = GetIndexOfView(selected_view_);
  if (target_index == starting_index ||
      !IsValidReorderTargetIndex(target_index)) {
    return;
  }

  handling_keyboard_move_ = true;

  AppListItemView* original_selected_view = selected_view_;
  const GridIndex original_selected_view_index =
      GetIndexOfView(original_selected_view);
  // Moving an AppListItemView is either a swap within the origin page, a swap
  // to a full page, or a dump to a page with room. A move within a folder is
  // always a swap because there are no gaps.
  const bool swap_items =
      folder_delegate_ || view_structure_.IsFullPage(target_index.page) ||
      target_index.page == original_selected_view_index.page;

  AppListItemView* target_view = GetViewAtIndex(target_index);
  {
    // If the move is a two part operation (swap) do not clear the overflow
    // during the initial move. Clearing the overflow when |target_index| is on
    // a full page results in the last item being pushed to the next page.
    std::unique_ptr<PagedViewStructure::ScopedSanitizeLock> sanitize_lock =
        view_structure_.GetSanitizeLock();
    MoveItemInModel(selected_view_->item(), target_index);
    if (swap_items) {
      DCHECK(target_view);
      MoveItemInModel(target_view->item(), original_selected_view_index);
    }
  }

  view_structure_.SaveToMetadata();

  int target_page = target_index.page;
  if (!folder_delegate_) {
    // Update paging because the move could have resulted in a
    // page getting collapsed or created.
    UpdatePaging();

    // |target_page| may change due to a page collapsing.
    target_page = std::min(GetTotalPages() - 1, target_index.page);
  }
  Layout();
  EnsureViewVisible(GridIndex(target_page, target_index.slot));
  SetSelectedView(original_selected_view);
  AnnounceReorder(target_index);

  if (target_index.page != original_selected_view_index.page &&
      !folder_delegate_) {
    RecordPageSwitcherSource(kMoveAppWithKeyboard, IsTabletMode());
  }
}

bool AppsGridView::IsValidIndex(const GridIndex& index) const {
  return index.page >= 0 && index.page < GetTotalPages() && index.slot >= 0 &&
         index.slot < TilesPerPage(index.page) &&
         view_structure_.GetModelIndexFromIndex(index) <
             view_model_.view_size();
}

bool AppsGridView::IsValidReorderTargetIndex(const GridIndex& index) const {
  return view_structure_.IsValidReorderTargetIndex(index);
}

int AppsGridView::GetModelIndexOfItem(const AppListItem* item) const {
  const auto& entries = view_model_.entries();
  const auto iter =
      std::find_if(entries.begin(), entries.end(), [item](const auto& entry) {
        return static_cast<AppListItemView*>(entry.view)->item() == item;
      });
  return std::distance(entries.begin(), iter);
}

int AppsGridView::GetTargetModelIndexFromItemIndex(size_t item_index) {
  if (folder_delegate_)
    return item_index;

  CHECK(item_index <= item_list_->item_count());
  int target_model_index = 0;
  for (size_t i = 0; i < item_index; ++i) {
    if (!item_list_->item_at(i)->is_page_break())
      ++target_model_index;
  }
  return target_model_index;
}

int AppsGridView::GetNumberOfItemsOnPage(int page) const {
  if (page < 0 || page >= GetTotalPages())
    return 0;

  if (!folder_delegate_ && !features::IsProductivityLauncherEnabled())
    return view_structure_.items_on_page(page);

  // We are guaranteed not on the last page, so the page must be full.
  if (page < GetTotalPages() - 1)
    return TilesPerPage(page);

  // We are on the last page, so calculate the number of items on the page.
  int item_count = view_model_.view_size();
  int current_page = 0;
  while (current_page < GetTotalPages() - 1) {
    item_count -= TilesPerPage(current_page);
    ++current_page;
  }
  return item_count;
}

void AppsGridView::MaybeCreateFolderDroppingAccessibilityEvent() {
  if (!drag_item_ || !drag_view_)
    return;
  if (drop_target_region_ != ON_ITEM || !DropTargetIsValidFolder() ||
      IsFolderItem(drag_item_) || folder_delegate_ ||
      drop_target_ == last_folder_dropping_a11y_event_location_) {
    return;
  }

  last_folder_dropping_a11y_event_location_ = drop_target_;
  last_reorder_a11y_event_location_ = GridIndex();

  AppListItemView* drop_view =
      GetViewDisplayedAtSlotOnCurrentPage(drop_target_.slot);
  DCHECK(drop_view);

  a11y_announcer_->AnnounceFolderDrop(drag_view_->title()->GetText(),
                                      drop_view->title()->GetText(),
                                      drop_view->is_folder());
}

void AppsGridView::MaybeCreateDragReorderAccessibilityEvent() {
  if (drop_target_region_ == ON_ITEM && !IsFolderItem(drag_item_))
    return;

  // If app was dragged out of folder, no need to announce location for the
  // now closed folder.
  if (drag_out_of_folder_container_)
    return;

  // If drop_target is not set or was already reset, then return.
  if (drop_target_ == GridIndex())
    return;

  // Don't create a11y event if |drop_target| has not changed.
  if (last_reorder_a11y_event_location_ == drop_target_)
    return;

  last_folder_dropping_a11y_event_location_ = GridIndex();
  last_reorder_a11y_event_location_ = drop_target_;

  AnnounceReorder(last_reorder_a11y_event_location_);
}

void AppsGridView::AnnounceReorder(const GridIndex& target_index) {
  const int page = target_index.page + 1;
  const int row =
      ((target_index.slot - (target_index.slot % cols_)) / cols_) + 1;
  const int col = (target_index.slot % cols_) + 1;
  if (view_structure_.mode() == PagedViewStructure::Mode::kSinglePage) {
    // Don't announce the page for single-page grids (e.g. scrollable grids).
    a11y_announcer_->AnnounceAppsGridReorder(row, col);
  } else {
    // Announce the page for paged grids.
    a11y_announcer_->AnnounceAppsGridReorder(page, row, col);
  }
}

void AppsGridView::CreateGhostImageView() {
  if (!features::IsProductivityLauncherEnabled())
    return;
  if (!drag_item_)
    return;

  // OnReorderTimer() can trigger this function even when the
  // |reorder_placeholder_| does not change, no need to set a new GhostImageView
  // in this case.
  if (reorder_placeholder_ == current_ghost_location_)
    return;

  // When the item is dragged outside the boundaries of the app grid, if the
  // |reorder_placeholder_| moves to another page, then do not show a ghost.
  if (GetSelectedPage() != reorder_placeholder_.page) {
    BeginHideCurrentGhostImageView();
    return;
  }

  BeginHideCurrentGhostImageView();
  current_ghost_location_ = reorder_placeholder_;

  if (last_ghost_view_)
    delete last_ghost_view_;

  // Preserve |current_ghost_view_| while it fades out and instantiate a new
  // GhostImageView that will fade in.
  last_ghost_view_ = current_ghost_view_;

  auto current_ghost_view =
      std::make_unique<GhostImageView>(reorder_placeholder_);
  gfx::Rect ghost_view_bounds = GetExpectedTileBounds(reorder_placeholder_);
  ghost_view_bounds.Offset(
      CalculateTransitionOffset(reorder_placeholder_.page));
  current_ghost_view->Init(ghost_view_bounds,
                           app_list_config_->grid_focus_corner_radius());
  current_ghost_view_ =
      items_container_->AddChildView(std::move(current_ghost_view));
  current_ghost_view_->FadeIn();

  // Adding the ghost view can reorder the child layers of the
  // |items_container_| so make sure the background cards remain at the bottom.
  StackCardsAtBottom();
}

void AppsGridView::BeginHideCurrentGhostImageView() {
  if (!features::IsProductivityLauncherEnabled())
    return;

  current_ghost_location_ = GridIndex();

  if (current_ghost_view_)
    current_ghost_view_->FadeOut();
}

void AppsGridView::OnAppListItemViewActivated(
    AppListItemView* pressed_item_view,
    const ui::Event& event) {
  if (IsDragging())
    return;

  if (IsFolderItem(pressed_item_view->item())) {
    // Note that `folder_controller_` will be null inside a folder apps grid,
    // but those grid are not expected to contain folder items.
    DCHECK(folder_controller_);
    SetOpenFolderInfo(pressed_item_view->item()->id(),
                      GetIndexOfView(pressed_item_view), GridIndex());
    ShowFolderForView(pressed_item_view, /*new_folder=*/false);
    return;
  }

  base::RecordAction(base::UserMetricsAction("AppList_ClickOnApp"));

  // Avoid using |item->id()| as the parameter. In some rare situations,
  // activating the item may destruct it. Using the reference to an object
  // which may be destroyed during the procedure as the function parameter
  // may bring the crash like https://crbug.com/990282.
  const std::string id = pressed_item_view->item()->id();
  app_list_view_delegate()->ActivateItem(
      id, event.flags(), AppListLaunchedFrom::kLaunchedFromGrid);
}

void AppsGridView::OnHostDragStartTimerFired() {
  gfx::Point last_drag_point_in_screen = last_drag_point_;
  views::View::ConvertPointToScreen(this, &last_drag_point_in_screen);
  if (drag_and_drop_host_->StartDrag(drag_view_->item()->id(),
                                     last_drag_point_in_screen,
                                     drag_icon_proxy_->GetBoundsInScreen())) {
    // From now on we forward the drag events.
    forward_events_to_drag_and_drop_host_ = true;
  }
}

void AppsGridView::OnFadeOutAnimationEnded(ReorderAnimationCallback callback,
                                           bool aborted) {
  reorder_animation_status_ = AppListReorderAnimationStatus::kIntermediaryState;

  // Reset with the identical transformation. Because the apps grid view is
  // translucent now, setting the layer transform does not bring noticeable
  // differences.
  layer()->SetTransform(gfx::Transform());

  if (aborted) {
    // If the fade out animation is aborted, show the apps grid because the fade
    // in animation should not be called when the fade out animation is aborted.
    layer()->SetOpacity(1.f);
  } else {
    // Hide all item views before the fade in animation in order to reduce the
    // painting cost incurred by the bounds changes because of reorder. The
    // fade in animation should be responsible for reshowing the item views that
    // are within the visible view port after reorder.
    for (int view_index = 0; view_index < view_model_.view_size();
         ++view_index) {
      view_model_.view_at(view_index)->SetVisible(false);
    }
  }

  // Before starting the fade in animation, the reordered items should be at
  // their final positions instantly.
  base::AutoReset auto_reset(&enable_item_move_animation_, false);

  // Prevent the opacity from changing before starting the fade in animation.
  // It is necessary because `PagedAppsGridView::UpdateOpacity()` updates
  // the apps grid opacity based on the app list state.
  // TODO(https://crbug.com/1289380): remove this line when a better solution
  // is came up with.
  base::ScopedClosureRunner runner = LockAppsGridOpacity();

  callback.Run(aborted);

  if (fade_out_done_closure_for_test_)
    std::move(fade_out_done_closure_for_test_).Run();

  // When the fade out animation is abortted, the fade in animation should not
  // run. Hence, the reorder animation ends. The aborted animation's smoothness
  // is not reported.
  if (aborted) {
    reorder_animation_status_ = AppListReorderAnimationStatus::kEmpty;
    MaybeRunNextReorderAnimationCallbackForTest(
        /*aborted=*/true, AppListReorderAnimationStatus::kFadeOutAnimation);

    // Reset `reorder_animation_tracker_` without calling Stop() because the
    // aborted animation's smoothness is not reported.
    reorder_animation_tracker_.reset();
  }
}

void AppsGridView::OnFadeInAnimationEnded(ReorderAnimationCallback callback,
                                          bool aborted) {
  // If the animation is aborted, reset the apps grid's layer.
  if (aborted)
    layer()->SetOpacity(1.f);

  // Ensure that all item views are visible after fade in animation completes.
  for (int view_index = 0; view_index < view_model_.view_size(); ++view_index) {
    view_model_.view_at(view_index)->SetVisible(true);
  }

  reorder_animation_status_ = AppListReorderAnimationStatus::kEmpty;

  // Do not report the smoothness data for the aborted animation.
  if (!aborted)
    reorder_animation_tracker_->Stop();
  reorder_animation_tracker_.reset();

  // Clean app list items' layers.
  OnBoundsAnimatorDone(nullptr);

  if (!callback.is_null())
    callback.Run(aborted);

  MaybeRunNextReorderAnimationCallbackForTest(
      aborted, AppListReorderAnimationStatus::kFadeInAnimation);
}

void AppsGridView::MaybeRunNextReorderAnimationCallbackForTest(
    bool aborted,
    AppListReorderAnimationStatus animation_source) {
  if (reorder_animation_callback_queue_for_test_.empty())
    return;

  TestReorderDoneCallbackType front_callback =
      std::move(reorder_animation_callback_queue_for_test_.front());
  reorder_animation_callback_queue_for_test_.pop();
  std::move(front_callback).Run(aborted, animation_source);
}

BEGIN_METADATA(AppsGridView, views::View)
END_METADATA

}  // namespace ash
