// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/password_manager/core/browser/sync/password_sync_bridge.h"

#include <unordered_set>
#include <utility>

#include "base/auto_reset.h"
#include "base/callback.h"
#include "base/check_op.h"
#include "base/containers/flat_map.h"
#include "base/feature_list.h"
#include "base/memory/raw_ptr.h"
#include "base/metrics/histogram_functions.h"
#include "base/notreached.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "components/password_manager/core/browser/insecure_credentials_table.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_manager_metrics_util.h"
#include "components/password_manager/core/browser/password_store_change.h"
#include "components/password_manager/core/browser/password_store_sync.h"
#include "components/password_manager/core/browser/sync/password_proto_utils.h"
#include "components/password_manager/core/common/password_manager_features.h"
#include "components/sync/model/in_memory_metadata_change_list.h"
#include "components/sync/model/metadata_batch.h"
#include "components/sync/model/metadata_change_list.h"
#include "components/sync/model/model_type_change_processor.h"
#include "components/sync/model/mutable_data_batch.h"
#include "components/sync/model/sync_metadata_store_change_list.h"
#include "net/base/escape.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "url/gurl.h"

namespace password_manager {

namespace {

// Error values for reading sync metadata.
// Used in metrics: "PasswordManager.SyncMetadataReadError". These values
// are persisted to logs. Entries should not be renumbered and numeric values
// should never be reused.
enum class SyncMetadataReadError {
  // Success.
  kNone = 0,
  // Database not available.
  kDbNotAvailable = 1,
  // Reading failure.
  kReadFailed = 2,
  // Reading successful but cleaning initiated in order to force initial sync.
  // This will clean undecryptable passwords.
  kReadSuccessButCleared = 3,

  kMaxValue = kReadSuccessButCleared,
};

std::string ComputeClientTag(
    const sync_pb::PasswordSpecificsData& password_data) {
  return net::EscapePath(GURL(password_data.origin()).spec()) + "|" +
         net::EscapePath(password_data.username_element()) + "|" +
         net::EscapePath(password_data.username_value()) + "|" +
         net::EscapePath(password_data.password_element()) + "|" +
         net::EscapePath(password_data.signon_realm());
}

base::Time ConvertToBaseTime(uint64_t time) {
  return base::Time::FromDeltaSinceWindowsEpoch(
      // Use FromDeltaSinceWindowsEpoch because create_time_us has
      // always used the Windows epoch.
      base::Microseconds(time));
}

PasswordForm PasswordFromEntityChange(const syncer::EntityChange& entity_change,
                                      base::Time sync_time) {
  DCHECK(entity_change.data().specifics.has_password());
  const sync_pb::PasswordSpecificsData& password_data =
      entity_change.data().specifics.password().client_only_encrypted_data();
  return PasswordFromSpecifics(password_data);
}

std::unique_ptr<syncer::EntityData> CreateEntityData(const PasswordForm& form) {
  auto entity_data = std::make_unique<syncer::EntityData>();
  *entity_data->specifics.mutable_password() = SpecificsFromPassword(form);
  entity_data->name = form.signon_realm;
  return entity_data;
}

FormPrimaryKey ParsePrimaryKey(const std::string& storage_key) {
  int primary_key = 0;
  bool success = base::StringToInt(storage_key, &primary_key);
  DCHECK(success)
      << "Invalid storage key. Failed to convert the storage key to "
         "an integer";
  return FormPrimaryKey(primary_key);
}

// Returns true iff |password_specifics| and |password_form| are equal
// memberwise. It doesn't compare |password_issues|.
bool AreLocalAndRemotePasswordsEqualExcludingIssues(
    const sync_pb::PasswordSpecificsData& password_specifics,
    const PasswordForm& password_form) {
  return (static_cast<int>(password_form.scheme) ==
              password_specifics.scheme() &&
          password_form.signon_realm == password_specifics.signon_realm() &&
          password_form.url.spec() == password_specifics.origin() &&
          password_form.action.spec() == password_specifics.action() &&
          base::UTF16ToUTF8(password_form.username_element) ==
              password_specifics.username_element() &&
          base::UTF16ToUTF8(password_form.password_element) ==
              password_specifics.password_element() &&
          base::UTF16ToUTF8(password_form.username_value) ==
              password_specifics.username_value() &&
          base::UTF16ToUTF8(password_form.password_value) ==
              password_specifics.password_value() &&
          password_form.date_last_used ==
              ConvertToBaseTime(password_specifics.date_last_used()) &&
          password_form.date_password_modified ==
              ConvertToBaseTime(
                  password_specifics
                      .date_password_modified_windows_epoch_micros()) &&
          password_form.date_created ==
              ConvertToBaseTime(password_specifics.date_created()) &&
          password_form.blocked_by_user == password_specifics.blacklisted() &&
          static_cast<int>(password_form.type) == password_specifics.type() &&
          password_form.times_used == password_specifics.times_used() &&
          base::UTF16ToUTF8(password_form.display_name) ==
              password_specifics.display_name() &&
          password_form.icon_url.spec() == password_specifics.avatar_url() &&
          url::Origin::Create(GURL(password_specifics.federation_url()))
                  .Serialize() == password_form.federation_origin.Serialize());
}

// Returns true iff |password_specifics| and |password_form| are equal
// memberwise.
bool AreLocalAndRemotePasswordsEqual(
    const sync_pb::PasswordSpecificsData& password_specifics,
    const PasswordForm& password_form) {
  return AreLocalAndRemotePasswordsEqualExcludingIssues(password_specifics,
                                                        password_form) &&
         password_form.password_issues ==
             PasswordIssuesMapFromProto(password_specifics);
}

// Whether we should try to recover undecryptable local passwords by deleting
// the local copy, to be replaced by the remote version coming from Sync during
// merge.
bool ShouldRecoverPasswordsDuringMerge() {
  // Delete the local undecryptable copy when this is MacOS only.
#if BUILDFLAG(IS_MAC)
  return true;
#elif BUILDFLAG(IS_LINUX)
  return base::FeatureList::IsEnabled(
      features::kSyncUndecryptablePasswordsLinux);
#else
  return false;
#endif
}

bool ShouldCleanSyncMetadataDuringStartupWhenDecryptionFails() {
#if BUILDFLAG(IS_LINUX)
  return ShouldRecoverPasswordsDuringMerge() &&
         base::FeatureList::IsEnabled(
             features::kForceInitialSyncWhenDecryptionFails);
#else
  return false;
#endif
}

bool DoesPasswordStoreHaveEncryptionServiceFailures(
    PasswordStoreSync* password_store_sync) {
  PrimaryKeyToFormMap key_to_form_map;
  FormRetrievalResult result =
      password_store_sync->ReadAllLogins(&key_to_form_map);
  if (result == FormRetrievalResult::kEncryptionServiceFailure ||
      result == FormRetrievalResult::kEncryptionServiceFailureWithPartialData) {
    return true;
  }
  return false;
}

// A simple class for scoping a password store sync transaction. If the
// transaction hasn't been committed, it will be rolled back when it goes out of
// scope.
class ScopedStoreTransaction {
 public:
  explicit ScopedStoreTransaction(PasswordStoreSync* store) : store_(store) {
    store_->BeginTransaction();
    committed_ = false;
  }

