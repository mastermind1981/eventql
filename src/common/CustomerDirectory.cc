/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include <stx/exception.h>
#include <stx/protobuf/msg.h>
#include <common/CustomerDirectory.h>

using namespace stx;

namespace cm {

CustomerDirectory::CustomerDirectory(
    const String& path,
    const InetAddr master_addr) :
    master_addr_(master_addr) {
  mdb::MDBOptions mdb_opts;
  mdb_opts.data_filename = "cdb.db",
  mdb_opts.lock_filename = "cdb.db.lck";
  mdb_opts.duplicate_keys = false;

  db_ = mdb::MDB::open(path, mdb_opts);

  listCustomers([this] (const CustomerConfig& cfg) {
    customers_.emplace(cfg.customer(), new CustomerConfigRef(cfg));
  });
}

RefPtr<CustomerConfigRef> CustomerDirectory::configFor(
    const String& customer_key) const {
  std::unique_lock<std::mutex> lk(mutex_);

  auto iter = customers_.find(customer_key);
  if (iter == customers_.end()) {
    RAISEF(kNotFoundError, "customer not found: $0", customer_key);
  }

  return iter->second;
}

void CustomerDirectory::updateCustomerConfig(CustomerConfig config) {
  std::unique_lock<std::mutex> lk(mutex_);

  auto db_key = StringUtil::format("cfg~$0", config.customer());
  auto buf = msg::encode(config);

  auto txn = db_->startTransaction(false);
  txn->autoAbort();

  txn->update(db_key.data(), db_key.size(), buf->data(), buf->size());
  txn->commit();

  customers_.emplace(config.customer(), new CustomerConfigRef(config));

  for (const auto& cb : on_customer_change_) {
    cb(config);
  }
}

void CustomerDirectory::listCustomers(
    Function<void (const CustomerConfig& cfg)> fn) const {
  auto prefix = "cfg~";

  Buffer key;
  Buffer value;

  auto txn = db_->startTransaction(true);
  txn->autoAbort();

  auto cursor = txn->getCursor();
  key.append(prefix);

  if (!cursor->getFirstOrGreater(&key, &value)) {
    return;
  }

  do {
    if (!StringUtil::beginsWith(key.toString(), prefix)) {
      break;
    }

    fn(msg::decode<CustomerConfig>(value));
  } while (cursor->getNext(&key, &value));

  cursor->close();
}

void CustomerDirectory::onCustomerConfigChange(
    Function<void (const CustomerConfig& cfg)> fn) {
  std::unique_lock<std::mutex> lk(mutex_);
  on_customer_change_.emplace_back(fn);
}

void CustomerDirectory::addTableDefinition(const TableDefinition& table) {
  std::unique_lock<std::mutex> lk(mutex_);

  if (!StringUtil::isShellSafe(table.table_name())) {
    RAISEF(
        kIllegalArgumentError,
        "invalid table name: '$0'",
        table.table_name());
  }

  if (!table.has_schema_name() && !table.has_schema_inline()) {
    RAISEF(
        kIllegalArgumentError,
        "can't add table without a schema: '$0'",
        table.table_name());
  }

  auto db_key = StringUtil::format(
      "tbl~$0~$1",
      table.customer(),
      table.table_name());

  auto buf = msg::encode(table);

  auto txn = db_->startTransaction(false);
  txn->autoAbort();

  txn->insert(db_key.data(), db_key.size(), buf->data(), buf->size());
  txn->commit();

  for (const auto& cb : on_table_change_) {
    cb(table);
  }
}

void CustomerDirectory::updateTableDefinition(const TableDefinition& table) {
  std::unique_lock<std::mutex> lk(mutex_);

  if (!StringUtil::isShellSafe(table.table_name())) {
    RAISEF(
        kIllegalArgumentError,
        "invalid table name: '$0'",
        table.table_name());
  }

  if (!table.has_schema_name() && !table.has_schema_inline()) {
    RAISEF(
        kIllegalArgumentError,
        "can't add table without a schema: '$0'",
        table.table_name());
  }

  auto db_key = StringUtil::format(
      "tbl~$0~$1",
      table.customer(),
      table.table_name());

  auto buf = msg::encode(table);

  auto txn = db_->startTransaction(false);
  txn->autoAbort();

  txn->update(db_key.data(), db_key.size(), buf->data(), buf->size());
  txn->commit();

  for (const auto& cb : on_table_change_) {
    cb(table);
  }
}

void CustomerDirectory::listTableDefinitions(
    Function<void (const TableDefinition& table)> fn) const {
  auto prefix = "tbl~";

  Buffer key;
  Buffer value;

  auto txn = db_->startTransaction(true);
  txn->autoAbort();

  auto cursor = txn->getCursor();
  key.append(prefix);

  if (!cursor->getFirstOrGreater(&key, &value)) {
    return;
  }

  do {
    if (!StringUtil::beginsWith(key.toString(), prefix)) {
      break;
    }

    fn(msg::decode<TableDefinition>(value));
  } while (cursor->getNext(&key, &value));

  cursor->close();
}

void CustomerDirectory::onTableDefinitionChange(
    Function<void (const TableDefinition& tbl)> fn) {
  std::unique_lock<std::mutex> lk(mutex_);
  on_table_change_.emplace_back(fn);
}

} // namespace cm
