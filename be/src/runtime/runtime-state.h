// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#ifndef IMPALA_RUNTIME_RUNTIME_STATE_H
#define IMPALA_RUNTIME_RUNTIME_STATE_H

#include <boost/scoped_ptr.hpp>
#include <vector>
#include <string>

// NOTE: try not to add more headers here: runtime-state.h is included in many many files.
#include "common/global-types.h"  // for PlanNodeId
#include "runtime/client-cache-types.h"
#include "runtime/exec-env.h"
#include "runtime/thread-resource-mgr.h"
#include "util/auth-util.h" // for GetEffectiveUser()
#include "util/runtime-profile.h"

namespace impala {

class BufferedBlockMgr;
class DataStreamRecvr;
class DescriptorTbl;
class DiskIoRequestContext;
class Expr;
class LlvmCodeGen;
class MemTracker;
class ObjectPool;
class RuntimeFilterBank;
class Status;
class TimestampValue;
class TQueryOptions;
class TUniqueId;

/// Counts how many rows an INSERT query has added to a particular partition
/// (partitions are identified by their partition keys: k1=v1/k2=v2
/// etc. Unpartitioned tables have a single 'default' partition which is
/// identified by ROOT_PARTITION_KEY.
typedef std::map<std::string, TInsertPartitionStatus> PartitionStatusMap;

/// Stats per partition for insert queries. They key is the same as for PartitionRowCount
typedef std::map<std::string, TInsertStats> PartitionInsertStats;

/// Tracks files to move from a temporary (key) to a final destination (value) as
/// part of query finalization. If the destination is empty, the file is to be
/// deleted.
typedef std::map<std::string, std::string> FileMoveMap;

/// A collection of items that are part of the global state of a
/// query and shared across all execution nodes of that query.
class RuntimeState {
 public:
  RuntimeState(const TExecPlanFragmentParams& fragment_params, ExecEnv* exec_env);

  /// RuntimeState for executing expr in fe-support.
  RuntimeState(const TQueryCtx& query_ctx);

  /// Empty d'tor to avoid issues with scoped_ptr.
  ~RuntimeState();

  /// Set up five-level hierarchy of mem trackers: process, pool, query, fragment
  /// instance. The instance tracker is tied to our profile. Specific parts of the
  /// fragment (i.e. exec nodes, sinks, data stream senders, etc) will add a fifth level
  /// when they are initialized. This function also initializes a user function mem
  /// tracker (in the fifth level). If 'request_pool' is null, no request pool mem
  /// tracker is set up, i.e. query pools will have the process mem pool as the parent.
  void InitMemTrackers(const TUniqueId& query_id, const std::string* request_pool,
      int64_t query_bytes_limit);

  /// Initializes the runtime filter bank. Must be called after InitMemTrackers().
  void InitFilterBank();

  /// Gets/Creates the query wide block mgr.
  Status CreateBlockMgr();

