/**
 * This file is part of the "FnordMetric" project
 *   Copyright (c) 2014 Paul Asmuth, Google Inc.
 *
 * FnordMetric is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License v3.0. You should have received a
 * copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#ifndef _FNORD_FEEDS_FEED_H
#define _FNORD_FEEDS_FEED_H
#include "fnord/base/stdtypes.h"
#include "fnord/base/option.h"
#include "fnord-rpc/RPC.h"
#include "fnord-feeds/FeedEntry.h"

namespace fnord {
namespace feeds {

class RemoteFeed {
public:
  static const int kDefaultBatchSize = 1024;
  static const int kDefaultBufferSize = 8192;

  RemoteFeed(
      const String& name,
      fnord::comm::RPCChannel* rpc_channel,
      int batch_size = kDefaultBatchSize,
      int buffer_size = kDefaultBufferSize);

  Future<bool> appendEntry(const String& entry_data);
  Future<Option<FeedEntry>> fetchEntry(const FeedOffset& offset);
  Future<Option<FeedEntry>> fetchNextEntry(const FeedEntry& entry);
  Future<Option<FeedEntry>> fetchFirstEntry();
  Future<Option<FeedEntry>> fetchLastEntry();

  Future<Vector<FeedEntry>> fetchEntries(
      const FeedOffset& offset,
      int batch_size);

protected:
  void maybeFillBuffer();
  void fillBuffer();

  std::string name_;
  fnord::comm::RPCChannel* rpc_channel_;
  int batch_size_;
  int buffer_size_;
  uint64_t offset_;

  std::mutex fetch_mutex_;
  Deque<FeedEntry> fetch_buf_;
  std::unique_ptr<
      comm::RPC<
          std::vector<FeedEntry>,
          std::tuple<String, uint64_t, int>>> cur_fetch_rpc_;
};

}
}
#endif
