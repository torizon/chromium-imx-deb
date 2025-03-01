// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ash/crosapi/browser_util.h"

#include "ash/constants/ash_features.h"
#include "ash/constants/ash_switches.h"
#include "base/command_line.h"
#include "base/containers/contains.h"
#include "base/containers/fixed_flat_map.h"
#include "base/containers/flat_map.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/system/sys_info.h"
#include "base/values.h"
#include "base/version.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/channel_info.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_paths.h"
#include "chromeos/crosapi/cpp/crosapi_constants.h"
#include "chromeos/crosapi/mojom/crosapi.mojom.h"
#include "components/exo/shell_surface_util.h"
#include "components/policy/core/common/policy_map.h"
#include "components/policy/policy_constants.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/user_manager/user.h"
#include "components/user_manager/user_manager.h"
#include "components/version_info/channel.h"
#include "components/version_info/version_info.h"
#include "google_apis/gaia/gaia_auth_util.h"

using user_manager::User;
using version_info::Channel;

namespace crosapi {
namespace browser_util {
namespace {

bool g_lacros_enabled_for_test = false;

bool g_profile_migration_completed_for_test = false;

absl::optional<bool> g_lacros_primary_browser_for_test;

// At session start the value for LacrosAvailability logic is applied and the
// result is stored in this variable which is used after that as a cache.
absl::optional<LacrosAvailability> g_lacros_availability_cache;

// The rootfs lacros-chrome metadata keys.
constexpr char kLacrosMetadataContentKey[] = "content";
constexpr char kLacrosMetadataVersionKey[] = "version";

// The conversion map for LacrosAvailability policy data. The values must match
// the ones from policy_templates.json.
constexpr auto kLacrosAvailabilityMap =
    base::MakeFixedFlatMap<base::StringPiece, LacrosAvailability>({
        {"user_choice", LacrosAvailability::kUserChoice},
        {"lacros_disallowed", LacrosAvailability::kLacrosDisallowed},
        {"side_by_side", LacrosAvailability::kSideBySide},
        {"lacros_primary", LacrosAvailability::kLacrosPrimary},
        {"lacros_only", LacrosAvailability::kLacrosOnly},
    });

// Some account types require features that aren't yet supported by lacros.
// See https://crbug.com/1080693
bool IsUserTypeAllowed(const User* user) {
  switch (user->GetType()) {
    case user_manager::USER_TYPE_REGULAR:
    case user_manager::USER_TYPE_WEB_KIOSK_APP:
    case user_manager::USER_TYPE_PUBLIC_ACCOUNT:
      return true;
    case user_manager::USER_TYPE_CHILD:
      return base::FeatureList::IsEnabled(kLacrosForSupervisedUsers);
    case user_manager::USER_TYPE_GUEST:
    case user_manager::USER_TYPE_KIOSK_APP:
    case user_manager::USER_TYPE_ARC_KIOSK_APP:
    case user_manager::USER_TYPE_ACTIVE_DIRECTORY:
    case user_manager::NUM_USER_TYPES:
      return false;
  }
}

// Returns true if the main profile is associated with a google internal
// account.
bool IsGoogleInternal() {
  user_manager::UserManager* user_manager = user_manager::UserManager::Get();
  const user_manager::User* user = user_manager->GetPrimaryUser();
  if (!user)
    return false;
  return gaia::IsGoogleInternalAccountEmail(
      user->GetAccountId().GetUserEmail());
}

// Returns the lacros integration suggested by the policy lacros-availability.
// There are several reasons why we might choose to ignore the
// lacros-availability policy.
// 1. The user has set a command line or chrome://flag for
//    kLacrosAvailabilityIgnore.
// 2. The user is a Googler and they are not opted into the
//    kLacrosGooglePolicyRollout trial and they did not have the
//    kLacrosDisallowed policy.
LacrosAvailability GetCachedLacrosAvailability() {
  // TODO(crbug.com/1286340): add DCHECK for production use to avoid the
  // same inconsistency for the future.
  if (g_lacros_availability_cache.has_value())
    return g_lacros_availability_cache.value();
  // It could happen in some browser tests that value is not cached. Return
  // default in that case.
  return LacrosAvailability::kUserChoice;
}

// Given a raw policy value, decides what LacrosAvailability value should be
// used as a result of policy application.
LacrosAvailability DetermineLacrosAvailabilityFromPolicyValue(
    base::StringPiece policy_value) {
  // Users can set this switch in chrome://flags to disable the effect of the
  // lacros-availability policy.
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(ash::switches::kLacrosAvailabilityIgnore))
    return LacrosAvailability::kUserChoice;

