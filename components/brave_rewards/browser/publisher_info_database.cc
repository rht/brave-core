/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_rewards/browser/publisher_info_database.h"

#include <stdint.h>

#include <string>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "bat/ledger/media_publisher_info.h"
#include "build/build_config.h"
#include "sql/meta_table.h"
#include "sql/statement.h"
#include "sql/transaction.h"
#include "content_site.h"

namespace brave_rewards {

namespace {

const int kCurrentVersionNumber = 3;
const int kCompatibleVersionNumber = 1;

}  // namespace

PublisherInfoDatabase::PublisherInfoDatabase(const base::FilePath& db_path) :
    db_path_(db_path),
    initialized_(false) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

PublisherInfoDatabase::~PublisherInfoDatabase() {
}

bool PublisherInfoDatabase::Init() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (initialized_) {
    return true;
  }

  if (!db_.Open(db_path_)) {
    return false;
  }

  // TODO - add error delegate
  sql::Transaction committer(&db_);
  if (!committer.Begin()) {
    return false;
  }

  if (!meta_table_.Init(&db_, GetCurrentVersion(), kCompatibleVersionNumber)) {
    return false;
  }

  if (!CreatePublisherInfoTable() ||
      !CreateContributionInfoTable() ||
      !CreateActivityInfoTable() ||
      !CreateMediaPublisherInfoTable() ||
      !CreateRecurringDonationTable()) {
    return false;
  }

  CreateContributionInfoIndex();
  CreateActivityInfoIndex();
  CreateRecurringDonationIndex();

  // Version check.
  sql::InitStatus version_status = EnsureCurrentVersion();
  if (version_status != sql::INIT_OK) {
    return version_status;
  }

  if (!committer.Commit()) {
    return false;
  }

  memory_pressure_listener_.reset(new base::MemoryPressureListener(
      base::Bind(&PublisherInfoDatabase::OnMemoryPressure,
      base::Unretained(this))));

  initialized_ = true;
  return initialized_;
}

/**
 *
 * CONTRIBUTION INFO
 *
 */

bool PublisherInfoDatabase::CreateContributionInfoTable() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  const char* name = "contribution_info";
  if (GetDB().DoesTableExist(name)) {
    return true;
  }

  std::string sql;
  sql.append("CREATE TABLE ");
  sql.append(name);
  sql.append(
      "("
      "publisher_id LONGVARCHAR,"
      "probi TEXT \"0\"  NOT NULL,"
      "date INTEGER NOT NULL,"
      "category INTEGER NOT NULL,"
      "month INTEGER NOT NULL,"
      "year INTEGER NOT NULL,"
      "CONSTRAINT fk_contribution_info_publisher_id"
      "    FOREIGN KEY (publisher_id)"
      "    REFERENCES publisher_info (publisher_id)"
      "    ON DELETE CASCADE)");

  return GetDB().Execute(sql.c_str());
}

bool PublisherInfoDatabase::CreateContributionInfoIndex() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return GetDB().Execute(
      "CREATE INDEX IF NOT EXISTS contribution_info_publisher_id_index "
      "ON contribution_info (publisher_id)");
}

bool PublisherInfoDatabase::InsertContributionInfo(
    const brave_rewards::ContributionInfo& info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool initialized = Init();
  DCHECK(initialized);

  if (!initialized) {
    return false;
  }

  sql::Statement statement(GetDB().GetCachedStatement(SQL_FROM_HERE,
      "INSERT INTO contribution_info "
      "(publisher_id, probi, date, "
      "category, month, year) "
      "VALUES (?, ?, ?, ?, ?, ?)"));

  statement.BindString(0, info.publisher_key);
  statement.BindString(1, info.probi);
  statement.BindInt64(2, info.date);
  statement.BindInt(3, info.category);
  statement.BindInt(4, info.month);
  statement.BindInt(5, info.year);

  return statement.Run();
}

