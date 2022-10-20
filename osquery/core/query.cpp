/**
 * Copyright (c) 2014-present, The osquery authors
 *
 * This source code is licensed as defined by the LICENSE file found in the
 * root directory of this source tree.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
 */

#include <algorithm>
#include <string>
#include <vector>

#include <osquery/core/flagalias.h>
#include <osquery/core/flags.h>
#include <osquery/core/query.h>
#include <osquery/database/database.h>
#include <osquery/logger/logger.h>

#include <osquery/utils/json/json.h>

namespace rj = rapidjson;

namespace osquery {

DECLARE_bool(decorations_top_level);

/// Log numeric values as numbers (in JSON syntax)
FLAG(bool,
     logger_numerics,
     false,
     "Use numeric JSON syntax for numeric values");
FLAG_ALIAS(bool, log_numerics_as_numbers, logger_numerics);

uint64_t Query::getPreviousEpoch() const {
  uint64_t epoch = 0;
  std::string raw;
  auto status = getDatabaseValue(kQueries, name_ + "epoch", raw);
  if (status.ok()) {
    epoch = std::stoull(raw);
  }
  return epoch;
}

uint64_t Query::getQueryCounter(bool all_records, bool new_query) const {
  uint64_t counter = 0;
  if (all_records) {
    return counter;
  }

  // If it's a new query but not returning all records, start with 1 instead of 0.
  // This allows consumers to reliably distinguish between differential results and
  // results with all records.
  if (new_query) {
    return counter + 1;
  }

  std::string raw;
  auto status = getDatabaseValue(kQueries, name_ + "counter", raw);
  if (status.ok()) {
    counter = std::stoull(raw) + 1;
  }
  return counter;
}

Status Query::getPreviousQueryResults(QueryDataSet& results) const {
  std::string raw;
  auto status = getDatabaseValue(kQueries, name_, raw);
  if (!status.ok()) {
    return status;
  }

  status = deserializeQueryDataJSON(raw, results);
  if (!status.ok()) {
    return status;
  }
  return Status::success();
}

std::vector<std::string> Query::getStoredQueryNames() {
  std::vector<std::string> results;
  scanDatabaseKeys(kQueries, results);
  return results;
}

bool Query::isQueryNameInDatabase() const {
  auto names = Query::getStoredQueryNames();
  return std::find(names.begin(), names.end(), name_) != names.end();
}

static inline void saveQuery(const std::string& name,
                             const std::string& query) {
  setDatabaseValue(kQueries, "query." + name, query);
}

bool Query::isNewQuery() const {
  std::string query;
  getDatabaseValue(kQueries, "query." + name_, query);
  return (query != query_);
}

void Query::getQueryStatus(uint64_t epoch,
			   uint64_t& previous_epoch,
                           bool& new_epoch,
                           bool& new_query) const {
  previous_epoch = getPreviousEpoch();
  if (!isQueryNameInDatabase()) {
    // This is the first encounter of the scheduled query.
    new_epoch = true;
    new_query = true;
    LOG(INFO) << "Storing initial results for new scheduled query: " << name_;
    saveQuery(name_, query_);
  } else if (previous_epoch != epoch) {
    new_epoch = true;
    LOG(INFO) << "New Epoch " << epoch << " for scheduled query " << name_;
  } else if (isNewQuery()) {
    // This query is 'new' in that the previous results may be invalid.
    new_query = true;
    LOG(INFO) << "Scheduled query has been updated: " + name_;
    saveQuery(name_, query_);
  }
}

Status Query::incrementCounter(bool all_records, bool new_query, uint64_t& counter) const {
  counter = getQueryCounter(all_records, new_query);
  return setDatabaseValue(kQueries, name_ + "counter", std::to_string(counter));
}

Status Query::addNewEvents(QueryDataTyped current_qd,
			   QueryLogItem& item) const {
  bool new_epoch = false;
  bool new_query = false;
  getQueryStatus(item.epoch, item.previous_epoch, new_epoch, new_query);
  if (new_epoch) {
    auto status = setDatabaseValue(kQueries, name_, "[]");
    if (!status.ok()) {
      return status;
    }
  }
  item.results.added = std::move(current_qd);
  if (!item.results.added.empty()) {
    auto status = incrementCounter(false, new_epoch || new_query, item.counter);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

Status Query::addNewResults(QueryDataTyped qd,
                            const uint64_t epoch,
                            uint64_t& counter) const {
  QueryLogItem item;
  item.epoch = epoch;
  return addNewResults(std::move(qd), item, false);
}

Status Query::addNewResults(QueryDataTyped current_qd,
			    QueryLogItem& item,
                            bool calculate_diff) const {
  bool new_epoch = false;
  bool new_query = false;
  getQueryStatus(item.epoch, item.previous_epoch, new_epoch, new_query);

  // Use a 'target' avoid copying the query data when serializing and saving.
  // If a differential is requested and needed the target remains the original
  // query data, otherwise the content is moved to the differential's added set.
  const auto* target_gd = &current_qd;
  bool update_db = true;
  if (!new_query && calculate_diff) {
    // Get the rows from the last run of this query name.
    QueryDataSet previous_qd;
    auto status = getPreviousQueryResults(previous_qd);
    if (!status.ok()) {
      return status;
    }

    // Calculate the differential between previous and current query results.
    if (new_epoch) {
      // If this is a new epoch, we first finish reporting the changes in the previous
      // epoch then report a snapshot of all results to start off the new epoch. Reporting
      // the changes in the previous epoch ensures consumers can filter out the snapshot
      // at the start of an epoch (to avoid re-processing duplicate events), while still not
      // missing any changes.
      item.previous_remaining = diff(previous_qd, current_qd);
      item.results.added = std::move(current_qd);
      target_gd = &item.results.added;
    } else {
      item.results = diff(previous_qd, current_qd);
    }
    if (!new_epoch && item.results.added.empty() && item.results.removed.empty()) {
      update_db = false;
    }
  } else {
    item.results.added = std::move(current_qd);
    target_gd = &item.results.added;
  }

  if (update_db) {
    // Replace the "previous" query data with the current.
    std::string json;
    auto status = serializeQueryDataJSON(*target_gd, json, true);
    if (!status.ok()) {
      return status;
    }

    status = setDatabaseValue(kQueries, name_, json);
    if (!status.ok()) {
      return status;
    }

    status = setDatabaseValue(
        kQueries, name_ + "epoch", std::to_string(item.epoch));
    if (!status.ok()) {
      return status;
    }
  }

  if (new_epoch && !(item.previous_remaining.added.empty() && item.previous_remaining.removed.empty())) {
    auto status = incrementCounter(false, false, item.previous_remaining_counter);
    if (!status.ok()) {
      return status;
    }
  }

  if (update_db || new_epoch || new_query) {
    auto status = incrementCounter(new_epoch, new_query, item.counter);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

Status deserializeDiffResults(const rj::Value& doc, DiffResults& dr) {
  if (!doc.IsObject()) {
    return Status(1);
  }

  if (doc.HasMember("removed")) {
    auto status = deserializeQueryData(doc["removed"], dr.removed);
    if (!status.ok()) {
      return status;
    }
  }

  if (doc.HasMember("added")) {
    auto status = deserializeQueryData(doc["added"], dr.added);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

inline void addLegacyFieldsAndDecorations(bool is_previous_remaining,
					  const QueryLogItem& item,
                                          JSON& doc,
                                          rj::Document& obj) {
  // Apply legacy fields.
  doc.addRef("name", item.name, obj);
  doc.addRef("hostIdentifier", item.identifier, obj);
  doc.addRef("calendarTime", item.calendar_time, obj);
  doc.add("unixTime", item.time, obj);
  if (is_previous_remaining) {
    doc.add("epoch", static_cast<size_t>(item.previous_epoch), obj);
    doc.add("previous_epoch", static_cast<size_t>(item.previous_epoch), obj);
    doc.add("counter", static_cast<size_t>(item.previous_remaining_counter), obj);
  } else {
    doc.add("epoch", static_cast<size_t>(item.epoch), obj);
    doc.add("previous_epoch", static_cast<size_t>(item.previous_epoch), obj);
    doc.add("counter", static_cast<size_t>(item.counter), obj);
  }

  // Apply field indicating if numerics are serialized as numbers
  doc.add("numerics", FLAGS_logger_numerics, obj);

  // Append the decorations.
  if (!item.decorations.empty()) {
    auto dec_obj = doc.getObject();
    auto target_obj = std::ref(dec_obj);
    if (FLAGS_decorations_top_level) {
      target_obj = std::ref(obj);
    }
    for (const auto& name : item.decorations) {
      doc.addRef(name.first, name.second, target_obj);
    }
    if (!FLAGS_decorations_top_level) {
      doc.add("decorations", dec_obj, obj);
    }
  }
}

inline void getLegacyFieldsAndDecorations(const JSON& doc, QueryLogItem& item) {
  if (doc.doc().HasMember("decorations")) {
    if (doc.doc()["decorations"].IsObject()) {
      for (const auto& i : doc.doc()["decorations"].GetObject()) {
        item.decorations[i.name.GetString()] = i.value.GetString();
      }
    }
  }

  item.name = doc.doc()["name"].GetString();
  item.identifier = doc.doc()["hostIdentifier"].GetString();
  item.calendar_time = doc.doc()["calendarTime"].GetString();
  item.time = doc.doc()["unixTime"].GetUint64();
}

Status serializeQueryLogItem(bool is_previous_remaining, const QueryLogItem& item, JSON& doc) {
  const auto* dr = &item.results;
  if (is_previous_remaining) {
    dr = &item.previous_remaining;
  }  
  if (dr->added.size() > 0 || dr->removed.size() > 0) {
    auto obj = doc.getObject();
    auto status =
      serializeDiffResults(*dr, doc, obj, FLAGS_logger_numerics);
    if (!status.ok()) {
      return status;
    }

    doc.add("diffResults", obj);
  } else {
    auto arr = doc.getArray();
    auto status = serializeQueryData(
        item.snapshot_results, doc, arr, FLAGS_logger_numerics);
    if (!status.ok()) {
      return status;
    }

    doc.add("snapshot", arr);
    doc.addRef("action", "snapshot");
  }

  addLegacyFieldsAndDecorations(is_previous_remaining, item, doc, doc.doc());
  return Status::success();
}

Status serializeEvent(bool is_previous_remaining,
		      const QueryLogItem& item,
                      const rj::Value& event_obj,
                      JSON& doc,
                      rj::Document& obj) {
  addLegacyFieldsAndDecorations(is_previous_remaining, item, doc, obj);
  auto columns_obj = doc.getObject();
  for (const auto& i : event_obj.GetObject()) {
    // Yield results as a "columns." map to avoid namespace collisions.
    doc.add(i.name.GetString(), i.value, columns_obj);
  }
  doc.add("columns", columns_obj, obj);
  return Status::success();
}

Status _serializeQueryLogItemAsEvents(bool is_previous_remaining, const QueryLogItem& item, JSON& doc) {
  auto temp_doc = JSON::newObject();
  const auto* dr = &item.results;
  if (is_previous_remaining) {
    dr = &item.previous_remaining;
  }
  if (!dr->added.empty() || !dr->removed.empty()) {
    auto status = serializeDiffResults(
        *dr, temp_doc, temp_doc.doc(), FLAGS_logger_numerics);
    if (!status.ok()) {
      return status;
    }
  } else if (!item.snapshot_results.empty()) {
    auto arr = doc.getArray();
    auto status = serializeQueryData(
        item.snapshot_results, temp_doc, arr, FLAGS_logger_numerics);
    if (!status.ok()) {
      return status;
    }
    temp_doc.add("snapshot", arr);
  } else {
    // This error case may also be represented in serializeQueryLogItem.
    return Status(1, "No differential or snapshot results");
  }

  for (auto& action : temp_doc.doc().GetObject()) {
    for (auto& row : action.value.GetArray()) {
      auto obj = doc.getObject();
      serializeEvent(is_previous_remaining, item, row, doc, obj);
      doc.addCopy("action", action.name.GetString(), obj);
      doc.push(obj);
    }
  }
  return Status::success();
}

Status serializeQueryLogItemAsEvents(const QueryLogItem& item, JSON& doc) {
  auto status = _serializeQueryLogItemAsEvents(true, item, doc);
  if (!status.ok()) {
    return status;
  }
  return _serializeQueryLogItemAsEvents(false, item, doc);
}

Status _serializeQueryLogItemJSON(bool is_previous_remaining, const QueryLogItem& item, std::string& json) {
  auto doc = JSON::newObject();
  auto status = serializeQueryLogItem(is_previous_remaining, item, doc);
  if (!status.ok()) {
    return status;
  }

  return doc.toString(json);
}

Status serializeQueryLogItemJSON(const QueryLogItem& item, std::vector<std::string>& json_items) {
  std::string json;
  auto status = _serializeQueryLogItemJSON(true, item, json);
  if (!status.ok()) {
    return status;
  }
  if (!json.empty()) {
    json_items.emplace_back(json);
  }
  status = _serializeQueryLogItemJSON(false, item, json);
  if (!status.ok()) {
    return status;
  }
  if (!json.empty()) {
    json_items.emplace_back(json);
  }
  return Status::success();
}

Status serializeQueryLogItemAsEventsJSON(const QueryLogItem& item,
                                         std::vector<std::string>& items) {
  auto doc = JSON::newArray();
  auto status = serializeQueryLogItemAsEvents(item, doc);
  if (!status.ok()) {
    return status;
  }

  // return doc.toString()
  for (auto& event : doc.doc().GetArray()) {
    rj::StringBuffer sb;
    rj::Writer<rj::StringBuffer> writer(sb);
    event.Accept(writer);
    items.push_back(sb.GetString());
  }
  return Status::success();
}

} // namespace osquery