  if (policy_value.empty()) {
    // Some tests call IsLacrosAllowedToBeEnabled but don't have the value set.
    return LacrosAvailability::kUserChoice;
  }

  auto result = ParseLacrosAvailability(policy_value);
  if (!result.has_value())
    return LacrosAvailability::kUserChoice;

  if (IsGoogleInternal() &&
      !base::FeatureList::IsEnabled(kLacrosGooglePolicyRollout) &&
      result != LacrosAvailability::kLacrosDisallowed) {
    return LacrosAvailability::kUserChoice;
  }

  return result.value();
}

// Gets called from IsLacrosAllowedToBeEnabled with primary user or from
// IsLacrosEnabledForMigration with the user that the
// IsLacrosEnabledForMigration was passed.
bool IsLacrosAllowedToBeEnabledWithUser(
    const User* user,
    LacrosAvailability launch_availability) {
  if (g_lacros_enabled_for_test)
    return true;

  if (!IsUserTypeAllowed(user)) {
    return false;
  }

  switch (launch_availability) {
    case LacrosAvailability::kUserChoice:
      break;
    case LacrosAvailability::kLacrosDisallowed:
      return false;
    case LacrosAvailability::kSideBySide:
    case LacrosAvailability::kLacrosPrimary:
    case LacrosAvailability::kLacrosOnly:
      return true;
  }

  return true;
}

// Called from `IsDataWipeRequired()` or `IsDataWipeRequiredForTesting()`.
// data_version` is the version of last data wipe. `current_version` is the
// version of ash-chrome. `required_version` is the version that introduces some
// breaking change. `data_version` needs to be greater or equal to
// `required_version`. If `required_version` is newer than `current_version`,
// data wipe is not required.
bool IsDataWipeRequiredInternal(base::Version data_version,
                                const base::Version& current_version,
                                const base::Version& required_version) {
  // `data_version` is invalid if any wipe has not been recorded yet. In
  // such a case, assume that the last data wipe happened significantly long
  // time ago.
  if (!data_version.IsValid())
    data_version = base::Version("0");

  if (current_version < required_version) {
    // If `current_version` is smaller than the `required_version`, that means
    // that the data wipe doesn't need to happen yet.
    return false;
  }

  if (data_version >= required_version) {
    // If `data_version` is greater or equal to `required_version`, this means
    // data wipe has already happened and that user data is compatible with the
    // current lacros.
    return false;
  }

  return true;
}

// Returns the string value for the kLacrosStabilitySwitch if present.
absl::optional<std::string> GetLacrosStabilitySwitchValue() {
  const base::CommandLine* cmdline = base::CommandLine::ForCurrentProcess();
  return cmdline->HasSwitch(browser_util::kLacrosStabilitySwitch)
             ? absl::optional<std::string>(cmdline->GetSwitchValueASCII(
                   browser_util::kLacrosStabilitySwitch))
             : absl::nullopt;
}

// Resolves the Lacros stateful channel in the following order:
//   1. From the kLacrosStabilitySwitch command line flag if present.
//   2. From the current ash channel.
Channel GetStatefulLacrosChannel() {
  static const auto kStabilitySwitchToChannelMap =
      base::MakeFixedFlatMap<base::StringPiece, Channel>({
          {browser_util::kLacrosStabilityChannelCanary, Channel::CANARY},
          {browser_util::kLacrosStabilityChannelDev, Channel::DEV},
          {browser_util::kLacrosStabilityChannelBeta, Channel::BETA},
          {browser_util::kLacrosStabilityChannelStable, Channel::STABLE},
      });
  auto stability_switch_value = GetLacrosStabilitySwitchValue();
  return stability_switch_value && base::Contains(kStabilitySwitchToChannelMap,
                                                  *stability_switch_value)
             ? kStabilitySwitchToChannelMap.at(*stability_switch_value)
             : chrome::GetChannel();
}

static_assert(
    crosapi::mojom::Crosapi::Version_ == 69,
    "if you add a new crosapi, please add it to kInterfaceVersionEntries");

}  // namespace

// NOTE: If you change the lacros component names, you must also update
// chrome/browser/component_updater/cros_component_installer_chromeos.cc
const ComponentInfo kLacrosDogfoodCanaryInfo = {
    "lacros-dogfood-canary", "hkifppleldbgkdlijbdfkdpedggaopda"};
const ComponentInfo kLacrosDogfoodDevInfo = {
    "lacros-dogfood-dev", "ldobopbhiamakmncndpkeelenhdmgfhk"};
const ComponentInfo kLacrosDogfoodBetaInfo = {
    "lacros-dogfood-beta", "hnfmbeciphpghlfgpjfbcdifbknombnk"};
const ComponentInfo kLacrosDogfoodStableInfo = {
    "lacros-dogfood-stable", "ehpjbaiafkpkmhjocnenjbbhmecnfcjb"};

// A kill switch for lacros chrome apps.
const base::Feature kLacrosDisableChromeApps{"LacrosDisableChromeApps",
                                             base::FEATURE_DISABLED_BY_DEFAULT};

// When this feature is enabled, Lacros is allowed to roll out by policy to
// Googlers.
const base::Feature kLacrosGooglePolicyRollout{
    "LacrosGooglePolicyRollout", base::FEATURE_DISABLED_BY_DEFAULT};

// Makes LaCrOS allowed for Family Link users.
// With this feature disabled LaCrOS cannot be enabled for Family Link users.
// When this feature is enabled LaCrOS availability is a under control of other
// launch switches.
// Note: Family Link users do not have access to chrome://flags and this feature
// flag is meant to help with development and testing.
const base::Feature kLacrosForSupervisedUsers{
    "LacrosForSupervisedUsers", base::FEATURE_DISABLED_BY_DEFAULT};

const Channel kLacrosDefaultChannel = Channel::DEV;

const char kLacrosStabilitySwitch[] = "lacros-stability";
const char kLacrosStabilityChannelCanary[] = "canary";
const char kLacrosStabilityChannelDev[] = "dev";
const char kLacrosStabilityChannelBeta[] = "beta";
const char kLacrosStabilityChannelStable[] = "stable";

const char kLacrosSelectionSwitch[] = "lacros-selection";
const char kLacrosSelectionRootfs[] = "rootfs";
const char kLacrosSelectionStateful[] = "stateful";

// The internal name in about_flags.cc for the lacros-availablility-policy
// config.
const char kLacrosAvailabilityPolicyInternalName[] =
    "lacros-availability-policy";

// The commandline flag name of lacros-availability-policy.
// The value should be the policy value as defined just below.
// The values need to be consistent with kLacrosAvailabilityMap above.
const char kLacrosAvailabilityPolicySwitch[] = "lacros-availability-policy";
const char kLacrosAvailabilityPolicyUserChoice[] = "user_choice";
const char kLacrosAvailabilityPolicyLacrosDisabled[] = "lacros_disabled";
const char kLacrosAvailabilityPolicySideBySide[] = "side_by_side";
const char kLacrosAvailabilityPolicyLacrosPrimary[] = "lacros_primary";
const char kLacrosAvailabilityPolicyLacrosOnly[] = "lacros_only";

const char kLaunchOnLoginPref[] = "lacros.launch_on_login";
const char kClearUserDataDir1Pref[] = "lacros.clear_user_data_dir_1";
const char kDataVerPref[] = "lacros.data_version";
const char kRequiredDataVersion[] = "92.0.0.0";
const char kProfileMigrationCompletedForUserPref[] =
    "lacros.profile_migration_completed_for_user";

void RegisterProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(kLaunchOnLoginPref, /*default_value=*/false);
  registry->RegisterBooleanPref(kClearUserDataDir1Pref,
                                /*default_value=*/false);
}