  ObjectPool* obj_pool() const { return obj_pool_.get(); }
  const DescriptorTbl& desc_tbl() const { return *desc_tbl_; }
  void set_desc_tbl(DescriptorTbl* desc_tbl) { desc_tbl_ = desc_tbl; }
  const TQueryOptions& query_options() const {
    return query_ctx().request.query_options;
  }
  int batch_size() const { return query_ctx().request.query_options.batch_size; }
  bool abort_on_error() const {
    return query_ctx().request.query_options.abort_on_error;
  }
  bool strict_mode() const {
    return query_ctx().request.query_options.strict_mode;
  }
  bool abort_on_default_limit_exceeded() const {
    return query_ctx().request.query_options.abort_on_default_limit_exceeded;
  }
  const TQueryCtx& query_ctx() const { return fragment_params_.query_ctx; }
  const TPlanFragmentInstanceCtx& fragment_ctx() const {
    return fragment_params_.fragment_instance_ctx;
  }
  const TExecPlanFragmentParams& fragment_params() const { return fragment_params_; }
  const std::string& effective_user() const {
    return GetEffectiveUser(query_ctx().session);
  }
  const TUniqueId& session_id() const { return query_ctx().session.session_id; }
  const std::string& do_as_user() const { return query_ctx().session.delegated_user; }
  const std::string& connected_user() const {
    return query_ctx().session.connected_user;
  }
  const TimestampValue* now() const { return now_.get(); }
  void set_now(const TimestampValue* now);
  const TUniqueId& query_id() const { return query_ctx().query_id; }
  const TUniqueId& fragment_instance_id() const {
    return fragment_ctx().fragment_instance_id;
  }
  ExecEnv* exec_env() { return exec_env_; }
  DataStreamMgr* stream_mgr() { return exec_env_->stream_mgr(); }
  HBaseTableFactory* htable_factory() { return exec_env_->htable_factory(); }
  ImpalaBackendClientCache* impalad_client_cache() {
    return exec_env_->impalad_client_cache();
  }
  CatalogServiceClientCache* catalogd_client_cache() {
    return exec_env_->catalogd_client_cache();
  }
  DiskIoMgr* io_mgr() { return exec_env_->disk_io_mgr(); }
  MemTracker* instance_mem_tracker() { return instance_mem_tracker_.get(); }
  MemTracker* query_mem_tracker() { return query_mem_tracker_.get(); }
  ThreadResourceMgr::ResourcePool* resource_pool() { return resource_pool_; }

  FileMoveMap* hdfs_files_to_move() { return &hdfs_files_to_move_; }

  void set_fragment_root_id(PlanNodeId id) {
    DCHECK_EQ(root_node_id_, -1) << "Should not set this twice.";
    root_node_id_ = id;
  }

  /// The seed value to use when hashing tuples.
  /// See comment on root_node_id_. We add one to prevent having a hash seed of 0.
  uint32_t fragment_hash_seed() const { return root_node_id_ + 1; }

  RuntimeFilterBank* filter_bank() { return filter_bank_.get(); }

  PartitionStatusMap* per_partition_status() { return &per_partition_status_; }

  /// Returns runtime state profile
  RuntimeProfile* runtime_profile() { return &profile_; }

  /// Returns true if codegen is enabled for this query.
  bool codegen_enabled() const { return !query_options().disable_codegen; }

  /// Returns true if the codegen object has been created. Note that this may return false
  /// even when codegen is enabled if nothing has been codegen'd.
  bool codegen_created() const { return codegen_.get() != NULL; }

  /// Takes ownership of a scan node's reader context and plan fragment executor will call
  /// UnregisterReaderContexts() to unregister it when the fragment is closed. The IO
  /// buffers may still be in use and thus the deferred unregistration.
  void AcquireReaderContext(DiskIoRequestContext* reader_context);

  /// Unregisters all reader contexts acquired through AcquireReaderContext().
  void UnregisterReaderContexts();

  /// Returns codegen_ in 'codegen'. If 'initialize' is true, codegen_ will be created if
  /// it has not been initialized by a previous call already. If 'initialize' is false,
  /// 'codegen' will be set to NULL if codegen_ has not been initialized.
  Status GetCodegen(LlvmCodeGen** codegen, bool initialize = true);

  /// Returns true if codegen should be used for expr evaluation in this plan fragment.
  bool ShouldCodegenExpr() { return codegen_expr_; }

  /// Records that this fragment should use codegen for expr evaluation whenever
  /// applicable if codegen is not disabled.
  void SetCodegenExpr() { codegen_expr_ = codegen_enabled(); }

  BufferedBlockMgr* block_mgr() {
    DCHECK(block_mgr_.get() != NULL);
    return block_mgr_.get();
  }

  inline Status GetQueryStatus() {
    // Do a racy check for query_status_ to avoid unnecessary spinlock acquisition.
    if (UNLIKELY(!query_status_.ok())) {
      boost::lock_guard<SpinLock> l(query_status_lock_);
      return query_status_;
    }
    return Status::OK();
  }

