// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/profiles/profile_picker_view.h"

#include "base/bind.h"
#include "base/callback.h"
#include "base/callback_helpers.h"
#include "base/containers/contains.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/metrics/histogram_functions.h"
#include "base/notreached.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_avatar_icon_util.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/signin/signin_promo.h"
#include "chrome/browser/signin/signin_util.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/views/accelerator_table.h"
#include "chrome/browser/ui/views/profiles/profile_creation_signed_in_flow_controller.h"
#include "chrome/browser/ui/webui/signin/profile_picker_ui.h"
#include "chrome/browser/ui/webui/signin/signin_web_dialog_ui.h"
#include "chrome/browser/ui/webui/signin/sync_confirmation_ui.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/chromium_strings.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/google_chrome_strings.h"
#include "components/keep_alive_registry/keep_alive_types.h"
#include "components/prefs/pref_service.h"
#include "components/signin/public/base/signin_metrics.h"
#include "components/startup_metric_utils/browser/startup_metric_utils.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "net/base/url_util.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/theme_provider.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"
#include "url/url_constants.h"

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
#include "chrome/browser/ui/views/profiles/profile_picker_dice_sign_in_provider.h"
#include "chrome/browser/ui/views/profiles/profile_picker_dice_sign_in_toolbar.h"
#endif

#if BUILDFLAG(IS_WIN)
#include "chrome/browser/shell_integration_win.h"
#include "ui/base/win/shell.h"
#include "ui/views/win/hwnd_util.h"
#endif

#if BUILDFLAG(IS_MAC)
#include "chrome/browser/global_keyboard_shortcuts_mac.h"
#endif

#if BUILDFLAG(IS_CHROMEOS_LACROS)
#include "chrome/browser/ui/views/profiles/lacros_first_run_signed_in_flow_controller.h"
#endif

namespace {

ProfilePickerView* g_profile_picker_view = nullptr;
base::OnceClosure* g_profile_picker_opened_callback_for_testing = nullptr;

constexpr int kWindowWidth = 1024;
constexpr int kWindowHeight = 758;
constexpr float kMaxRatioOfWorkArea = 0.9;

constexpr base::TimeDelta kExtendedAccountInfoTimeout = base::Seconds(10);

constexpr int kSupportedAcceleratorCommands[] = {
    IDC_CLOSE_TAB,  IDC_CLOSE_WINDOW,    IDC_EXIT,
    IDC_FULLSCREEN, IDC_MINIMIZE_WINDOW, IDC_BACK,
#if BUILDFLAG(ENABLE_DICE_SUPPORT)
    IDC_RELOAD
#endif
};

class ProfilePickerWidget : public views::Widget {
 public:
  explicit ProfilePickerWidget(ProfilePickerView* profile_picker_view)
      : profile_picker_view_(profile_picker_view) {
    views::Widget::InitParams params;
    params.delegate = profile_picker_view_;
    Init(std::move(params));
  }
  ~ProfilePickerWidget() override = default;

  // views::Widget:
  const ui::ThemeProvider* GetThemeProvider() const override {
    Profile* profile = profile_picker_view_->GetProfileBeingCreated();
    if (profile)
      return &ThemeService::GetThemeProviderForProfile(profile);
    return nullptr;
  }
  ui::ColorProviderManager::ThemeInitializerSupplier* GetCustomTheme()
      const override {
    return profile_picker_view_->GetCustomThemeForProfileBeingCreated();
  }

 private:
  const raw_ptr<ProfilePickerView> profile_picker_view_;
};

}  // namespace

// static
void ProfilePicker::Show(Params&& params) {
  // Re-open with new params if necessary.
  if (g_profile_picker_view && g_profile_picker_view->MaybeReopen(params))
    return;

  if (!g_profile_picker_view)
    g_profile_picker_view = new ProfilePickerView(std::move(params));
  g_profile_picker_view->Display();
}

// static
GURL ProfilePicker::GetOnSelectProfileTargetUrl() {
  if (g_profile_picker_view) {
    return g_profile_picker_view->GetOnSelectProfileTargetUrl();
  }
  return GURL();
}