void RegisterLocalStatePrefs(PrefRegistrySimple* registry) {
  registry->RegisterDictionaryPref(kDataVerPref);
  registry->RegisterDictionaryPref(kProfileMigrationCompletedForUserPref,
                                   base::DictionaryValue());
}

base::FilePath GetUserDataDir() {
  if (base::SysInfo::IsRunningOnChromeOS()) {
    // NOTE: On device this function is privacy/security sensitive. The
    // directory must be inside the encrypted user partition.
    return base::FilePath(crosapi::kLacrosUserDataPath);
  }
  // For developers on Linux desktop, put the directory under the developer's
  // specified --user-data-dir.
  base::FilePath base_path;
  base::PathService::Get(chrome::DIR_USER_DATA, &base_path);
  return base_path.Append("lacros");
}

bool IsLacrosAllowedToBeEnabled() {
  // Allows tests to avoid enabling the flag, constructing a fake user manager,
  // creating g_browser_process->local_state(), etc.
  if (g_lacros_enabled_for_test)
    return true;

  // TODO(crbug.com/1185813): TaskManagerImplTest is not ready to run with
  // Lacros enabled.
  // UserManager is not initialized for unit tests by default, unless a fake
  // user manager is constructed.
  if (!user_manager::UserManager::IsInitialized())
    return false;

  // GetPrimaryUser works only after user session is started.
  const User* user = user_manager::UserManager::Get()->GetPrimaryUser();
  if (!user) {
    return false;
  }

  return IsLacrosAllowedToBeEnabledWithUser(user,
                                            GetCachedLacrosAvailability());
}