void PublisherInfoDatabase::GetTips(ledger::PublisherInfoList* list,
                                    ledger::PUBLISHER_MONTH month,
                                    int year) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool initialized = Init();
  DCHECK(initialized);

  if (!initialized) {
    return;
  }

  sql::Statement info_sql(db_.GetUniqueStatement(
      "SELECT pi.publisher_id, pi.name, pi.url, pi.favIcon, "
      "ci.probi, ci.date, pi.verified, pi.provider "
      "FROM contribution_info as ci "
      "INNER JOIN publisher_info AS pi ON ci.publisher_id = pi.publisher_id "
      "AND ci.month = ? AND ci.year = ? "
      "AND (ci.category = ? OR ci.category = ?)"));

  info_sql.BindInt(0, month);
  info_sql.BindInt(1, year);
  info_sql.BindInt(2, ledger::PUBLISHER_CATEGORY::DIRECT_DONATION);
  info_sql.BindInt(3, ledger::PUBLISHER_CATEGORY::TIPPING);

  while (info_sql.Step()) {
    std::string id(info_sql.ColumnString(0));

    ledger::PublisherInfo publisher(id, ledger::PUBLISHER_MONTH::ANY, -1);

    publisher.name = info_sql.ColumnString(1);
    publisher.url = info_sql.ColumnString(2);
    publisher.favicon_url = info_sql.ColumnString(3);
    publisher.weight = info_sql.ColumnDouble(4);
    publisher.reconcile_stamp = info_sql.ColumnInt64(5);
    publisher.verified = info_sql.ColumnBool(6);
    publisher.provider = info_sql.ColumnString(7);

    list->push_back(publisher);
  }
}

/**
 *
 * PUBLISHER INFO
 *
 */

bool PublisherInfoDatabase::CreatePublisherInfoTable() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  const char* name = "publisher_info";
  if (GetDB().DoesTableExist(name)) {
    return true;
  }

  std::string sql;
  sql.append("CREATE TABLE ");
  sql.append(name);
  sql.append(
      "("
      "publisher_id LONGVARCHAR PRIMARY KEY NOT NULL UNIQUE,"
      "verified BOOLEAN DEFAULT 0 NOT NULL,"
      "excluded INTEGER DEFAULT 0 NOT NULL,"
      "name TEXT NOT NULL,"
      "favIcon TEXT NOT NULL,"
      "url TEXT NOT NULL,"
      "provider TEXT NOT NULL)");

  return GetDB().Execute(sql.c_str());
}

bool PublisherInfoDatabase::InsertOrUpdatePublisherInfo(
    const ledger::PublisherInfo& info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool initialized = Init();
  DCHECK(initialized);

  if (!initialized)
    return false;

  sql::Statement publisher_info_statement(
      GetDB().GetCachedStatement(SQL_FROM_HERE,
                                 "INSERT OR REPLACE INTO publisher_info "
                                 "(publisher_id, verified, excluded, "
                                 "name, url, provider, favIcon) "
                                 "VALUES (?, ?, ?, ?, ?, ?, ?)"));

  publisher_info_statement.BindString(0, info.id);
  publisher_info_statement.BindBool(1, info.verified);
  publisher_info_statement.BindInt(2, static_cast<int>(info.excluded));
  publisher_info_statement.BindString(3, info.name);
  publisher_info_statement.BindString(4, info.url);
  publisher_info_statement.BindString(5, info.provider);
  publisher_info_statement.BindString(6, info.favicon_url);

  return publisher_info_statement.Run();
}

/**
 *
 * ACTIVITY INFO
 *
 */
bool PublisherInfoDatabase::CreateActivityInfoTable() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  const char* name = "activity_info";
  if (GetDB().DoesTableExist(name)) {
    return true;
  }

  std::string sql;
  sql.append("CREATE TABLE ");
  sql.append(name);
  sql.append(
      "("
      "publisher_id LONGVARCHAR NOT NULL,"
      "duration INTEGER DEFAULT 0 NOT NULL,"
      "visits INTEGER DEFAULT 0 NOT NULL,"
      "score DOUBLE DEFAULT 0 NOT NULL,"
      "percent INTEGER DEFAULT 0 NOT NULL,"
      "weight DOUBLE DEFAULT 0 NOT NULL,"
      "month INTEGER NOT NULL,"
      "year INTEGER NOT NULL,"
      "reconcile_stamp INTEGER DEFAULT 0 NOT NULL,"
      "CONSTRAINT activity_unique "
      "UNIQUE (publisher_id, month, year, reconcile_stamp) "
      "CONSTRAINT fk_activity_info_publisher_id"
      "    FOREIGN KEY (publisher_id)"
      "    REFERENCES publisher_info (publisher_id)"
      "    ON DELETE CASCADE)");

  return GetDB().Execute(sql.c_str());
}