// static
base::FilePath ProfilePicker::GetSwitchProfilePath() {
  if (g_profile_picker_view && g_profile_picker_view->signed_in_flow_) {
    return g_profile_picker_view->signed_in_flow_->switch_profile_path();
  }
  return base::FilePath();
}

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
// static
void ProfilePicker::SwitchToDiceSignIn(
    absl::optional<SkColor> profile_color,
    base::OnceCallback<void(bool)> switch_finished_callback) {
  if (g_profile_picker_view) {
    g_profile_picker_view->SwitchToDiceSignIn(
        profile_color, std::move(switch_finished_callback));
  }
}
#endif

// static
void ProfilePicker::SwitchToSignedInFlow(absl::optional<SkColor> profile_color,
                                         Profile* signed_in_profile) {
  if (g_profile_picker_view) {
    g_profile_picker_view->SwitchToSignedInFlow(
        profile_color, signed_in_profile,
        content::WebContents::Create(
            content::WebContents::CreateParams(signed_in_profile)),
        /*is_saml=*/false);
  }
}

// static
void ProfilePicker::CancelSignedInFlow() {
  if (g_profile_picker_view) {
    g_profile_picker_view->CancelSignedInFlow();
  }
}

// static
base::FilePath ProfilePicker::GetPickerProfilePath() {
  return ProfileManager::GetSystemProfilePath();
}

// static
void ProfilePicker::ShowDialog(content::BrowserContext* browser_context,
                               const GURL& url,
                               const base::FilePath& profile_path) {
  if (g_profile_picker_view) {
    g_profile_picker_view->ShowDialog(browser_context, url, profile_path);
  }
}

// static
void ProfilePicker::HideDialog() {
  if (g_profile_picker_view) {
    g_profile_picker_view->HideDialog();
  }
}

// static
base::FilePath ProfilePicker::GetForceSigninProfilePath() {
  if (g_profile_picker_view) {
    return g_profile_picker_view->GetForceSigninProfilePath();
  }

  return base::FilePath();
}

// static
void ProfilePicker::Hide() {
  if (g_profile_picker_view)
    g_profile_picker_view->Clear();
}

// static
bool ProfilePicker::IsOpen() {
  return g_profile_picker_view;
}

bool ProfilePicker::IsActive() {
  if (!IsOpen())
    return false;

#if BUILDFLAG(IS_MAC)
  return g_profile_picker_view->GetWidget()->IsVisible();
#else
  return g_profile_picker_view->GetWidget()->IsActive();
#endif
}

// static
views::WebView* ProfilePicker::GetWebViewForTesting() {
  if (!g_profile_picker_view)
    return nullptr;
  return g_profile_picker_view->web_view_;
}

// static
views::View* ProfilePicker::GetViewForTesting() {
  return g_profile_picker_view;
}

// static
void ProfilePicker::AddOnProfilePickerOpenedCallbackForTesting(
    base::OnceClosure callback) {
  DCHECK(!g_profile_picker_opened_callback_for_testing);
  DCHECK(!callback.is_null());
  g_profile_picker_opened_callback_for_testing =
      new base::OnceClosure(std::move(callback));
}

// static
void ProfilePicker::SetExtendedAccountInfoTimeoutForTesting(
    base::TimeDelta timeout) {
  if (g_profile_picker_view) {
    g_profile_picker_view->SetExtendedAccountInfoTimeoutForTesting(  // IN-TEST
        timeout);
  }
}

// ProfilePickerForceSigninDialog
// -------------------------------------------------------------

// static
void ProfilePickerForceSigninDialog::ShowReauthDialog(
    content::BrowserContext* browser_context,
    const std::string& email,
    const base::FilePath& profile_path) {
  DCHECK(signin_util::IsForceSigninEnabled());
  if (!ProfilePicker::IsActive())
    return;
  GURL url = signin::GetEmbeddedReauthURLWithEmail(
      signin_metrics::AccessPoint::ACCESS_POINT_USER_MANAGER,
      signin_metrics::Reason::kReauthentication, email);
  ProfilePicker::ShowDialog(browser_context, url, profile_path);
}

// static
void ProfilePickerForceSigninDialog::ShowForceSigninDialog(
    content::BrowserContext* browser_context,
    const base::FilePath& profile_path) {
  DCHECK(signin_util::IsForceSigninEnabled());
  if (!ProfilePicker::IsActive())
    return;

  GURL url = signin::GetEmbeddedPromoURL(
      signin_metrics::AccessPoint::ACCESS_POINT_USER_MANAGER,
      signin_metrics::Reason::kForcedSigninPrimaryAccount, true);

  ProfilePicker::ShowDialog(browser_context, url, profile_path);
}