  void Commit() {
    if (!committed_) {
      store_->CommitTransaction();
      committed_ = true;
    }
  }

  ScopedStoreTransaction(const ScopedStoreTransaction&) = delete;
  ScopedStoreTransaction& operator=(const ScopedStoreTransaction&) = delete;

  ~ScopedStoreTransaction() {
    if (!committed_) {
      store_->RollbackTransaction();
    }
  }

 private:
  raw_ptr<PasswordStoreSync> store_;
  bool committed_;
};

}  // namespace

PasswordSyncBridge::PasswordSyncBridge(
    std::unique_ptr<syncer::ModelTypeChangeProcessor> change_processor,
    PasswordStoreSync* password_store_sync,
    const base::RepeatingClosure& sync_enabled_or_disabled_cb)
    : ModelTypeSyncBridge(std::move(change_processor)),
      password_store_sync_(password_store_sync),
      sync_enabled_or_disabled_cb_(sync_enabled_or_disabled_cb) {
  DCHECK(password_store_sync_);
  DCHECK(sync_enabled_or_disabled_cb_);
  // The metadata store could be null if the login database initialization
  // fails.
  SyncMetadataReadError sync_metadata_read_error = SyncMetadataReadError::kNone;
  std::unique_ptr<syncer::MetadataBatch> batch;
  if (!password_store_sync_->GetMetadataStore()) {
    this->change_processor()->ReportError(
        {FROM_HERE, "Password metadata store isn't available."});
    sync_metadata_read_error = SyncMetadataReadError::kDbNotAvailable;
  } else {
    batch = password_store_sync_->GetMetadataStore()->GetAllSyncMetadata();
    if (!batch) {
      // If the metadata cannot be read, it's either a persistent error or force
      // initial sync has been requested. In both cases, we drop the metadata to
      // go through the initial sync flow.
      password_store_sync_->GetMetadataStore()->DeleteAllSyncMetadata();
      batch = std::make_unique<syncer::MetadataBatch>();
      sync_metadata_read_error = SyncMetadataReadError::kReadFailed;
    } else if (ShouldCleanSyncMetadataDuringStartupWhenDecryptionFails() &&
               DoesPasswordStoreHaveEncryptionServiceFailures(
                   password_store_sync_)) {
      // Some logins in the passwords store cannot be read, force initial sync
      // by dropping the metadata.
      password_store_sync_->GetMetadataStore()->DeleteAllSyncMetadata();
      batch = std::make_unique<syncer::MetadataBatch>();
      sync_metadata_read_error = SyncMetadataReadError::kReadSuccessButCleared;
    }
  }
  base::UmaHistogramEnumeration("PasswordManager.SyncMetadataReadError",
                                sync_metadata_read_error);

  if (batch) {
    this->change_processor()->ModelReadyToSync(std::move(batch));
  }
}

PasswordSyncBridge::~PasswordSyncBridge() = default;

void PasswordSyncBridge::ActOnPasswordStoreChanges(
    const PasswordStoreChangeList& local_changes) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // It's the responsibility of the callers to call this method within the same
  // transaction as the data changes to fulfill atomic writes of data and
  // metadata constraint.