  /// Log an error that will be sent back to the coordinator based on an instance of the
  /// ErrorMsg class. The runtime state aggregates log messages based on type with one
  /// exception: messages with the GENERAL type are not aggregated but are kept
  /// individually.
  bool LogError(const ErrorMsg& msg, int vlog_level = 1);

  /// Returns true if the error log has not reached max_errors_.
  bool LogHasSpace() {
    boost::lock_guard<SpinLock> l(error_log_lock_);
    return error_log_.size() < query_options().max_errors;
  }

  /// Returns the error log lines as a string joined with '\n'.
  std::string ErrorLog();

  /// Copy error_log_ to *errors
  void GetErrors(ErrorLogMap* errors);

  /// Append all accumulated errors since the last call to this function to new_errors to
  /// be sent back to the coordinator
  void GetUnreportedErrors(ErrorLogMap* new_errors);

  /// Given an error message, determine whether execution should be aborted and, if so,
  /// return the corresponding error status. Otherwise, log the error and return
  /// Status::OK(). Execution is aborted if the ABORT_ON_ERROR query option is set to
  /// true or the error is not recoverable and should be handled upstream.
  Status LogOrReturnError(const ErrorMsg& message);

  bool is_cancelled() const { return is_cancelled_; }
  void set_is_cancelled(bool v) { is_cancelled_ = v; }

  RuntimeProfile::Counter* total_cpu_timer() { return total_cpu_timer_; }
  RuntimeProfile::Counter* total_storage_wait_timer() {
    return total_storage_wait_timer_;
  }
  RuntimeProfile::Counter* total_network_send_timer() {
    return total_network_send_timer_;
  }
  RuntimeProfile::Counter* total_network_receive_timer() {
    return total_network_receive_timer_;
  }

  /// Sets query_status_ with err_msg if no error has been set yet.
  void SetQueryStatus(const std::string& err_msg) {
    boost::lock_guard<SpinLock> l(query_status_lock_);
    if (!query_status_.ok()) return;
    query_status_ = Status(err_msg);
  }

  /// Function for logging memory usages to the error log when memory limit is exceeded.
  /// If 'failed_allocation_size' is greater than zero, logs the allocation size. If
  /// 'failed_allocation_size' is zero, nothing about the allocation size is logged.
  void LogMemLimitExceeded(const MemTracker* tracker, int64_t failed_allocation_size);

  /// Sets query_status_ to MEM_LIMIT_EXCEEDED and logs all the registered trackers.
  /// Subsequent calls to this will be no-ops. Returns query_status_.
  /// If 'failed_allocation_size' is not 0, then it is the size of the allocation (in
  /// bytes) that would have exceeded the limit allocated for 'tracker'.
  /// This value and tracker are only used for error reporting.
  /// If 'msg' is non-NULL, it will be appended to query_status_ in addition to the
  /// generic "Memory limit exceeded" error.
  /// Note that this interface is deprecated and MemTracker::LimitExceeded() should be
  /// used and the error status should be returned.
  Status SetMemLimitExceeded(MemTracker* tracker = NULL,
      int64_t failed_allocation_size = 0, const ErrorMsg* msg = NULL);

  /// Returns a non-OK status if query execution should stop (e.g., the query was
  /// cancelled or a mem limit was exceeded). Exec nodes should check this periodically so
  /// execution doesn't continue if the query terminates abnormally.
  Status CheckQueryState();

 private:
  /// Allow TestEnv to set block_mgr manually for testing.
  friend class TestEnv;

  /// Set per-fragment state.
  Status Init(ExecEnv* exec_env);

  /// Create a codegen object in codegen_. No-op if it has already been called. This is
  /// created on first use.
  Status CreateCodegen();

  /// Use a custom block manager for the query for testing purposes.
  void set_block_mgr(const std::shared_ptr<BufferedBlockMgr>& block_mgr) {
    block_mgr_ = block_mgr;
  }

  static const int DEFAULT_BATCH_SIZE = 1024;