void ProfilePickerForceSigninDialog::ShowDialogAndDisplayErrorMessage(
    content::BrowserContext* browser_context) {
  DCHECK(signin_util::IsForceSigninEnabled());
  if (!ProfilePicker::IsActive())
    return;

  GURL url(chrome::kChromeUISigninErrorURL);
  ProfilePicker::ShowDialog(browser_context, url, base::FilePath());
  return;
}

// static
void ProfilePickerForceSigninDialog::DisplayErrorMessage() {
  DCHECK(signin_util::IsForceSigninEnabled());
  if (g_profile_picker_view) {
    g_profile_picker_view->DisplayErrorMessage();
  }
}

// static
void ProfilePickerForceSigninDialog::HideDialog() {
  ProfilePicker::HideDialog();
}

// ProfilePickerView::NavigationFinishedObserver ------------------------------

ProfilePickerView::NavigationFinishedObserver::NavigationFinishedObserver(
    const GURL& url,
    base::OnceClosure closure,
    content::WebContents* contents)
    : content::WebContentsObserver(contents),
      url_(url),
      closure_(std::move(closure)) {}

ProfilePickerView::NavigationFinishedObserver::~NavigationFinishedObserver() =
    default;

void ProfilePickerView::NavigationFinishedObserver::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!closure_ || navigation_handle->GetURL() != url_ ||
      !navigation_handle->HasCommitted()) {
    return;
  }
  std::move(closure_).Run();
}

// ProfilePickerView ----------------------------------------------------------

// Returns the initialized profile or nullptr if the profile has not been
// initialized yet or not in the dice flow.
Profile* ProfilePickerView::GetProfileBeingCreated() const {
#if BUILDFLAG(ENABLE_DICE_SUPPORT)
  // Theme provider is only needed for the dice flow.
  if (dice_sign_in_provider_ && dice_sign_in_provider_->IsInitialized()) {
    return dice_sign_in_provider_->GetInitializedProfile();
  }
#endif
  return nullptr;
}

ui::ColorProviderManager::ThemeInitializerSupplier*
ProfilePickerView::GetCustomThemeForProfileBeingCreated() const {
#if BUILDFLAG(ENABLE_DICE_SUPPORT)
  // Custom theme is only needed for the dice flow.
  if (dice_sign_in_provider_)
    return dice_sign_in_provider_->GetCustomTheme();
#endif
  return nullptr;
}

void ProfilePickerView::DisplayErrorMessage() {
  dialog_host_.DisplayErrorMessage();
}

#if BUILDFLAG(IS_CHROMEOS_LACROS)
void ProfilePickerView::NotifyAccountSelected(const std::string& gaia_id) {
  params_.NotifyAccountSelected(gaia_id);
}
#endif

void ProfilePickerView::ShowScreen(
    content::WebContents* contents,
    const GURL& url,
    base::OnceClosure navigation_finished_closure) {
  if (url.is_empty()) {
    DCHECK(!navigation_finished_closure);
    ShowScreenFinished(contents);
    return;
  }

  contents->GetController().LoadURL(url, content::Referrer(),
                                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                    std::string());

  // Special-case the first ever screen to make sure the WebView has a contents
  // assigned in the moment when it gets displayed. This avoids a black flash on
  // Win (and potentially other GPU artifacts on other platforms). The rest of
  // the work can still be done asynchronously in ShowScreenFinished().
  if (web_view_->GetWebContents() == nullptr)
    web_view_->SetWebContents(contents);

  // Binding as Unretained as `this` outlives member
  // `show_screen_finished_observer_`. If ShowScreen gets called twice in a
  // short period of time, the first callback may never get called as the first
  // observer gets destroyed here or later in ShowScreenFinished(). This is okay
  // as all the previous values get replaced by the new values.
  show_screen_finished_observer_ = std::make_unique<NavigationFinishedObserver>(
      url,
      base::BindOnce(&ProfilePickerView::ShowScreenFinished,
                     base::Unretained(this), contents,
                     std::move(navigation_finished_closure)),
      contents);

  if (!GetWidget()->IsVisible())
    GetWidget()->Show();
}