  // TODO(mamir):ActOnPasswordStoreChanges() DCHECK we are inside a
  // transaction!;

  if (!change_processor()->IsTrackingMetadata()) {
    return;  // Sync processor not yet ready, don't sync.
  }

  // ActOnPasswordStoreChanges() can be called from ApplySyncChanges(). Do
  // nothing in this case.
  if (is_processing_remote_sync_changes_) {
    return;
  }

  syncer::SyncMetadataStoreChangeList metadata_change_list(
      password_store_sync_->GetMetadataStore(), syncer::PASSWORDS);

  for (const PasswordStoreChange& change : local_changes) {
    const std::string storage_key =
        base::NumberToString(change.primary_key().value());
    switch (change.type()) {
      case PasswordStoreChange::ADD:
      case PasswordStoreChange::UPDATE: {
        change_processor()->Put(storage_key, CreateEntityData(change.form()),
                                &metadata_change_list);
        break;
      }
      case PasswordStoreChange::REMOVE: {
        change_processor()->Delete(storage_key, &metadata_change_list);
        break;
      }
    }
  }
}

std::unique_ptr<syncer::MetadataChangeList>
PasswordSyncBridge::CreateMetadataChangeList() {
  return std::make_unique<syncer::InMemoryMetadataChangeList>();
}