  DescriptorTbl* desc_tbl_;
  boost::scoped_ptr<ObjectPool> obj_pool_;

  /// Lock protecting error_log_
  SpinLock error_log_lock_;

  /// Logs error messages.
  ErrorLogMap error_log_;

  /// Original thrift descriptor for this fragment. Includes its unique id, the total
  /// number of fragment instances, the query context, the coordinator address, the
  /// descriptor table, etc.
  TExecPlanFragmentParams fragment_params_;

  /// Query-global timestamp, e.g., for implementing now(). Set from query_globals_.
  /// Use pointer to avoid inclusion of timestampvalue.h and avoid clang issues.
  boost::scoped_ptr<TimestampValue> now_;

  ExecEnv* exec_env_;
  boost::scoped_ptr<LlvmCodeGen> codegen_;

  /// True if this fragment should force codegen for expr evaluation.
  bool codegen_expr_;

  /// Thread resource management object for this fragment's execution.  The runtime
  /// state is responsible for returning this pool to the thread mgr.
  ThreadResourceMgr::ResourcePool* resource_pool_;

  /// Temporary Hdfs files created, and where they should be moved to ultimately.
  /// Mapping a filename to a blank destination causes it to be deleted.
  FileMoveMap hdfs_files_to_move_;

  /// Records summary statistics for the results of inserts into Hdfs partitions.
  PartitionStatusMap per_partition_status_;

  RuntimeProfile profile_;

  /// Total CPU time (across all threads), including all wait times.
  RuntimeProfile::Counter* total_cpu_timer_;

  /// Total time waiting in storage (across all threads)
  RuntimeProfile::Counter* total_storage_wait_timer_;

  /// Total time spent sending over the network (across all threads)
  RuntimeProfile::Counter* total_network_send_timer_;

  /// Total time spent receiving over the network (across all threads)
  RuntimeProfile::Counter* total_network_receive_timer_;

  /// MemTracker that is shared by all fragment instances running on this host.
  /// The query mem tracker must be released after the instance_mem_tracker_.
  std::shared_ptr<MemTracker> query_mem_tracker_;

  /// Memory usage of this fragment instance
  boost::scoped_ptr<MemTracker> instance_mem_tracker_;

  /// if true, execution should stop with a CANCELLED status
  bool is_cancelled_;

  /// Non-OK if an error has occurred and query execution should abort. Used only for
  /// asynchronously reporting such errors (e.g., when a UDF reports an error), so this
  /// will not necessarily be set in all error cases.
  SpinLock query_status_lock_;
  Status query_status_;

  /// Reader contexts that need to be closed when the fragment is closed.
  /// Synchronization is needed if there are multiple scan nodes in a plan fragment and
  /// Close() may be called on them concurrently (see IMPALA-4180).
  SpinLock reader_contexts_lock_;
  std::vector<DiskIoRequestContext*> reader_contexts_;

  /// BufferedBlockMgr object used to allocate and manage blocks of input data in memory
  /// with a fixed memory budget.
  /// The block mgr is shared by all fragments for this query.
  std::shared_ptr<BufferedBlockMgr> block_mgr_;

  /// This is the node id of the root node for this plan fragment. This is used as the
  /// hash seed and has two useful properties:
  /// 1) It is the same for all exec nodes in a fragment, so the resulting hash values
  /// can be shared.
  /// 2) It is different between different fragments, so we do not run into hash
  /// collisions after data partitioning (across fragments). See IMPALA-219 for more
  /// details.
  PlanNodeId root_node_id_;

  /// Manages runtime filters that are either produced or consumed (or both!) by plan
  /// nodes that share this runtime state.
  boost::scoped_ptr<RuntimeFilterBank> filter_bank_;

  /// prohibit copies
  RuntimeState(const RuntimeState&);
};

#define RETURN_IF_CANCELLED(state) \
  do { \
    if (UNLIKELY((state)->is_cancelled())) return Status::CANCELLED; \
  } while (false)

}

#endif