void ProfilePickerView::ShowScreenInPickerContents(
    const GURL& url,
    base::OnceClosure navigation_finished_closure) {
  ShowScreen(contents_.get(), url, std::move(navigation_finished_closure));
}

void ProfilePickerView::Clear() {
  TRACE_EVENT1("browser,startup", "ProfilePickerView::Clear", "state", state_);
  if (state_ == kClosing)
    return;

  if (state_ == kReady) {
    GetWidget()->Close();
    state_ = kClosing;
    return;
  }

  WindowClosing();
  DeleteDelegate();
}

bool ProfilePickerView::ShouldUseDarkColors() const {
  return GetNativeTheme()->ShouldUseDarkColors();
}

bool ProfilePickerView::HandleKeyboardEvent(
    content::WebContents* source,
    const content::NativeWebKeyboardEvent& event) {
  // Forward the keyboard event to AcceleratorPressed() through the
  // FocusManager.
  return unhandled_keyboard_event_handler_.HandleKeyboardEvent(
      event, GetFocusManager());
}

bool ProfilePickerView::HandleContextMenu(
    content::RenderFrameHost& render_frame_host,
    const content::ContextMenuParams& params) {
  // Ignores context menu.
  return true;
}

gfx::NativeView ProfilePickerView::GetHostView() const {
  return GetWidget()->GetNativeView();
}

gfx::Point ProfilePickerView::GetDialogPosition(const gfx::Size& size) {
  gfx::Size widget_size = GetWidget()->GetWindowBoundsInScreen().size();
  return gfx::Point(std::max(0, (widget_size.width() - size.width()) / 2), 0);
}

gfx::Size ProfilePickerView::GetMaximumDialogSize() {
  return GetWidget()->GetWindowBoundsInScreen().size();
}

void ProfilePickerView::AddObserver(
    web_modal::ModalDialogHostObserver* observer) {}

void ProfilePickerView::RemoveObserver(
    web_modal::ModalDialogHostObserver* observer) {}

ProfilePickerView::ProfilePickerView(ProfilePicker::Params&& params)
    : keep_alive_(KeepAliveOrigin::USER_MANAGER_VIEW,
                  KeepAliveRestartOption::DISABLED),
      params_(std::move(params)),
      extended_account_info_timeout_(kExtendedAccountInfoTimeout) {
  // Setup the WidgetDelegate.
  SetHasWindowSizeControls(true);
  SetTitle(IDS_PRODUCT_NAME);

  ConfigureAccelerators();

  // Record creation metrics.
  base::UmaHistogramEnumeration("ProfilePicker.Shown", params_.entry_point());
  if (params_.entry_point() == ProfilePicker::EntryPoint::kOnStartup) {
    DCHECK(creation_time_on_startup_.is_null());
    creation_time_on_startup_ = base::TimeTicks::Now();
    base::UmaHistogramTimes("ProfilePicker.StartupTime.BeforeCreation",
                            creation_time_on_startup_ -
                                startup_metric_utils::MainEntryPointTicks());
  }

  // TODO(crbug.com/1063856): Add |RecordDialogCreation|.
}

ProfilePickerView::~ProfilePickerView() {
  if (contents_)
    contents_->SetDelegate(nullptr);
}

bool ProfilePickerView::MaybeReopen(ProfilePicker::Params& params) {
  // Need to reopen if already closing or if `profile_path()` differs
  // from the current one (as we can't switch the profile during run-time).
  if (state_ != kClosing && params_.profile_path() == params.profile_path())
    return false;

  restart_on_window_closing_ =
      base::BindOnce(&ProfilePicker::Show, std::move(params));
  // No-op if already closing.
  ProfilePicker::Hide();
  return true;
}

void ProfilePickerView::Display() {
  DCHECK_NE(state_, kClosing);
  TRACE_EVENT2("browser,startup", "ProfilePickerView::Display", "entry_point",
               params_.entry_point(), "state", state_);

  if (state_ == kNotStarted) {
    state_ = kInitializing;
    // Build the layout synchronously before creating the picker profile to
    // simplify tests.
    BuildLayout();

    g_browser_process->profile_manager()->CreateProfileAsync(
        params_.profile_path(),
        base::BindRepeating(&ProfilePickerView::OnPickerProfileCreated,
                            weak_ptr_factory_.GetWeakPtr()));
    return;
  }

  if (state_ == kInitializing)
    return;

  GetWidget()->Activate();
}