absl::optional<syncer::ModelError> PasswordSyncBridge::MergeSyncData(
    std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
    syncer::EntityChangeList entity_data) {
  // This method merges the local and remote passwords based on their client
  // tags. For a form |F|, there are three cases to handle:
  // 1. |F| exists only in the local model --> |F| should be Put() in the change
  //    processor.
  // 2. |F| exists only in the remote model --> |F| should be AddLoginSync() to
  //    the local password store.
  // 3. |F| exists in both the local and the remote models --> both versions
  //    should be merged by accepting the most recently created one, and update
  //    local and remote models accordingly.
  base::AutoReset<bool> processing_changes(&is_processing_remote_sync_changes_,
                                           true);

  // Read all local passwords.
  PrimaryKeyToFormMap key_to_local_form_map;
  FormRetrievalResult read_result =
      password_store_sync_->ReadAllLogins(&key_to_local_form_map);

  if (read_result == FormRetrievalResult::kDbError) {
    metrics_util::LogPasswordSyncState(metrics_util::NOT_SYNCING_FAILED_READ);
    return syncer::ModelError(FROM_HERE,
                              "Failed to load entries from password store.");
  }
  if (read_result == FormRetrievalResult::kEncryptionServiceFailure ||
      read_result ==
          FormRetrievalResult::kEncryptionServiceFailureWithPartialData) {
    if (!ShouldRecoverPasswordsDuringMerge()) {
      metrics_util::LogPasswordSyncState(
          metrics_util::NOT_SYNCING_FAILED_DECRYPTION);
      return syncer::ModelError(FROM_HERE,
                                "Failed to load entries from password store. "
                                "Encryption service failure.");
    }
    absl::optional<syncer::ModelError> cleanup_result_error =
        CleanupPasswordStore();
    if (cleanup_result_error) {
      metrics_util::LogPasswordSyncState(
          metrics_util::NOT_SYNCING_FAILED_CLEANUP);
      return cleanup_result_error;
    }
    // Clean up done successfully, try to read again.
    read_result = password_store_sync_->ReadAllLogins(&key_to_local_form_map);
    if (read_result != FormRetrievalResult::kSuccess) {
      metrics_util::LogPasswordSyncState(metrics_util::NOT_SYNCING_FAILED_READ);
      return syncer::ModelError(
          FROM_HERE,
          "Failed to load entries from password store after cleanup.");
    }
  }
  DCHECK_EQ(read_result, FormRetrievalResult::kSuccess);

  // Collect the client tags of remote passwords and the corresponding
  // EntityChange. Note that |entity_data| only contains client tag *hashes*.
  std::map<std::string, const syncer::EntityChange*>
      client_tag_to_remote_entity_change_map;
  for (const std::unique_ptr<syncer::EntityChange>& entity_change :
       entity_data) {
    client_tag_to_remote_entity_change_map[GetClientTag(
        entity_change->data())] = entity_change.get();
  }

  // This is used to keep track of all the changes applied to the password
  // store to notify other observers of the password store.
  PasswordStoreChangeList password_store_changes;
  {
    ScopedStoreTransaction transaction(password_store_sync_);
    const base::Time time_now = base::Time::Now();
    // For any local password that doesn't exist in the remote passwords, issue
    // a change_processor()->Put(). For any local password that exists in the
    // remote passwords, both should be merged by picking the most recently
    // created version. Password comparison is done by comparing the client
    // tags. In addition, collect the client tags of local passwords.
    std::unordered_set<std::string> client_tags_of_local_passwords;
    for (const auto& [primary_key, local_password_form] :
         key_to_local_form_map) {
      std::unique_ptr<syncer::EntityData> local_form_entity_data =
          CreateEntityData(*local_password_form);
      const std::string client_tag_of_local_password =
          GetClientTag(*local_form_entity_data);
      client_tags_of_local_passwords.insert(client_tag_of_local_password);

      if (client_tag_to_remote_entity_change_map.count(
              client_tag_of_local_password) == 0) {
        // Local password doesn't exist in the remote model, Put() it in the
        // processor.
        change_processor()->Put(
            /*storage_key=*/base::NumberToString(primary_key.value()),
            std::move(local_form_entity_data), metadata_change_list.get());
        continue;
      }

      // Local password exists in the remote model as well. A merge is required.
      const syncer::EntityChange& remote_entity_change =
          *client_tag_to_remote_entity_change_map[client_tag_of_local_password];
      const sync_pb::PasswordSpecificsData& remote_password_specifics =
          remote_entity_change.data()
              .specifics.password()
              .client_only_encrypted_data();

      // First, we need to inform the processor about the storage key anyway.
      change_processor()->UpdateStorageKey(
          remote_entity_change.data(),
          /*storage_key=*/
          base::NumberToString(primary_key.value()),
          metadata_change_list.get());

      if (AreLocalAndRemotePasswordsEqual(remote_password_specifics,
                                          *local_password_form)) {
        // Passwords are identical, nothing else to do.
        continue;
      }

      // Passwords or insecure credentials aren't identical.
      if (ConvertToBaseTime(remote_password_specifics.date_created()) <
              local_password_form->date_created ||
          (AreLocalAndRemotePasswordsEqualExcludingIssues(
               remote_password_specifics, *local_password_form) &&
           local_password_form->IsInsecureCredential(InsecureType::kPhished))) {
        // Either the local password is more recent, or they are equal but the
        // local password has been marked as phished. While all other types of
        // issues are easy to recompute (e.g. via Password Check) phished
        // entries are only found locally, so persisting them is important.
        change_processor()->Put(
            /*storage_key=*/base::NumberToString(primary_key.value()),
            std::move(local_form_entity_data), metadata_change_list.get());
      } else {
        // The remote password is more recent, update the local model.
        UpdateLoginError update_login_error;
        const PasswordForm form =
            PasswordFromEntityChange(remote_entity_change,
                                     /*sync_time=*/time_now);
        PasswordStoreChangeList changes =
            password_store_sync_->UpdateLoginSync(form, &update_login_error);
        DCHECK_LE(changes.size(), 1U);
        base::UmaHistogramEnumeration(
            "PasswordManager.MergeSyncData.UpdateLoginSyncError",
            update_login_error);
        if (changes.empty()) {
          metrics_util::LogPasswordSyncState(
              metrics_util::NOT_SYNCING_FAILED_UPDATE);
          return syncer::ModelError(
              FROM_HERE, "Failed to update an entry in the password store.");
        }
        DCHECK(changes[0].primary_key() == primary_key);
        password_store_changes.push_back(changes[0]);
      }
    }

    // At this point, we have processed all local passwords. In addition, we
    // also have processed all remote passwords that exist in the local model.
    // What's remaining is to process remote passwords that don't exist in the
    // local model.

    // For any remote password that doesn't exist in the local passwords, issue
    // a password_store_sync_->AddLoginSync() and invoke the
    // change_processor()->UpdateStorageKey(). Password comparison is done by
    // comparing the client tags.
    for (const std::unique_ptr<syncer::EntityChange>& entity_change :
         entity_data) {
      const std::string client_tag_of_remote_password =
          GetClientTag(entity_change->data());
      if (client_tags_of_local_passwords.count(client_tag_of_remote_password) !=
          0) {
        // Passwords in both local and remote models have been processed
        // already.
        continue;
      }

      AddLoginError add_login_error;
      PasswordStoreChangeList changes = password_store_sync_->AddLoginSync(
          PasswordFromEntityChange(*entity_change, /*sync_time=*/time_now),
          &add_login_error);
      base::UmaHistogramEnumeration(
          "PasswordManager.MergeSyncData.AddLoginSyncError", add_login_error);

      // TODO(crbug.com/939302): It's not yet clear if the DCHECK_LE below is
      // legit. However, recent crashes suggest that 2 changes are returned
      // when trying to AddLoginSync (details are in the bug). Once this is
      // resolved, we should update the call the UpdateStorageKey() if
      // necessary and remove unnecessary DCHECKs below.
      // DCHECK_LE(changes.size(), 1U);
      DCHECK_LE(changes.size(), 2U);
      if (changes.empty()) {
        DCHECK_NE(add_login_error, AddLoginError::kNone);
        metrics_util::LogPasswordSyncState(
            metrics_util::NOT_SYNCING_FAILED_ADD);
        // If the remote update is invalid, direct the processor to ignore and
        // move on.
        if (add_login_error == AddLoginError::kConstraintViolation) {
          change_processor()->UntrackEntityForClientTagHash(
              entity_change->data().client_tag_hash);
          continue;
        }
        // For all other types of error, we should stop syncing.
        return syncer::ModelError(
            FROM_HERE, "Failed to add an entry in the password store.");
      }

      if (changes.size() == 1) {
        DCHECK_EQ(changes[0].type(), PasswordStoreChange::ADD);
      } else {
        // There must be 2 changes.
        DCHECK_EQ(changes[0].type(), PasswordStoreChange::REMOVE);
        DCHECK_EQ(changes[1].type(), PasswordStoreChange::ADD);
      }

      change_processor()->UpdateStorageKey(
          entity_change->data(),
          /*storage_key=*/
          base::NumberToString(changes.back().primary_key().value()),
          metadata_change_list.get());

      password_store_changes.insert(password_store_changes.end(),
                                    changes.begin(), changes.end());
    }

    // Persist the metadata changes.
    // TODO(mamir): add some test coverage for the metadata persistence.
    syncer::SyncMetadataStoreChangeList sync_metadata_store_change_list(
        password_store_sync_->GetMetadataStore(), syncer::PASSWORDS);
    // |metadata_change_list| must have been created via
    // CreateMetadataChangeList() so downcasting is safe.
    static_cast<syncer::InMemoryMetadataChangeList*>(metadata_change_list.get())
        ->TransferChangesTo(&sync_metadata_store_change_list);
    absl::optional<syncer::ModelError> error =
        sync_metadata_store_change_list.TakeError();
    if (error) {
      metrics_util::LogPasswordSyncState(
          metrics_util::NOT_SYNCING_FAILED_METADATA_PERSISTENCE);
      return error;
    }
    transaction.Commit();
  }  // End of scoped transaction.

  if (!password_store_changes.empty()) {
    // It could be the case that there are no remote passwords. In such case,
    // there would be no changes to the password store other than the sync
    // metadata changes, and no need to notify observers since they aren't
    // interested in changes to sync metadata.
    password_store_sync_->NotifyLoginsChanged(password_store_changes);
  }

  metrics_util::LogPasswordSyncState(metrics_util::SYNCING_OK);
  if (password_store_sync_->IsAccountStore()) {
    int password_count = base::ranges::count_if(
        entity_data,
        [](const std::unique_ptr<syncer::EntityChange>& entity_change) {
          return !entity_change->data()
                      .specifics.password()
                      .client_only_encrypted_data()
                      .blacklisted();
        });
    metrics_util::LogDownloadedPasswordsCountFromAccountStoreAfterUnlock(
        password_count);
    metrics_util::
        LogDownloadedBlocklistedEntriesCountFromAccountStoreAfterUnlock(
            entity_data.size() - password_count);
  }

  sync_enabled_or_disabled_cb_.Run();
  return absl::nullopt;
}

