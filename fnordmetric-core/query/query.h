/**
 * This file is part of the "FnordMetric" project
 *   Copyright (c) 2014 Paul Asmuth, Google Inc.
 *
 * Licensed under the MIT license (see LICENSE).
 */
#ifndef _FNORDMETRIC_QUERY_QUERY_H
#define _FNORDMETRIC_QUERY_QUERY_H
#include <stdlib.h>
#include <string>
#include <vector>
#include <memory>
#include "parser.h"
#include "planner.h"

namespace fnordmetric {
namespace query {

class Query {
public:
  static std::unique_ptr<Query> parse(const char* query_string);

  explicit Query(std::unique_ptr<Executable>&& executable);
  Query(const Query& copy) = delete;
  Query& operator=(const Query& copy) = delete;
  Query(Query&& move);

  bool execute();

protected:

  std::unique_ptr<Executable> executable_;
};

}
}
#endif