void ProfilePickerView::OnPickerProfileCreated(Profile* picker_profile,
                                               Profile::CreateStatus status) {
  TRACE_EVENT2("browser,startup", "ProfilePickerView::OnPickerProfileCreated",
               "profile_path",
               (picker_profile ? picker_profile->GetPath().AsUTF8Unsafe() : ""),
               "status", status);
  DCHECK_NE(status, Profile::CREATE_STATUS_LOCAL_FAIL);
  if (status != Profile::CREATE_STATUS_INITIALIZED)
    return;

  Init(picker_profile);
}

void ProfilePickerView::Init(Profile* picker_profile) {
  DCHECK_EQ(state_, kInitializing);
  TRACE_EVENT1(
      "browser,startup", "ProfilePickerView::Init", "profile_path",
      (picker_profile ? picker_profile->GetPath().AsUTF8Unsafe() : ""));
  contents_ = content::WebContents::Create(
      content::WebContents::CreateParams(picker_profile));
  contents_->SetDelegate(this);

  // Destroy the System Profile when the ProfilePickerView is closed (assuming
  // its refcount hits 0). We need to use GetOriginalProfile() here because
  // |profile_picker| is an OTR Profile, and ScopedProfileKeepAlive only
  // supports non-OTR Profiles. Trying to acquire a keepalive on the OTR Profile
  // would trigger a DCHECK.
  //
  // TODO(crbug.com/1153922): Once OTR Profiles use refcounting, remove the call
  // to GetOriginalProfile(). The OTR Profile will hold a keepalive on the
  // regular Profile, so the ownership model will be more straightforward.
  profile_keep_alive_ = std::make_unique<ScopedProfileKeepAlive>(
      picker_profile->GetOriginalProfile(),
      ProfileKeepAliveOrigin::kProfilePickerView);

  // The widget is owned by the native widget.
  new ProfilePickerWidget(this);

#if BUILDFLAG(IS_WIN)
  // Set the app id for the user manager to the app id of its parent.
  ui::win::SetAppIdForWindow(
      shell_integration::win::GetAppUserModelIdForBrowser(
          picker_profile->GetPath()),
      views::HWNDForWidget(GetWidget()));
#endif

  if (params_.entry_point() ==
      ProfilePicker::EntryPoint::kLacrosPrimaryProfileFirstRun) {
#if BUILDFLAG(IS_CHROMEOS_LACROS)
    // TODO(crbug.com/1300109): Consider some refactoring to share this
    // `WebContents` for usage in this class instead of a separate `contents_`.
    std::unique_ptr<content::WebContents> contents_for_signed_in_flow =
        content::WebContents::Create(
            content::WebContents::CreateParams(picker_profile));

    signed_in_flow_ = std::make_unique<LacrosFirstRunSignedInFlowController>(
        this, picker_profile, std::move(contents_for_signed_in_flow),
        /*profile_color=*/absl::optional<SkColor>(),
        base::BindOnce(&ProfilePicker::Params::NotifyFirstRunFinished,
                       base::Unretained(&params_)));
    signed_in_flow_->Init();
#else
    NOTREACHED();
#endif  // BUILDFLAG(IS_CHROMEOS_LACROS)
  } else {
    ShowScreenInPickerContents(params_.GetInitialURL());
  }
  state_ = kReady;

  PrefService* prefs = g_browser_process->local_state();
  prefs->SetBoolean(prefs::kBrowserProfilePickerShown, true);

  if (params_.entry_point() == ProfilePicker::EntryPoint::kOnStartup) {
    DCHECK(!creation_time_on_startup_.is_null());
    base::UmaHistogramTimes("ProfilePicker.StartupTime.WebViewCreated",
                            base::TimeTicks::Now() - creation_time_on_startup_);
  }

  if (g_profile_picker_opened_callback_for_testing) {
    std::move(*g_profile_picker_opened_callback_for_testing).Run();
    delete g_profile_picker_opened_callback_for_testing;
    g_profile_picker_opened_callback_for_testing = nullptr;
  }
}

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
void ProfilePickerView::SwitchToDiceSignIn(
    absl::optional<SkColor> profile_color,
    base::OnceCallback<void(bool)> switch_finished_callback) {
  // TODO(crbug.com/1227029): Consider having forced signin as another
  // implementation of an abstract signin interface to move the code out of
  // this class.
  if (signin_util::IsForceSigninEnabled()) {
    size_t icon_index = profiles::GetPlaceholderAvatarIndex();
    ProfileManager::CreateMultiProfileAsync(
        g_browser_process->profile_manager()
            ->GetProfileAttributesStorage()
            .ChooseNameForNewProfile(icon_index),
        icon_index, /*is_hidden=*/true,
        base::BindRepeating(
            &ProfilePickerView::OnProfileForDiceForcedSigninCreated,
            weak_ptr_factory_.GetWeakPtr(),
            base::OwnedRef(std::move(switch_finished_callback))));
    return;
  }

  if (!dice_sign_in_provider_) {
    dice_sign_in_provider_ =
        std::make_unique<ProfilePickerDiceSignInProvider>(this, toolbar_);
  }

  dice_sign_in_provider_->SwitchToSignIn(
      std::move(switch_finished_callback),
      base::BindOnce(&ProfilePickerView::OnDiceSigninFinished,
                     weak_ptr_factory_.GetWeakPtr(), profile_color));
}