absl::optional<syncer::ModelError> PasswordSyncBridge::ApplySyncChanges(
    std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
    syncer::EntityChangeList entity_changes) {
  base::AutoReset<bool> processing_changes(&is_processing_remote_sync_changes_,
                                           true);

  const base::Time time_now = base::Time::Now();

  // This is used to keep track of all the changes applied to the password store
  // to notify other observers of the password store.
  PasswordStoreChangeList password_store_changes;
  // Whether local state of insecure credentials changed.
  {
    ScopedStoreTransaction transaction(password_store_sync_);

    for (const std::unique_ptr<syncer::EntityChange>& entity_change :
         entity_changes) {
      PasswordStoreChangeList changes;
      switch (entity_change->type()) {
        case syncer::EntityChange::ACTION_ADD:
          AddLoginError add_login_error;
          changes = password_store_sync_->AddLoginSync(
              PasswordFromEntityChange(*entity_change, /*sync_time=*/time_now),
              &add_login_error);
          base::UmaHistogramEnumeration(
              "PasswordManager.ApplySyncChanges.AddLoginSyncError",
              add_login_error);
          // If the addition has been successful, inform the processor about the
          // assigned storage key. AddLoginSync() might return multiple changes
          // and the last one should be the one representing the actual addition
          // in the DB.
          if (changes.empty()) {
            DCHECK_NE(add_login_error, AddLoginError::kNone);
            metrics_util::LogApplySyncChangesState(
                metrics_util::ApplySyncChangesState::kApplyAddFailed);
            // If the remote update is invalid, direct the processor to ignore
            // and move on.
            if (add_login_error == AddLoginError::kConstraintViolation) {
              change_processor()->UntrackEntityForClientTagHash(
                  entity_change->data().client_tag_hash);
              continue;
            }
            // For all other types of error, we should stop syncing.
            return syncer::ModelError(
                FROM_HERE, "Failed to add an entry to the password store.");
          }
          // TODO(crbug.com/939302): It's not yet clear if the DCHECK_LE below
          // is legit. However, recent crashes suggest that 2 changes are
          // returned when trying to AddLoginSync (details are in the bug). Once
          // this is resolved, we should update the call the UpdateStorageKey()
          // if necessary and remove unnecessary DCHECKs below.
          // DCHECK_EQ(1U, changes.size());
          DCHECK_LE(changes.size(), 2U);
          if (changes.size() == 1) {
            DCHECK_EQ(changes[0].type(), PasswordStoreChange::ADD);
          } else {
            // There must be 2 changes.
            DCHECK_EQ(changes[0].type(), PasswordStoreChange::REMOVE);
            DCHECK_EQ(changes[1].type(), PasswordStoreChange::ADD);
          }

          change_processor()->UpdateStorageKey(
              entity_change->data(),
              /*storage_key=*/
              base::NumberToString(changes.back().primary_key().value()),
              metadata_change_list.get());
          break;
        case syncer::EntityChange::ACTION_UPDATE: {
          // TODO(mamir): This had been added to mitigate some potential issues
          // in the login database. Once the underlying cause is verified, we
          // should remove this check.
          if (entity_change->storage_key().empty()) {
            continue;
          }
          UpdateLoginError update_login_error;
          PasswordForm form =
              PasswordFromEntityChange(*entity_change, /*sync_time=*/time_now);
          changes =
              password_store_sync_->UpdateLoginSync(form, &update_login_error);
          FormPrimaryKey primary_key =
              FormPrimaryKey(ParsePrimaryKey(entity_change->storage_key()));
          base::UmaHistogramEnumeration(
              "PasswordManager.ApplySyncChanges.UpdateLoginSyncError",
              update_login_error);
          // If there are no entries to update, direct the processor to ignore
          // and move on.
          if (update_login_error == UpdateLoginError::kNoUpdatedRecords) {
            change_processor()->UntrackEntityForClientTagHash(
                entity_change->data().client_tag_hash);
            continue;
          }
          if (changes.empty()) {
            metrics_util::LogApplySyncChangesState(
                metrics_util::ApplySyncChangesState::kApplyUpdateFailed);
            return syncer::ModelError(
                FROM_HERE, "Failed to update an entry in the password store.");
          }
          DCHECK_EQ(1U, changes.size());
          DCHECK(changes[0].primary_key() == primary_key);
          break;
        }
        case syncer::EntityChange::ACTION_DELETE: {
          // TODO(mamir): This had been added to mitigate some potential issues
          // in the login database. Once the underlying cause is verified, we
          // should remove this check.
          if (entity_change->storage_key().empty()) {
            continue;
          }
          FormPrimaryKey primary_key =
              ParsePrimaryKey(entity_change->storage_key());
          changes =
              password_store_sync_->RemoveLoginByPrimaryKeySync(primary_key);
          if (changes.empty()) {
            metrics_util::LogApplySyncChangesState(
                metrics_util::ApplySyncChangesState::kApplyDeleteFailed);
            // TODO(mamir): Revisit this after checking UMA to decide if this
            // relaxation is crucial or not.
            continue;
          }
          DCHECK_EQ(1U, changes.size());
          DCHECK_EQ(changes[0].primary_key(), primary_key);
          break;
        }
      }
      password_store_changes.insert(password_store_changes.end(),
                                    changes.begin(), changes.end());
    }

    // Persist the metadata changes.
    // TODO(mamir): add some test coverage for the metadata persistence.
    syncer::SyncMetadataStoreChangeList sync_metadata_store_change_list(
        password_store_sync_->GetMetadataStore(), syncer::PASSWORDS);
    // |metadata_change_list| must have been created via
    // CreateMetadataChangeList() so downcasting is safe.
    static_cast<syncer::InMemoryMetadataChangeList*>(metadata_change_list.get())
        ->TransferChangesTo(&sync_metadata_store_change_list);
    absl::optional<syncer::ModelError> error =
        sync_metadata_store_change_list.TakeError();
    if (error) {
      metrics_util::LogApplySyncChangesState(
          metrics_util::ApplySyncChangesState::kApplyMetadataChangesFailed);
      return error;
    }
    transaction.Commit();
  }  // End of scoped transaction.

  if (!password_store_changes.empty()) {
    // It could be the case that there are no password store changes, and all
    // changes are only metadata changes. In such case, no need to notify
    // observers since they aren't interested in changes to sync metadata.
    password_store_sync_->NotifyLoginsChanged(password_store_changes);
  }
  metrics_util::LogApplySyncChangesState(
      metrics_util::ApplySyncChangesState::kApplyOK);
  return absl::nullopt;
}