bool PublisherInfoDatabase::CreateActivityInfoIndex() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return GetDB().Execute(
      "CREATE INDEX IF NOT EXISTS activity_info_publisher_id_index "
      "ON activity_info (publisher_id)");
}

bool PublisherInfoDatabase::InsertOrUpdateActivityInfo(
    const ledger::PublisherInfo& info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool initialized = Init();
  DCHECK(initialized);

  if (!initialized)
    return false;

  // Insert publisher if it doesn't exist
  sql::Statement publisher_info_statement(
      GetDB().GetCachedStatement(SQL_FROM_HERE,
          "INSERT OR IGNORE INTO publisher_info "
          "(publisher_id, verified, excluded, "
          "name, url, provider, favIcon) "
          "VALUES (?, ?, ?, ?, ?, ?, ?)"));

  publisher_info_statement.BindString(0, info.id);
  publisher_info_statement.BindBool(1, info.verified);
  publisher_info_statement.BindInt(2, static_cast<int>(info.excluded));
  publisher_info_statement.BindString(3, info.name);
  publisher_info_statement.BindString(4, info.url);
  publisher_info_statement.BindString(5, info.provider);
  publisher_info_statement.BindString(6, info.favicon_url);

  if (!publisher_info_statement.Run()) {
    return false;
  }

  sql::Statement activity_info_insert(
    GetDB().GetCachedStatement(SQL_FROM_HERE,
        "INSERT OR REPLACE INTO activity_info "
        "(publisher_id, duration, score, percent, "
        "weight, month, year, reconcile_stamp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));

  activity_info_insert.BindString(0, info.id);
  activity_info_insert.BindInt64(1, static_cast<int>(info.duration));
  activity_info_insert.BindDouble(2, info.score);
  activity_info_insert.BindInt64(3, static_cast<int>(info.percent));
  activity_info_insert.BindDouble(4, info.weight);
  activity_info_insert.BindInt(5, info.month);
  activity_info_insert.BindInt(6, info.year);
  activity_info_insert.BindInt64(7, info.reconcile_stamp);

  return activity_info_insert.Run();
}

bool PublisherInfoDatabase::GetPublisherActivityList(
    int start,
    int limit,
    const ledger::PublisherInfoFilter& filter,
    ledger::PublisherInfoList* list) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  CHECK(list);

  bool initialized = Init();
  DCHECK(initialized);

  if (!initialized) {
    return false;
  }

  std::string query = "SELECT ai.publisher_id, ai.duration, ai.score, "
                      "ai.percent, ai.weight, pi.verified, pi.excluded, "
                      "ai.month, ai.year, pi.name, pi.url, pi.provider, "
                      "pi.favIcon, ai.reconcile_stamp "
                      "FROM activity_info AS ai "
                      "INNER JOIN publisher_info AS pi "
                      "ON ai.publisher_id = pi.publisher_id "
                      "WHERE 1 = 1";

  if (!filter.id.empty()) {
    query += " AND ai.publisher_id = ?";
  }

  if (filter.month != ledger::PUBLISHER_MONTH::ANY) {
    query += " AND ai.month = ?";
  }

  if (filter.year > 0) {
    query += " AND ai.year = ?";
  }

  if (filter.reconcile_stamp > 0) {
    query += " AND ai.reconcile_stamp = ?";
  }

  if (filter.min_duration > 0) {
    query += " AND ai.duration >= ?";
  }

  if (filter.excluded != ledger::PUBLISHER_EXCLUDE_FILTER::FILTER_ALL &&
      filter.excluded !=
        ledger::PUBLISHER_EXCLUDE_FILTER::FILTER_ALL_EXCEPT_EXCLUDED) {
    query += " AND pi.excluded = ?";
  }

  if (filter.excluded ==
    ledger::PUBLISHER_EXCLUDE_FILTER::FILTER_ALL_EXCEPT_EXCLUDED) {
    query += " AND pi.excluded != ?";
  }

  for (const auto& it : filter.order_by) {
    query += " ORDER BY " + it.first;
    query += (it.second ? " ASC" : " DESC");
  }

  if (limit > 0) {
    query += " LIMIT " + std::to_string(limit);

    if (start > 1) {
      query += " OFFSET " + std::to_string(start);
    }
  }

  sql::Statement info_sql(db_.GetUniqueStatement(query.c_str()));

  int column = 0;
  if (!filter.id.empty()) {
    info_sql.BindString(column++, filter.id);
  }

  if (filter.month != ledger::PUBLISHER_MONTH::ANY) {
    info_sql.BindInt(column++, filter.month);
  }

  if (filter.year > 0) {
    info_sql.BindInt(column++, filter.year);
  }

  if (filter.reconcile_stamp > 0) {
    info_sql.BindInt64(column++, filter.reconcile_stamp);
  }

  if (filter.min_duration > 0) {
    info_sql.BindInt(column++, filter.min_duration);
  }

  if (filter.excluded != ledger::PUBLISHER_EXCLUDE_FILTER::FILTER_ALL &&
      filter.excluded !=
      ledger::PUBLISHER_EXCLUDE_FILTER::FILTER_ALL_EXCEPT_EXCLUDED) {
    info_sql.BindInt(column++, filter.excluded);
  }

  if (filter.excluded ==
      ledger::PUBLISHER_EXCLUDE_FILTER::FILTER_ALL_EXCEPT_EXCLUDED) {
    info_sql.BindInt(column++, ledger::PUBLISHER_EXCLUDE::EXCLUDED);
  }

  while (info_sql.Step()) {
    std::string id(info_sql.ColumnString(0));
    ledger::PUBLISHER_MONTH month(
        static_cast<ledger::PUBLISHER_MONTH>(info_sql.ColumnInt(7)));
    int year(info_sql.ColumnInt(8));

    ledger::PublisherInfo info(id, month, year);
    info.duration = info_sql.ColumnInt64(1);

    info.score = info_sql.ColumnDouble(2);
    info.percent = info_sql.ColumnInt64(3);
    info.weight = info_sql.ColumnDouble(4);
    info.verified = info_sql.ColumnBool(5);
    info.name = info_sql.ColumnString(9);
    info.url = info_sql.ColumnString(10);
    info.provider = info_sql.ColumnString(11);
    info.favicon_url = info_sql.ColumnString(12);
    info.reconcile_stamp = info_sql.ColumnInt64(13);

    info.excluded = static_cast<ledger::PUBLISHER_EXCLUDE>(
        info_sql.ColumnInt(6));

    list->push_back(info);
  }

  return list;
}

/**
 *
 * MEDIA PUBLISHER INFO
 *
 */
bool PublisherInfoDatabase::CreateMediaPublisherInfoTable() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  const char* name = "media_publisher_info";
  if (GetDB().DoesTableExist(name)) {
    return true;
  }

  std::string sql;
  sql.append("CREATE TABLE ");
  sql.append(name);
  sql.append(
      "("
      "media_key TEXT NOT NULL PRIMARY KEY UNIQUE,"
      "publisher_id LONGVARCHAR NOT NULL,"
      "CONSTRAINT fk_media_publisher_info_publisher_id"
      "    FOREIGN KEY (publisher_id)"
      "    REFERENCES publisher_info (publisher_id)"
      "    ON DELETE CASCADE)");

  return GetDB().Execute(sql.c_str());
}