void ProfilePickerView::OnProfileForDiceForcedSigninCreated(
    base::OnceCallback<void(bool)>& switch_finished_callback,
    Profile* profile,
    Profile::CreateStatus status) {
  DCHECK(signin_util::IsForceSigninEnabled());

  if (status == Profile::CREATE_STATUS_LOCAL_FAIL) {
    std::move(switch_finished_callback).Run(false);
    return;
  } else if (status != Profile::CREATE_STATUS_INITIALIZED) {
    return;
  }

  DCHECK(profile);
  std::move(switch_finished_callback).Run(true);

  ProfilePickerForceSigninDialog::ShowForceSigninDialog(
      web_view_->GetWebContents()->GetBrowserContext(), profile->GetPath());
}

void ProfilePickerView::OnDiceSigninFinished(
    absl::optional<SkColor> profile_color,
    Profile* signed_in_profile,
    std::unique_ptr<content::WebContents> contents,
    bool is_saml) {
  SwitchToSignedInFlow(profile_color, signed_in_profile, std::move(contents),
                       is_saml);
  // Reset the provider after switching to signed-in flow to make sure there's
  // always one ScopedProfileKeepAlive.
  dice_sign_in_provider_.reset();
}
#endif

void ProfilePickerView::SwitchToSignedInFlow(
    absl::optional<SkColor> profile_color,
    Profile* signed_in_profile,
    std::unique_ptr<content::WebContents> contents,
    bool is_saml) {
  DCHECK(!signed_in_flow_);
  signed_in_flow_ = std::make_unique<ProfileCreationSignedInFlowController>(
      this, signed_in_profile, std::move(contents), profile_color,
      extended_account_info_timeout_, is_saml);
  signed_in_flow_->Init();
}

void ProfilePickerView::CancelSignedInFlow() {
  DCHECK(signed_in_flow_);

  signed_in_flow_->Cancel();

  switch (params_.entry_point()) {
    case ProfilePicker::EntryPoint::kOnStartup:
    case ProfilePicker::EntryPoint::kProfileMenuManageProfiles:
    case ProfilePicker::EntryPoint::kOpenNewWindowAfterProfileDeletion:
    case ProfilePicker::EntryPoint::kNewSessionOnExistingProcess:
    case ProfilePicker::EntryPoint::kProfileLocked:
    case ProfilePicker::EntryPoint::kUnableToCreateBrowser:
    case ProfilePicker::EntryPoint::kBackgroundModeManager: {
      // Navigate to the very beginning which is guaranteed to be the profile
      // picker.
      contents_->GetController().GoToIndex(0);
      ShowScreenInPickerContents(GURL());
      // Reset the sign-in flow.
      signed_in_flow_.reset();
      return;
    }
    case ProfilePicker::EntryPoint::kProfileMenuAddNewProfile: {
      // This results in destroying `this` incl. `sign_in_`.
      Clear();
      return;
    }
    case ProfilePicker::EntryPoint::kLacrosSelectAvailableAccount:
    case ProfilePicker::EntryPoint::kLacrosPrimaryProfileFirstRun:
      NOTREACHED() << "Signed in flow is not reachable from this entry point";
  }
}