void PasswordSyncBridge::GetData(StorageKeyList storage_keys,
                                 DataCallback callback) {
  // This method is called only when there are uncommitted changes on startup.
  // There are more efficient implementations, but since this method is rarely
  // called, simplicity is preferred over efficiency.
  PrimaryKeyToFormMap key_to_form_map;
  if (password_store_sync_->ReadAllLogins(&key_to_form_map) !=
      FormRetrievalResult::kSuccess) {
    change_processor()->ReportError(
        {FROM_HERE, "Failed to load entries from the password store."});
    return;
  }

  auto batch = std::make_unique<syncer::MutableDataBatch>();
  for (const std::string& storage_key : storage_keys) {
    FormPrimaryKey primary_key = ParsePrimaryKey(storage_key);
    if (key_to_form_map.count(primary_key) != 0) {
      batch->Put(storage_key, CreateEntityData(*key_to_form_map[primary_key]));
    }
  }
  std::move(callback).Run(std::move(batch));
}

void PasswordSyncBridge::GetAllDataForDebugging(DataCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  PrimaryKeyToFormMap key_to_form_map;
  if (password_store_sync_->ReadAllLogins(&key_to_form_map) !=
      FormRetrievalResult::kSuccess) {
    change_processor()->ReportError(
        {FROM_HERE, "Failed to load entries from the password store."});
    return;
  }

  auto batch = std::make_unique<syncer::MutableDataBatch>();
  for (const auto& [primary_key, form] : key_to_form_map) {
    form->password_value = u"<redacted>";
    batch->Put(base::NumberToString(primary_key.value()),
               CreateEntityData(*form));
  }
  std::move(callback).Run(std::move(batch));
}