bool PublisherInfoDatabase::InsertOrUpdateMediaPublisherInfo(
    const std::string& media_key, const std::string& publisher_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool initialized = Init();
  DCHECK(initialized);

  if (!initialized) {
    return false;
  }

  sql::Statement statement(GetDB().GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT OR REPLACE INTO media_publisher_info "
      "(media_key, publisher_id) "
      "VALUES (?, ?)"));

  statement.BindString(0, media_key);
  statement.BindString(1, publisher_id);

  return statement.Run();
}

std::unique_ptr<ledger::PublisherInfo>
PublisherInfoDatabase::GetMediaPublisherInfo(const std::string& media_key) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool initialized = Init();
  DCHECK(initialized);

  std::unique_ptr<ledger::PublisherInfo> info;

  if (!initialized) {
    return info;
  }

  sql::Statement info_sql(db_.GetUniqueStatement(
      "SELECT pi.publisher_id, pi.name, pi.url, pi.favIcon, "
      "pi.provider, pi.verified, pi.excluded "
      "FROM media_publisher_info as mpi "
      "INNER JOIN publisher_info AS pi ON mpi.publisher_id = pi.publisher_id "
      "WHERE mpi.media_key=?"));

  info_sql.BindString(0, media_key);

  if (info_sql.Step()) {
    info.reset(new ledger::PublisherInfo());
    info->id = info_sql.ColumnString(0);
    info->name = info_sql.ColumnString(1);
    info->url = info_sql.ColumnString(2);
    info->favicon_url = info_sql.ColumnString(3);
    info->provider = info_sql.ColumnString(4);
    info->verified = info_sql.ColumnBool(5);
    info->excluded = static_cast<ledger::PUBLISHER_EXCLUDE>(
        info_sql.ColumnInt(6));
  }

  return info;
}