void ProfilePickerView::WindowClosing() {
  views::WidgetDelegateView::WindowClosing();
  // Now that the window is closed, we can allow a new one to be opened.
  // (WindowClosing comes in asynchronously from the call to Close() and we
  // may have already opened a new instance).
  if (g_profile_picker_view == this)
    g_profile_picker_view = nullptr;

  // Show a new profile window if it has been requested while the current window
  // was closing.
  if (state_ == kClosing && restart_on_window_closing_)
    std::move(restart_on_window_closing_).Run();
}

views::ClientView* ProfilePickerView::CreateClientView(views::Widget* widget) {
  return new views::ClientView(widget, TransferOwnershipOfContentsView());
}

views::View* ProfilePickerView::GetContentsView() {
  return this;
}

std::u16string ProfilePickerView::GetAccessibleWindowTitle() const {
  if (!web_view_ || !web_view_->GetWebContents() ||
      web_view_->GetWebContents()->GetTitle().empty()) {
#if BUILDFLAG(IS_CHROMEOS_LACROS)
    return l10n_util::GetStringUTF16(IDS_PROFILE_PICKER_MAIN_VIEW_TITLE_LACROS);
#else
    return l10n_util::GetStringUTF16(IDS_PROFILE_PICKER_MAIN_VIEW_TITLE);
#endif
  }
  return web_view_->GetWebContents()->GetTitle();
}

gfx::Size ProfilePickerView::CalculatePreferredSize() const {
  gfx::Size preferred_size = gfx::Size(kWindowWidth, kWindowHeight);
  gfx::Size work_area_size = GetWidget()->GetWorkAreaBoundsInScreen().size();
  // Keep the window smaller then |work_area_size| so that it feels more like a
  // dialog then like the actual Chrome window.
  gfx::Size max_dialog_size = ScaleToFlooredSize(
      work_area_size, kMaxRatioOfWorkArea, kMaxRatioOfWorkArea);
  preferred_size.SetToMin(max_dialog_size);
  return preferred_size;
}

gfx::Size ProfilePickerView::GetMinimumSize() const {
  // On small screens, the preferred size may be smaller than the picker
  // minimum size. In that case there will be scrollbars on the picker.
  gfx::Size minimum_size = GetPreferredSize();
  minimum_size.SetToMin(ProfilePickerUI::GetMinimumSize());
  return minimum_size;
}

bool ProfilePickerView::AcceleratorPressed(const ui::Accelerator& accelerator) {
  const auto& iter = accelerator_table_.find(accelerator);
  DCHECK(iter != accelerator_table_.end());
  int command_id = iter->second;
  switch (command_id) {
    case IDC_CLOSE_TAB:
    case IDC_CLOSE_WINDOW:
      // kEscKeyPressed is used although that shortcut is disabled (this is
      // Ctrl-Shift-W instead).
      GetWidget()->CloseWithReason(views::Widget::ClosedReason::kEscKeyPressed);
      break;
    case IDC_EXIT:
      chrome::AttemptUserExit();
      break;
    case IDC_FULLSCREEN:
      GetWidget()->SetFullscreen(!GetWidget()->IsFullscreen());
      break;
    case IDC_MINIMIZE_WINDOW:
      GetWidget()->Minimize();
      break;
    case IDC_BACK: {
      NavigateBack();
      break;
    }
#if BUILDFLAG(ENABLE_DICE_SUPPORT)
    // Always reload bypassing cache.
    case IDC_RELOAD:
    case IDC_RELOAD_BYPASSING_CACHE:
    case IDC_RELOAD_CLEARING_CACHE: {
      // Sign-in may fail due to connectivity issues, allow reloading.
      if (GetDiceSigningIn()) {
        dice_sign_in_provider_->ReloadSignInPage();
      }
      break;
    }
#endif
    default:
      NOTREACHED() << "Unexpected command_id: " << command_id;
      break;
  }

  return true;
}

void ProfilePickerView::BuildLayout() {
  SetLayoutManager(std::make_unique<views::FlexLayout>())
      ->SetOrientation(views::LayoutOrientation::kVertical)
      .SetMainAxisAlignment(views::LayoutAlignment::kStart)
      .SetCrossAxisAlignment(views::LayoutAlignment::kStretch)
      .SetDefault(
          views::kFlexBehaviorKey,
          views::FlexSpecification(views::MinimumFlexSizeRule::kScaleToMinimum,
                                   views::MaximumFlexSizeRule::kUnbounded));

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
  auto toolbar = std::make_unique<ProfilePickerDiceSignInToolbar>();
  toolbar_ = AddChildView(std::move(toolbar));
  // Toolbar gets built and set visible once we it's needed for the Dice signin.
  toolbar_->SetVisible(false);
#endif

  auto web_view = std::make_unique<views::WebView>();
  web_view->set_allow_accelerators(true);
  web_view_ = AddChildView(std::move(web_view));
}