std::string PasswordSyncBridge::GetClientTag(
    const syncer::EntityData& entity_data) {
  DCHECK(entity_data.specifics.has_password())
      << "EntityData does not have password specifics.";

  return ComputeClientTag(
      entity_data.specifics.password().client_only_encrypted_data());
}

std::string PasswordSyncBridge::GetStorageKey(
    const syncer::EntityData& entity_data) {
  NOTREACHED() << "PasswordSyncBridge does not support GetStorageKey.";
  return std::string();
}

bool PasswordSyncBridge::SupportsGetStorageKey() const {
  return false;
}

void PasswordSyncBridge::ApplyStopSyncChanges(
    std::unique_ptr<syncer::MetadataChangeList> delete_metadata_change_list) {
  if (!delete_metadata_change_list) {
    return;
  }
  if (!password_store_sync_->IsAccountStore()) {
    password_store_sync_->GetMetadataStore()->DeleteAllSyncMetadata();
    sync_enabled_or_disabled_cb_.Run();
    return;
  }
  // For the account store, the data should be deleted too. So do the following:
  // 1. Collect the credentials that will be deleted.
  // 2. Collect which credentials out of those to be deleted are unsynced.
  // 3. Delete the metadata and the data.
  // 4. Notify the store about deleted credentials, to notify store observers.
  // 5. Notify the store about deleted unsynced credentials, to take care of
  //    notifying the UI and offering the user to save those credentials in the
  //    profile store.
  base::AutoReset<bool> processing_changes(&is_processing_remote_sync_changes_,
                                           true);

  PasswordStoreChangeList password_store_changes;
  std::vector<PasswordForm> unsynced_logins_being_deleted;
  PrimaryKeyToFormMap logins;
  FormRetrievalResult result = password_store_sync_->ReadAllLogins(&logins);
  if (result == FormRetrievalResult::kSuccess) {
    std::set<FormPrimaryKey> unsynced_passwords_storage_keys =
        GetUnsyncedPasswordsStorageKeys();
    for (const auto& [primary_key, form] : logins) {
      password_store_changes.emplace_back(PasswordStoreChange::REMOVE, *form,
                                          primary_key);
      if (unsynced_passwords_storage_keys.count(primary_key) != 0 &&
          !form->blocked_by_user) {
        unsynced_logins_being_deleted.push_back(*form);
      }
    }
  }
  password_store_sync_->GetMetadataStore()->DeleteAllSyncMetadata();
  password_store_sync_->DeleteAndRecreateDatabaseFile();
  password_store_sync_->NotifyLoginsChanged(password_store_changes);

  base::UmaHistogramCounts100(
      "PasswordManager.AccountStorage.UnsyncedPasswordsFoundDuringSignOut",
      unsynced_logins_being_deleted.size());

  if (!unsynced_logins_being_deleted.empty()) {
    password_store_sync_->NotifyUnsyncedCredentialsWillBeDeleted(
        std::move(unsynced_logins_being_deleted));
  }

  sync_enabled_or_disabled_cb_.Run();
}