bool IsLacrosEnabled() {
  // Allows tests to avoid enabling the flag, constructing a fake user manager,
  // creating g_browser_process->local_state(), etc.
  if (g_lacros_enabled_for_test)
    return true;

  if (!IsLacrosAllowedToBeEnabled())
    return false;

  // If profile migration is enabled for the user, then make profile migration a
  // requirement to enable lacros.
  if (IsProfileMigrationEnabled(
          user_manager::UserManager::Get()->GetPrimaryUser()->GetAccountId())) {
    PrefService* local_state = g_browser_process->local_state();
    // Note that local_state can be nullptr in tests.
    if (local_state && !IsProfileMigrationCompletedForUser(
                           local_state, user_manager::UserManager::Get()
                                            ->GetPrimaryUser()
                                            ->username_hash())) {
      // If migration has not been completed, do not enable lacros.
      return false;
    }
  }

  switch (GetCachedLacrosAvailability()) {
    case LacrosAvailability::kUserChoice:
      break;
    case LacrosAvailability::kLacrosDisallowed:
      return false;
    case LacrosAvailability::kSideBySide:
    case LacrosAvailability::kLacrosPrimary:
    case LacrosAvailability::kLacrosOnly:
      return true;
  }

  return base::FeatureList::IsEnabled(chromeos::features::kLacrosSupport);
}

bool IsProfileMigrationEnabled(const AccountId& account_id) {
  // Emergency switch to turn off profile migration. Turn this on via Finch in
  // case profile migration needs to be turned off after launch.
  if (base::FeatureList::IsEnabled(
          ash::features::kLacrosProfileMigrationForceOff)) {
    return false;
  }

  //  Currently we turn on profile migration only for Googlers.
  //  `kLacrosProfileMigrationForAnyUser` can be enabled to allow testing with
  //  non-googler accounts.
  if (gaia::IsGoogleInternalAccountEmail(account_id.GetUserEmail()) ||
      base::FeatureList::IsEnabled(
          ash::features::kLacrosProfileMigrationForAnyUser))
    return true;

  return false;
}

bool IsLacrosEnabledForMigration(const User* user,
                                 PolicyInitState policy_init_state) {
  if (g_lacros_enabled_for_test)
    return true;

  LacrosAvailability lacros_availability;
  if (policy_init_state == PolicyInitState::kBeforeInit) {
    // Before Policy is initialized, the value won't be available.
    // So, we'll use the value preserved in the feature flags.
    // See also LacrosAvailabilityPolicyObserver how it will be propergated.
    lacros_availability = DetermineLacrosAvailabilityFromPolicyValue(
        base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
            kLacrosAvailabilityPolicySwitch));
  } else {
    DCHECK_EQ(policy_init_state, PolicyInitState::kAfterInit);
    lacros_availability = GetCachedLacrosAvailability();
  }

  if (!IsLacrosAllowedToBeEnabledWithUser(user, lacros_availability))
    return false;

  switch (lacros_availability) {
    case LacrosAvailability::kUserChoice:
      break;
    case LacrosAvailability::kLacrosDisallowed:
      return false;
    case LacrosAvailability::kSideBySide:
    case LacrosAvailability::kLacrosPrimary:
    case LacrosAvailability::kLacrosOnly:
      return true;
  }

  return base::FeatureList::IsEnabled(chromeos::features::kLacrosSupport);
}

bool IsLacrosSupportFlagAllowed() {
  return IsLacrosAllowedToBeEnabled() &&
         (GetCachedLacrosAvailability() == LacrosAvailability::kUserChoice);
}