void ProfilePickerView::ShowScreenFinished(
    content::WebContents* contents,
    base::OnceClosure navigation_finished_closure) {
  // Stop observing for this (or any previous) navigation.
  if (show_screen_finished_observer_)
    show_screen_finished_observer_.reset();

  web_view_->SetWebContents(contents);
  contents->Focus();

  if (navigation_finished_closure)
    std::move(navigation_finished_closure).Run();
}

void ProfilePickerView::BackButtonPressed(const ui::Event& event) {
  NavigateBack();
}

void ProfilePickerView::NavigateBack() {
  // Navigating back is not allowed when in the sync-setup phase of profile
  // creation.
  if (signed_in_flow_)
    return;

  // Go back in the picker WebContents if it's currently displayed.
  if (contents_ && web_view_->GetWebContents() == contents_.get() &&
      web_view_->GetWebContents()->GetController().CanGoBack()) {
    web_view_->GetWebContents()->GetController().GoBack();
    return;
  }

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
  if (GetDiceSigningIn())
    dice_sign_in_provider_->NavigateBack();
#endif
}

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
bool ProfilePickerView::GetDiceSigningIn() const {
  // We are on the sign-in screen if the sign-in provider exists.
  return static_cast<bool>(dice_sign_in_provider_);
}
#endif

void ProfilePickerView::SetExtendedAccountInfoTimeoutForTesting(
    base::TimeDelta timeout) {
  extended_account_info_timeout_ = timeout;
}

void ProfilePickerView::ConfigureAccelerators() {
  const std::vector<AcceleratorMapping> accelerator_list(GetAcceleratorList());
  for (const auto& entry : accelerator_list) {
    if (!base::Contains(kSupportedAcceleratorCommands, entry.command_id))
      continue;
    ui::Accelerator accelerator(entry.keycode, entry.modifiers);
    accelerator_table_[accelerator] = entry.command_id;
    AddAccelerator(accelerator);
  }

#if BUILDFLAG(IS_MAC)
  // Check Mac-specific accelerators. Note: Chrome does not support dynamic or
  // user-configured accelerators on Mac. Default static accelerators are used
  // instead.
  for (int command_id : kSupportedAcceleratorCommands) {
    ui::Accelerator accelerator;
    bool mac_accelerator_found =
        GetDefaultMacAcceleratorForCommandId(command_id, &accelerator);
    if (mac_accelerator_found) {
      accelerator_table_[accelerator] = command_id;
      AddAccelerator(accelerator);
    }
  }
#endif  // BUILDFLAG(IS_MAC)
}

void ProfilePickerView::ShowDialog(content::BrowserContext* browser_context,
                                   const GURL& url,
                                   const base::FilePath& profile_path) {
  gfx::NativeView parent = GetWidget()->GetNativeView();
  dialog_host_.ShowDialog(browser_context, url, profile_path, parent);
}

void ProfilePickerView::HideDialog() {
  dialog_host_.HideDialog();
}

base::FilePath ProfilePickerView::GetForceSigninProfilePath() const {
  return dialog_host_.GetForceSigninProfilePath();
}

GURL ProfilePickerView::GetOnSelectProfileTargetUrl() const {
  return params_.on_select_profile_target_url();
}

#if BUILDFLAG(IS_CHROMEOS_LACROS)
// static
void ProfilePicker::NotifyAccountSelected(const std::string& gaia_id) {
  if (!g_profile_picker_view)
    return;
  g_profile_picker_view->NotifyAccountSelected(gaia_id);
}
#endif

BEGIN_METADATA(ProfilePickerView, views::WidgetDelegateView)
#if BUILDFLAG(ENABLE_DICE_SUPPORT)
ADD_READONLY_PROPERTY_METADATA(bool, DiceSigningIn)
#endif
ADD_READONLY_PROPERTY_METADATA(base::FilePath, ForceSigninProfilePath)
END_METADATA