sync_pb::EntitySpecifics PasswordSyncBridge::TrimRemoteSpecificsForCaching(
    const sync_pb::EntitySpecifics& entity_specifics) {
  DCHECK(entity_specifics.has_password());
  sync_pb::EntitySpecifics trimmed_entity_specifics;
  *trimmed_entity_specifics.mutable_password()
       ->mutable_client_only_encrypted_data() =
      TrimPasswordSpecificsDataForCaching(
          entity_specifics.password().client_only_encrypted_data());
  return trimmed_entity_specifics;
}

std::set<FormPrimaryKey> PasswordSyncBridge::GetUnsyncedPasswordsStorageKeys() {
  std::set<FormPrimaryKey> storage_keys;
  DCHECK(password_store_sync_);
  PasswordStoreSync::MetadataStore* metadata_store =
      password_store_sync_->GetMetadataStore();
  // The metadata store could be null if the login database initialization
  // fails.
  if (!metadata_store) {
    return storage_keys;
  }
  std::unique_ptr<syncer::MetadataBatch> batch =
      metadata_store->GetAllSyncMetadata();
  for (const auto& [storage_key, metadata] : batch->GetAllMetadata()) {
    // Ignore unsynced deletions.
    if (!metadata->is_deleted() &&
        change_processor()->IsEntityUnsynced(storage_key)) {
      storage_keys.insert(ParsePrimaryKey(storage_key));
    }
  }
  return storage_keys;
}

// static
std::string PasswordSyncBridge::ComputeClientTagForTesting(
    const sync_pb::PasswordSpecificsData& password_data) {
  return ComputeClientTag(password_data);
}

absl::optional<syncer::ModelError> PasswordSyncBridge::CleanupPasswordStore() {
  DatabaseCleanupResult cleanup_result =
      password_store_sync_->DeleteUndecryptableLogins();
  switch (cleanup_result) {
    case DatabaseCleanupResult::kSuccess:
      break;
    case DatabaseCleanupResult::kEncryptionUnavailable:
      metrics_util::LogPasswordSyncState(
          metrics_util::NOT_SYNCING_FAILED_DECRYPTION);
      return syncer::ModelError(
          FROM_HERE, "Failed to get encryption key during database cleanup.");
    case DatabaseCleanupResult::kItemFailure:
    case DatabaseCleanupResult::kDatabaseUnavailable:
      metrics_util::LogPasswordSyncState(
          metrics_util::NOT_SYNCING_FAILED_CLEANUP);
      return syncer::ModelError(FROM_HERE, "Failed to cleanup database.");
  }
  return absl::nullopt;
}

}  // namespace password_manager