/**
 *
 * RECURRING DONATION
 *
 */
bool PublisherInfoDatabase::CreateRecurringDonationTable() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  const char* name = "recurring_donation";
  if (GetDB().DoesTableExist(name)) {
    return true;
  }

  std::string sql;
  sql.append("CREATE TABLE ");
  sql.append(name);
  sql.append(
      "("
      "publisher_id LONGVARCHAR NOT NULL PRIMARY KEY UNIQUE,"
      "amount DOUBLE DEFAULT 0 NOT NULL,"
      "added_date INTEGER DEFAULT 0 NOT NULL,"
      "CONSTRAINT fk_recurring_donation_publisher_id"
      "    FOREIGN KEY (publisher_id)"
      "    REFERENCES publisher_info (publisher_id)"
      "    ON DELETE CASCADE)");

  return GetDB().Execute(sql.c_str());
}

bool PublisherInfoDatabase::CreateRecurringDonationIndex() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return GetDB().Execute(
      "CREATE INDEX IF NOT EXISTS recurring_donation_publisher_id_index "
      "ON recurring_donation (publisher_id)");
}

bool PublisherInfoDatabase::InsertOrUpdateRecurringDonation(
    const brave_rewards::RecurringDonation& info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool initialized = Init();
  DCHECK(initialized);

  if (!initialized) {
    return false;
  }

  sql::Statement statement(GetDB().GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT OR REPLACE INTO recurring_donation "
      "(publisher_id, amount, added_date) "
      "VALUES (?, ?, ?)"));

  statement.BindString(0, info.publisher_key);
  statement.BindDouble(1, info.amount);
  statement.BindInt64(2, info.added_date);

  return statement.Run();
}

void PublisherInfoDatabase::GetRecurringDonations(
    ledger::PublisherInfoList* list) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool initialized = Init();
  DCHECK(initialized);

  if (!initialized) {
    return;
  }

  sql::Statement info_sql(db_.GetUniqueStatement(
      "SELECT pi.publisher_id, pi.name, pi.url, pi.favIcon, "
      "rd.amount, rd.added_date, pi.verified, pi.provider "
      "FROM recurring_donation as rd "
      "INNER JOIN publisher_info AS pi ON rd.publisher_id = pi.publisher_id "));

  while (info_sql.Step()) {
    std::string id(info_sql.ColumnString(0));

    ledger::PublisherInfo publisher(id, ledger::PUBLISHER_MONTH::ANY, -1);

    publisher.name = info_sql.ColumnString(1);
    publisher.url = info_sql.ColumnString(2);
    publisher.favicon_url = info_sql.ColumnString(3);
    publisher.weight = info_sql.ColumnDouble(4);
    publisher.reconcile_stamp = info_sql.ColumnInt64(5);
    publisher.verified = info_sql.ColumnBool(6);
    publisher.provider = info_sql.ColumnString(7);

    list->push_back(publisher);
  }
}

bool PublisherInfoDatabase::RemoveRecurring(const std::string& publisher_key) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool initialized = Init();
  DCHECK(initialized);

  if (!initialized) {
    return false;
  }

  sql::Statement statement(GetDB().GetCachedStatement(
      SQL_FROM_HERE,
      "DELETE FROM recurring_donation WHERE publisher_id = ?"));

  statement.BindString(0, publisher_key);

  return statement.Run();
}

// static
int PublisherInfoDatabase::GetCurrentVersion() {
  return kCurrentVersionNumber;
}

