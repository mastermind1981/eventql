/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#pragma once
#include "eventql/util/thread/threadpool.h"
#include "eventql/core/TSDBService.h"
#include "eventql/AnalyticsAuth.h"
#include "eventql/CustomerConfig.h"
#include "eventql/ConfigDirectory.h"
#include "eventql/JavaScriptContext.h"
#include "eventql/mapreduce/MapReduceTask.h"

using namespace stx;

namespace zbase {

class MapReduceService {
public:

  MapReduceService(
      ConfigDirectory* cdir,
      AnalyticsAuth* auth,
      zbase::TSDBService* tsdb,
      zbase::PartitionMap* pmap,
      zbase::ReplicationScheme* repl,
      JSRuntime* js_runtime,
      const String& cachedir);

  void executeScript(
      const AnalyticsSession& session,
      RefPtr<MapReduceJobSpec> job,
      const String& program_source);

  Option<SHA1Hash> mapPartition(
      const AnalyticsSession& session,
      RefPtr<MapReduceJobSpec> job,
      const String& table_name,
      const SHA1Hash& partition_key,
      const String& map_fn,
      const String& globals,
      const String& params,
      const Set<String>& required_columns);

  Option<SHA1Hash> reduceTables(
      const AnalyticsSession& session,
      RefPtr<MapReduceJobSpec> job,
      const Vector<String>& input_tables,
      const String& reduce_fn,
      const String& globals,
      const String& params);

  Option<String> getResultFilename(
      const SHA1Hash& result_id);

  bool saveLocalResultToTable(
      const AnalyticsSession& session,
      const String& table_name,
      const SHA1Hash& partition,
      const SHA1Hash& result_id);

  bool saveRemoteResultsToTable(
      const AnalyticsSession& session,
      const String& table_name,
      const SHA1Hash& partition,
      const Vector<String>& input_tables);

  static void downloadResult(
      const http::HTTPRequest& req,
      Function<void (const void*, size_t, const void*, size_t)> fn);

protected:
  ConfigDirectory* cdir_;
  AnalyticsAuth* auth_;
  zbase::TSDBService* tsdb_;
  zbase::PartitionMap* pmap_;
  zbase::ReplicationScheme* repl_;
  JSRuntime* js_runtime_;
  String cachedir_;
  thread::ThreadPool tpool_;
};

} // namespace zbase