void SetLacrosEnabledForTest(bool force_enabled) {
  g_lacros_enabled_for_test = force_enabled;
}

bool IsAshWebBrowserEnabled() {
  // If Lacros is not a primary browser, Ash browser is always enabled.
  if (!IsLacrosPrimaryBrowser())
    return true;

  switch (GetCachedLacrosAvailability()) {
    case LacrosAvailability::kUserChoice:
      break;
    case LacrosAvailability::kLacrosDisallowed:
    case LacrosAvailability::kSideBySide:
    case LacrosAvailability::kLacrosPrimary:
      return true;
    case LacrosAvailability::kLacrosOnly:
      return false;
  }

  return !base::FeatureList::IsEnabled(chromeos::features::kLacrosOnly);
}

bool IsLacrosPrimaryBrowser() {
  if (g_lacros_primary_browser_for_test.has_value())
    return g_lacros_primary_browser_for_test.value();

  if (!IsLacrosEnabled())
    return false;

  // Lacros-chrome will always be the primary browser if Lacros is enabled in
  // web Kiosk session.
  if (user_manager::UserManager::Get()->IsLoggedInAsWebKioskApp())
    return true;

  if (!IsLacrosPrimaryBrowserAllowed())
    return false;

  switch (GetCachedLacrosAvailability()) {
    case LacrosAvailability::kUserChoice:
      break;
    case LacrosAvailability::kLacrosDisallowed:
      NOTREACHED();
      return false;
    case LacrosAvailability::kSideBySide:
      return false;
    case LacrosAvailability::kLacrosPrimary:
    case LacrosAvailability::kLacrosOnly:
      return true;
  }

  return base::FeatureList::IsEnabled(chromeos::features::kLacrosPrimary);
}

void SetLacrosPrimaryBrowserForTest(absl::optional<bool> value) {
  g_lacros_primary_browser_for_test = value;
}

bool IsLacrosPrimaryBrowserAllowed() {
  if (!IsLacrosAllowedToBeEnabled())
    return false;

  switch (GetCachedLacrosAvailability()) {
    case LacrosAvailability::kLacrosDisallowed:
      return false;
    case LacrosAvailability::kLacrosPrimary:
    case LacrosAvailability::kLacrosOnly:
      // Forcibly allow to use Lacros as a Primary respecting the policy.
      return true;
    default:
      // Fallback others.
      break;
  }

  return true;
}

bool IsLacrosPrimaryFlagAllowed() {
  return IsLacrosPrimaryBrowserAllowed() &&
         (GetCachedLacrosAvailability() == LacrosAvailability::kUserChoice);
}

bool IsLacrosOnlyBrowserAllowed() {
  if (!IsLacrosAllowedToBeEnabled())
    return false;

  switch (GetCachedLacrosAvailability()) {
    case LacrosAvailability::kLacrosDisallowed:
      return false;
    case LacrosAvailability::kLacrosOnly:
      // Forcibly allow to use Lacros as a Primary respecting the policy.
      return true;
    case LacrosAvailability::kUserChoice:
    case LacrosAvailability::kSideBySide:
    case LacrosAvailability::kLacrosPrimary:
      // Fallback others.
      break;
  }

  return true;
}

bool IsLacrosOnlyFlagAllowed() {
  return IsLacrosOnlyBrowserAllowed() &&
         (GetCachedLacrosAvailability() == LacrosAvailability::kUserChoice);
}

bool IsLacrosAllowedToLaunch() {
  return user_manager::UserManager::Get()->GetLoggedInUsers().size() == 1;
}

bool IsLacrosChromeAppsEnabled() {
  if (base::FeatureList::IsEnabled(kLacrosDisableChromeApps))
    return false;

  if (!IsLacrosPrimaryBrowser())
    return false;

  return true;
}

bool IsLacrosEnabledInWebKioskSession() {
  return user_manager::UserManager::Get()->IsLoggedInAsWebKioskApp() &&
         base::FeatureList::IsEnabled(features::kWebKioskEnableLacros) &&
         IsLacrosEnabled();
}

bool IsLacrosWindow(const aura::Window* window) {
  const std::string* app_id = exo::GetShellApplicationId(window);
  if (!app_id)
    return false;
  return base::StartsWith(*app_id, kLacrosAppIdPrefix);
}