void PublisherInfoDatabase::Vacuum() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!initialized_)
    return;

  DCHECK_EQ(0, db_.transaction_nesting()) <<
      "Can not have a transaction when vacuuming.";
  ignore_result(db_.Execute("VACUUM"));
}

void PublisherInfoDatabase::OnMemoryPressure(
    base::MemoryPressureListener::MemoryPressureLevel memory_pressure_level) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool trim_aggressively =
      memory_pressure_level ==
      base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL;
  db_.TrimMemory(trim_aggressively);
}

std::string PublisherInfoDatabase::GetDiagnosticInfo(int extended_error,
                                               sql::Statement* statement) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(initialized_);
  return db_.GetDiagnosticInfo(extended_error, statement);
}

sql::Database& PublisherInfoDatabase::GetDB() {
  return db_;
}

sql::MetaTable& PublisherInfoDatabase::GetMetaTable() {
  return meta_table_;
}

// Migration -------------------------------------------------------------------

bool PublisherInfoDatabase::MigrateV1toV2() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  std::string sql;

  // Activity info
  const char* activity = "activity_info";
  if (GetDB().DoesTableExist(activity)) {
    const char* column = "reconcile_stamp";
    if (!GetDB().DoesColumnExist(activity, column)) {
      sql.append(" ALTER TABLE ");
      sql.append(activity);
      sql.append(" ADD reconcile_stamp INTEGER DEFAULT 0 NOT NULL; ");
    }
  }

  // Contribution info
  const char* contribution = "contribution_info";
  if (GetDB().DoesTableExist(contribution)) {
    sql.append(" DROP TABLE ");
    sql.append(contribution);
    sql.append(" ; ");
  }

  if (!GetDB().Execute(sql.c_str())) {
    return false;
  }

  if (!CreateContributionInfoTable()) {
    return false;
  }

  if (!CreateContributionInfoIndex()) {
    return false;
  }

  if (!CreateRecurringDonationTable()) {
    return false;
  }

  return CreateRecurringDonationIndex();
}

bool PublisherInfoDatabase::MigrateV2toV3() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Activity info
  const char* activity = "activity_info";
  if (GetDB().DoesTableExist(activity)) {
    std::string sql = "ALTER TABLE activity_info RENAME TO activity_info_old;";

    if (!GetDB().Execute(sql.c_str())) {
      return false;
    }

    if (!CreateActivityInfoTable()) {
      return false;
    }

    if (!CreateActivityInfoIndex()) {
      return false;
    }

    std::string columns = "publisher_id, "
                          "duration, "
                          "score, "
                          "percent, "
                          "weight, "
                          "month, "
                          "year, "
                          "reconcile_stamp";

    sql = "PRAGMA foreign_keys=off;";
    sql.append("INSERT INTO activity_info (" + columns + ") "
               "SELECT " + columns + " "
               "FROM activity_info_old;");
    sql.append("UPDATE activity_info SET visits=5;");
    sql.append("DROP TABLE activity_info_old;");
    sql.append("PRAGMA foreign_keys=on;");

    return GetDB().Execute(sql.c_str());
  }

  return false;
}

sql::InitStatus PublisherInfoDatabase::EnsureCurrentVersion() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // We can't read databases newer than we were designed for.
  if (meta_table_.GetCompatibleVersionNumber() > GetCurrentVersion()) {
    LOG(WARNING) << "Publisher info database is too new.";
    return sql::INIT_TOO_NEW;
  }

  const int old_version = meta_table_.GetVersionNumber();
  const int cur_version = GetCurrentVersion();

  // Migration from version 1
  if (old_version == 1) {
    // to version 2
    if (cur_version < 3) {
      if (!MigrateV1toV2()) {
        LOG(ERROR) << "DB: Error with MigrateV1toV2";
      }
    }

    // to version 3
    if (cur_version < 4) {
      if (!MigrateV2toV3()) {
        LOG(ERROR) << "DB: Error with MigrateV2toV3";
      }
    }

    meta_table_.SetVersionNumber(cur_version);
  }

  // Migration from version 2
  if (old_version == 2) {
    // to version 3
    if (cur_version < 4) {
      if (!MigrateV2toV3()) {
        LOG(ERROR) << "DB: Error with MigrateV2toV3";
      }
    }

    meta_table_.SetVersionNumber(cur_version);
  }

  return sql::INIT_OK;
}

}  // namespace brave_rewards
