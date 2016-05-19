/**
 * Copyright (c) 2016 zScale Technology GmbH <legal@zscale.io>
 * Authors:
 *   - Paul Asmuth <paul@zscale.io>
 *   - Laura Schlimmer <laura@zscale.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include <eventql/server/sql/scheduler.h>
#include <eventql/server/sql/transaction_info.h>
#include <eventql/server/sql/pipelined_expression.h>
#include <eventql/sql/qtree/QueryTreeUtil.h>

#include "eventql/eventql.h"

namespace eventql {

Scheduler::Scheduler(
    PartitionMap* pmap,
    AnalyticsAuth* auth,
    ReplicationScheme* repl_scheme) :
    pmap_(pmap),
    auth_(auth),
    repl_scheme_(repl_scheme),
    running_cnt_(0) {}

ScopedPtr<csql::TableExpression> Scheduler::buildLimit(
    csql::Transaction* ctx,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::LimitNode> node) {
  return mkScoped(
      new csql::LimitExpression(
          execution_context,
          node->limit(),
          node->offset(),
          buildExpression(ctx, execution_context, node->inputTable())));
}

ScopedPtr<csql::TableExpression> Scheduler::buildSelectExpression(
    csql::Transaction* ctx,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::SelectExpressionNode> node) {
  Vector<csql::ValueExpression> select_expressions;
  for (const auto& slnode : node->selectList()) {
    select_expressions.emplace_back(
        ctx->getCompiler()->buildValueExpression(ctx, slnode->expression()));
  }

  return mkScoped(
      new csql::SelectExpression(
          ctx,
          execution_context,
          std::move(select_expressions)));
};

ScopedPtr<csql::TableExpression> Scheduler::buildSubquery(
    csql::Transaction* txn,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::SubqueryNode> node) {
  Vector<csql::ValueExpression> select_expressions;
  Option<csql::ValueExpression> where_expr;

  if (!node->whereExpression().isEmpty()) {
    where_expr = std::move(Option<csql::ValueExpression>(
        txn->getCompiler()->buildValueExpression(
            txn,
            node->whereExpression().get())));
  }

  for (const auto& slnode : node->selectList()) {
    select_expressions.emplace_back(
        txn->getCompiler()->buildValueExpression(
            txn,
            slnode->expression()));
  }

  return mkScoped(
      new csql::SubqueryExpression(
          txn,
          execution_context,
          std::move(select_expressions),
          std::move(where_expr),
          buildExpression(txn, execution_context, node->subquery())));
}

ScopedPtr<csql::TableExpression> Scheduler::buildOrderByExpression(
    csql::Transaction* txn,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::OrderByNode> node) {
  Vector<csql::OrderByExpression::SortExpr> sort_exprs;
  for (const auto& ss : node->sortSpecs()) {
    csql::OrderByExpression::SortExpr se;
    se.descending = ss.descending;
    se.expr = txn->getCompiler()->buildValueExpression(txn, ss.expr);
    sort_exprs.emplace_back(std::move(se));
  }

  return mkScoped(
      new csql::OrderByExpression(
          txn,
          execution_context,
          std::move(sort_exprs),
          buildExpression(txn, execution_context, node->inputTable())));
}

ScopedPtr<csql::TableExpression> Scheduler::buildSequentialScan(
    csql::Transaction* txn,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::SequentialScanNode> node) {
  const auto& table_name = node->tableName();
  auto table_provider = txn->getTableProvider();

  auto seqscan = table_provider->buildSequentialScan(
      txn,
      execution_context,
      node);
  if (seqscan.isEmpty()) {
    RAISEF(kRuntimeError, "table not found: $0", table_name);
  }

  return std::move(seqscan.get());
}

ScopedPtr<csql::TableExpression> Scheduler::buildGroupByExpression(
    csql::Transaction* txn,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::GroupByNode> node) {
  Vector<csql::ValueExpression> select_expressions;
  Vector<csql::ValueExpression> group_expressions;

  for (const auto& slnode : node->selectList()) {
    select_expressions.emplace_back(
        txn->getCompiler()->buildValueExpression(
            txn,
            slnode->expression()));
  }

  for (const auto& e : node->groupExpressions()) {
    group_expressions.emplace_back(
        txn->getCompiler()->buildValueExpression(txn, e));
  }

  return mkScoped(
      new csql::GroupByExpression(
          txn,
          execution_context,
          std::move(select_expressions),
          std::move(group_expressions),
          buildExpression(txn, execution_context, node->inputTable())));
}

ScopedPtr<csql::TableExpression> Scheduler::buildPartialGroupByExpression(
    csql::Transaction* txn,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::GroupByNode> node) {
  Vector<csql::ValueExpression> select_expressions;
  Vector<csql::ValueExpression> group_expressions;

  for (const auto& slnode : node->selectList()) {
    select_expressions.emplace_back(
        txn->getCompiler()->buildValueExpression(
            txn,
            slnode->expression()));
  }

  for (const auto& e : node->groupExpressions()) {
    group_expressions.emplace_back(
        txn->getCompiler()->buildValueExpression(txn, e));
  }

  return mkScoped(
      new csql::PartialGroupByExpression(
          txn,
          std::move(select_expressions),
          std::move(group_expressions),
          buildExpression(txn, execution_context, node->inputTable())));
}

ScopedPtr<csql::TableExpression> Scheduler::buildPipelineGroupByExpression(
    csql::Transaction* txn,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::GroupByNode> node) {
  auto remote_aggregate = mkScoped(
      new PipelinedExpression(
          txn,
          execution_context,
          TransactionInfo::get(txn)->getNamespace(),
          auth_,
          kMaxConcurrency));

  auto shards = pipelineExpression(txn, node.get());
  for (size_t i = 0; i < shards.size(); ++i) {
    auto group_by_copy = mkRef(
        new csql::GroupByNode(
            node->selectList(),
            node->groupExpressions(),
            shards[i].qtree));

    group_by_copy->setIsPartialAggreagtion(true);
    if (shards[i].is_local) {
      auto partial = 
          buildPartialGroupByExpression(txn, execution_context, group_by_copy);
      remote_aggregate->addLocalQuery(std::move(partial));
    } else {
      remote_aggregate->addRemoteQuery(group_by_copy.get(), shards[i].hosts);
    }
  }

  Vector<csql::ValueExpression> select_expressions;
  for (const auto& slnode : node->selectList()) {
    select_expressions.emplace_back(
        txn->getCompiler()->buildValueExpression(
            txn,
            slnode->expression()));
  }

  return mkScoped(
      new csql::GroupByMergeExpression(
          txn,
          execution_context,
          std::move(select_expressions),
          std::move(remote_aggregate)));
}

Vector<Scheduler::PipelinedQueryTree> Scheduler::pipelineExpression(
    csql::Transaction* txn,
    RefPtr<csql::QueryTreeNode> qtree) {
  auto seqscan = csql::QueryTreeUtil::findNode<csql::SequentialScanNode>(
      qtree.get());
  if (!seqscan) {
    RAISE(kIllegalStateError, "can't pipeline query tree");
  }

  auto table_ref = TSDBTableRef::parse(seqscan->tableName());
  if (!table_ref.partition_key.isEmpty()) {
    RAISE(kIllegalStateError, "can't pipeline query tree");
  }

  auto user_data = txn->getUserData();
  if (user_data == nullptr) {
    RAISE(kRuntimeError, "no user data");
  }

  auto table = pmap_->findTable(
      TransactionInfo::get(txn)->getNamespace(),
      table_ref.table_key);
  if (table.isEmpty()) {
    RAISE(kIllegalStateError, "can't pipeline query tree");
  }

  auto partitioner = table.get()->partitioner();
  auto partitions = partitioner->listPartitions(seqscan->constraints());

  Vector<PipelinedQueryTree> shards;
  for (const auto& partition : partitions) {
    auto table_name = StringUtil::format(
        "tsdb://localhost/$0/$1",
        URI::urlEncode(table_ref.table_key),
        partition.toString());

    auto qtree_copy = qtree->deepCopy();
    auto shard = csql::QueryTreeUtil::findNode<csql::SequentialScanNode>(
        qtree_copy.get());
    shard->setTableName(table_name);

    auto shard_hosts = repl_scheme_->replicasFor(partition);

    shards.emplace_back(PipelinedQueryTree {
      .is_local = repl_scheme_->hasLocalReplica(partition),
      .qtree = shard,
      .hosts = shard_hosts
    });
  }

  return shards;
}


ScopedPtr<csql::TableExpression> Scheduler::buildJoinExpression(
    csql::Transaction* ctx,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::JoinNode> node) {
  Vector<String> column_names;
  Vector<csql::ValueExpression> select_expressions;

  for (const auto& slnode : node->selectList()) {
    select_expressions.emplace_back(
        ctx->getCompiler()->buildValueExpression(ctx, slnode->expression()));
  }

  Option<csql::ValueExpression> where_expr;
  if (!node->whereExpression().isEmpty()) {
    where_expr = std::move(Option<csql::ValueExpression>(
        ctx->getCompiler()->buildValueExpression(ctx, node->whereExpression().get())));
  }

  Option<csql::ValueExpression> join_cond_expr;
  if (!node->joinCondition().isEmpty()) {
    join_cond_expr = std::move(Option<csql::ValueExpression>(
        ctx->getCompiler()->buildValueExpression(ctx, node->joinCondition().get())));
  }

  return mkScoped(
      new csql::NestedLoopJoin(
          ctx,
          node->joinType(),
          node->inputColumnMap(),
          std::move(select_expressions),
          std::move(join_cond_expr),
          std::move(where_expr),
          buildExpression(ctx, execution_context, node->baseTable()),
          buildExpression(ctx, execution_context, node->joinedTable())));
}

ScopedPtr<csql::TableExpression> Scheduler::buildChartExpression(
    csql::Transaction* txn,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::ChartStatementNode> node) {
  Vector<Vector<ScopedPtr<csql::TableExpression>>> input_tables;
  Vector<Vector<RefPtr<csql::TableExpressionNode>>> input_table_qtrees;
  for (const auto& draw_stmt_qtree : node->getDrawStatements()) {
    input_tables.emplace_back();
    input_table_qtrees.emplace_back();
    auto draw_stmt = draw_stmt_qtree.asInstanceOf<csql::DrawStatementNode>();
    for (const auto& input_tbl : draw_stmt->inputTables()) {
      input_tables.back().emplace_back(buildExpression(
          txn,
          execution_context,
          input_tbl));
      input_table_qtrees.back().emplace_back(
          input_tbl.asInstanceOf<csql::TableExpressionNode>());
    }
  }

  return mkScoped(
      new csql::ChartExpression(
          txn,
          node,
          std::move(input_tables),
          input_table_qtrees));
}


ScopedPtr<csql::TableExpression> Scheduler::buildExpression(
    csql::Transaction* ctx,
    csql::ExecutionContext* execution_context,
    RefPtr<csql::QueryTreeNode> node) {

  if (dynamic_cast<csql::LimitNode*>(node.get())) {
    return buildLimit(ctx, execution_context, node.asInstanceOf<csql::LimitNode>());
  }

  if (dynamic_cast<csql::SelectExpressionNode*>(node.get())) {
    return buildSelectExpression(
        ctx,
        execution_context,
        node.asInstanceOf<csql::SelectExpressionNode>());
  }

  if (dynamic_cast<csql::SubqueryNode*>(node.get())) {
    return buildSubquery(
        ctx,
        execution_context,
        node.asInstanceOf<csql::SubqueryNode>());
  }

  if (dynamic_cast<csql::OrderByNode*>(node.get())) {
    return buildOrderByExpression(
        ctx,
        execution_context,
        node.asInstanceOf<csql::OrderByNode>());
  }

  if (dynamic_cast<csql::SequentialScanNode*>(node.get())) {
    return buildSequentialScan(
        ctx,
        execution_context,
        node.asInstanceOf<csql::SequentialScanNode>());
  }

  if (dynamic_cast<csql::GroupByNode*>(node.get())) {
    auto group_node = node.asInstanceOf<csql::GroupByNode>();
    if (group_node->isPartialAggregation()) {
      return buildPartialGroupByExpression(
          ctx,
          execution_context,
          group_node);
    }

    if (isPipelineable(*group_node->inputTable())) {
      return buildPipelineGroupByExpression(
          ctx,
          execution_context,
          group_node);
    } else {
      return buildGroupByExpression(
          ctx,
          execution_context,
          group_node);
    }
  }

  if (dynamic_cast<csql::ShowTablesNode*>(node.get())) {
    return mkScoped(new csql::ShowTablesExpression(ctx));
  }

  if (dynamic_cast<csql::DescribeTableNode*>(node.get())) {
    return mkScoped(new csql::DescribeTableStatement(
        ctx,
        node.asInstanceOf<csql::DescribeTableNode>()->tableName()));
  }

  if (dynamic_cast<csql::JoinNode*>(node.get())) {
    return buildJoinExpression(
        ctx,
        execution_context,
        node.asInstanceOf<csql::JoinNode>());
  }

  if (dynamic_cast<csql::ChartStatementNode*>(node.get())) {
    return buildChartExpression(
        ctx,
        execution_context,
        node.asInstanceOf<csql::ChartStatementNode>());
  }

  RAISEF(
      kRuntimeError,
      "cannot figure out how to execute that query, sorry. -- $0",
      node->toString());
};

ScopedPtr<csql::ResultCursor> Scheduler::execute(
    csql::QueryPlan* query_plan,
    csql::ExecutionContext* execution_context,
    size_t stmt_idx) {
  auto qtree = query_plan->getStatement(stmt_idx);
  rewriteTableTimeSuffix(qtree);

  return mkScoped(
      new csql::TableExpressionResultCursor(
          buildExpression(
              query_plan->getTransaction(),
              execution_context,
              qtree)));
};

bool Scheduler::isPipelineable(const csql::QueryTreeNode& qtree) {
  if (dynamic_cast<const csql::SequentialScanNode*>(&qtree)) {
    return true;
  }

  if (dynamic_cast<const csql::SelectExpressionNode*>(&qtree)) {
    return true;
  }

  if (dynamic_cast<const csql::SubqueryNode*>(&qtree)) {
    return isPipelineable(
        *dynamic_cast<const csql::SubqueryNode&>(qtree).subquery());
  }

  return false;
}

void Scheduler::rewriteTableTimeSuffix(RefPtr<csql::QueryTreeNode> node) {
  auto seqscan = dynamic_cast<csql::SequentialScanNode*>(node.get());
  if (seqscan) {
    auto table_ref = TSDBTableRef::parse(seqscan->tableName());
    if (!table_ref.timerange_begin.isEmpty() &&
        !table_ref.timerange_limit.isEmpty()) {
      seqscan->setTableName(table_ref.table_key);

      auto pred = mkRef(
          new csql::CallExpressionNode(
              "logical_and",
              Vector<RefPtr<csql::ValueExpressionNode>>{
                new csql::CallExpressionNode(
                    "gte",
                    Vector<RefPtr<csql::ValueExpressionNode>>{
                      new csql::ColumnReferenceNode("time"),
                      new csql::LiteralExpressionNode(
                          csql::SValue(csql::SValue::IntegerType(
                              table_ref.timerange_begin.get().unixMicros())))
                    }),
                new csql::CallExpressionNode(
                    "lte",
                    Vector<RefPtr<csql::ValueExpressionNode>>{
                      new csql::ColumnReferenceNode("time"),
                      new csql::LiteralExpressionNode(
                          csql::SValue(csql::SValue::IntegerType(
                              table_ref.timerange_limit.get().unixMicros())))
                    })
              }));

      auto where_expr = seqscan->whereExpression();
      if (!where_expr.isEmpty()) {
        pred = mkRef(
            new csql::CallExpressionNode(
                "logical_and",
                Vector<RefPtr<csql::ValueExpressionNode>>{
                  where_expr.get(),
                  pred.asInstanceOf<csql::ValueExpressionNode>()
                }));
      }

      seqscan->setWhereExpression(
          pred.asInstanceOf<csql::ValueExpressionNode>());
    }
  }

  auto ntables = node->numChildren();
  for (int i = 0; i < ntables; ++i) {
    rewriteTableTimeSuffix(node->child(i));
  }
}


} // namespace eventql