// Assuming the metadata exists, parse the version and check if it contains the
// non-backwards-compatible account_manager change.
// A typical format for metadata is:
// {
//   "content": {
//     "version": "91.0.4469.5"
//   },
//   "metadata_version": 1
// }
bool DoesMetadataSupportNewAccountManager(base::Value* metadata) {
  if (!metadata)
    return false;

  base::Value* version = metadata->FindPath("content.version");
  if (!version || !version->is_string())
    return false;

  std::string version_str = version->GetString();
  std::vector<std::string> versions_str = base::SplitString(
      version_str, ".", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  if (versions_str.size() != 4)
    return false;

  int major_version = 0;
  int minor_version = 0;
  if (!base::StringToInt(versions_str[0], &major_version))
    return false;
  if (!base::StringToInt(versions_str[2], &minor_version))
    return false;

  // TODO(https://crbug.com/1197220): Come up with more appropriate major/minor
  // version numbers.
  return major_version >= 1000 && minor_version >= 0;
}

base::Version GetDataVer(PrefService* local_state,
                         const std::string& user_id_hash) {
  const base::Value* data_versions = local_state->GetDictionary(kDataVerPref);
  const std::string* data_version_str =
      data_versions->FindStringKey(user_id_hash);

  if (!data_version_str)
    return base::Version();

  return base::Version(*data_version_str);
}

void RecordDataVer(PrefService* local_state,
                   const std::string& user_id_hash,
                   const base::Version& version) {
  DCHECK(version.IsValid());
  DictionaryPrefUpdate update(local_state, kDataVerPref);
  base::Value* dict = update.Get();
  dict->SetStringKey(user_id_hash, version.GetString());
}

bool IsDataWipeRequired(const std::string& user_id_hash) {
  base::Version data_version =
      GetDataVer(g_browser_process->local_state(), user_id_hash);
  base::Version current_version = version_info::GetVersion();
  base::Version required_version =
      base::Version(base::StringPiece(kRequiredDataVersion));

  return IsDataWipeRequiredInternal(data_version, current_version,
                                    required_version);
}

bool IsDataWipeRequiredForTesting(base::Version data_version,
                                  const base::Version& current_version,
                                  const base::Version& required_version) {
  return IsDataWipeRequiredInternal(data_version, current_version,
                                    required_version);
}

base::Version GetRootfsLacrosVersionMayBlock(
    const base::FilePath& version_file_path) {
  if (!base::PathExists(version_file_path)) {
    LOG(WARNING) << "The rootfs lacros-chrome metadata is missing.";
    return {};
  }

  std::string metadata;
  if (!base::ReadFileToString(version_file_path, &metadata)) {
    PLOG(WARNING) << "Failed to read rootfs lacros-chrome metadata.";
    return {};
  }

  absl::optional<base::Value> v = base::JSONReader::Read(metadata);
  if (!v || !v->is_dict()) {
    LOG(WARNING) << "Failed to parse rootfs lacros-chrome metadata.";
    return {};
  }

  const base::Value* content = v->FindKey(kLacrosMetadataContentKey);
  if (!content || !content->is_dict()) {
    LOG(WARNING)
        << "Failed to parse rootfs lacros-chrome metadata content key.";
    return {};
  }

  const base::Value* version = content->FindKey(kLacrosMetadataVersionKey);
  if (!version || !version->is_string()) {
    LOG(WARNING)
        << "Failed to parse rootfs lacros-chrome metadata version key.";
    return {};
  }

  return base::Version{version->GetString()};
}

void CacheLacrosAvailability(const policy::PolicyMap& map) {
  if (g_lacros_availability_cache.has_value()) {
    // Some browser tests might call this multiple times.
    LOG(ERROR) << "Trying to cache LacrosAvailability and the value was set";
    return;
  }

  const base::Value* value =
      map.GetValue(policy::key::kLacrosAvailability, base::Value::Type::STRING);
  g_lacros_availability_cache = DetermineLacrosAvailabilityFromPolicyValue(
      value ? value->GetString() : base::StringPiece());
}

ComponentInfo GetLacrosComponentInfo() {
  // We default to the Dev component for UNKNOWN channels.
  static const auto kChannelToComponentInfoMap =
      base::MakeFixedFlatMap<Channel, const ComponentInfo*>({
          {Channel::UNKNOWN, &kLacrosDogfoodDevInfo},
          {Channel::CANARY, &kLacrosDogfoodCanaryInfo},
          {Channel::DEV, &kLacrosDogfoodDevInfo},
          {Channel::BETA, &kLacrosDogfoodBetaInfo},
          {Channel::STABLE, &kLacrosDogfoodStableInfo},
      });
  return *kChannelToComponentInfoMap.at(GetStatefulLacrosChannel());
}

Channel GetLacrosSelectionUpdateChannel(LacrosSelection selection) {
  switch (selection) {
    case LacrosSelection::kRootfs:
      // For 'rootfs' Lacros use the same channel as ash/OS. Obtained from
      // the LSB's release track property.
      return chrome::GetChannel();
    case LacrosSelection::kStateful:
      // For 'stateful' Lacros directly check the channel of stateful-lacros
      // that the user is on.
      return GetStatefulLacrosChannel();
  }
}

LacrosAvailability GetCachedLacrosAvailabilityForTesting() {
  return GetCachedLacrosAvailability();
}

void ClearLacrosAvailabilityCacheForTest() {
  g_lacros_availability_cache.reset();
}

bool IsProfileMigrationCompletedForUser(PrefService* local_state,
                                        const std::string& user_id_hash) {
  // Allows tests to avoid marking profile migration as completed by getting
  // user_id_hash of the logged in user and updating
  // g_browser_process->local_state() etc.
  if (g_profile_migration_completed_for_test)
    return true;

  if (base::FeatureList::IsEnabled(
          ash::features::kForceProfileMigrationCompletion)) {
    return true;
  }

  const auto* pref =
      local_state->FindPreference(kProfileMigrationCompletedForUserPref);
  // Return if the pref is not registered. This can happen in browsertests. In
  // such a case, assume that migration was completed.
  if (!pref)
    return true;

  const base::Value* value = pref->GetValue();
  DCHECK(value->is_dict());
  absl::optional<bool> is_completed = value->FindBoolKey(user_id_hash);

  // If migration was skipped or failed, disable lacros.
  return is_completed.value_or(false);
}

void SetProfileMigrationCompletedForUser(PrefService* local_state,
                                         const std::string& user_id_hash) {
  DictionaryPrefUpdate update(local_state,
                              kProfileMigrationCompletedForUserPref);
  base::Value* dict = update.Get();
  dict->SetBoolKey(user_id_hash, true);
}

void ClearProfileMigrationCompletedForUser(PrefService* local_state,
                                           const std::string& user_id_hash) {
  DictionaryPrefUpdate update(local_state,
                              kProfileMigrationCompletedForUserPref);
  base::Value* dict = update.Get();
  dict->RemoveKey(user_id_hash);
}

void SetProfileMigrationCompletedForTest(bool is_completed) {
  g_profile_migration_completed_for_test = is_completed;
}

LacrosLaunchSwitchSource GetLacrosLaunchSwitchSource() {
  if (!g_lacros_availability_cache.has_value())
    return LacrosLaunchSwitchSource::kUnknown;

  // Note: this check needs to be consistent with the one in
  // DetermineLacrosAvailabilityFromPolicyValue.
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(ash::switches::kLacrosAvailabilityIgnore))
    return LacrosLaunchSwitchSource::kForcedByUser;

  return GetCachedLacrosAvailability() == LacrosAvailability::kUserChoice
             ? LacrosLaunchSwitchSource::kPossiblySetByUser
             : LacrosLaunchSwitchSource::kForcedByPolicy;
}

absl::optional<LacrosAvailability> ParseLacrosAvailability(
    base::StringPiece value) {
  auto* it = kLacrosAvailabilityMap.find(value);
  if (it != kLacrosAvailabilityMap.end())
    return it->second;

  LOG(ERROR) << "Unknown LacrosAvailability policy value is passed: " << value;
  return absl::nullopt;
}

base::StringPiece GetLacrosAvailabilityPolicyName(LacrosAvailability value) {
  for (const auto& entry : kLacrosAvailabilityMap) {
    if (entry.second == value)
      return entry.first;
  }

  NOTREACHED();
  return base::StringPiece();
}

bool IsAshBrowserSyncEnabled() {
  // Turn off sync from Ash if Lacros is enabled and Ash web browser is
  // disabled.
  // TODO(crbug.com/1293250): We must check whether profile migration is
  // completed or not here. Currently that is checked inside `IsLacrosEnabled()`
  // but it is planned to be decoupled with the function in the future.
  if (IsLacrosEnabled() && !IsAshWebBrowserEnabled())
    return false;

  return true;
}

}  // namespace browser_util
}  // namespace crosapi
