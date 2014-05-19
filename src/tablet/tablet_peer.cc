// Copyright (c) 2013, Cloudera, inc.

#include "tablet/tablet_peer.h"

#include <string>
#include <vector>

#include <gflags/gflags.h>

#include "consensus/local_consensus.h"
#include "consensus/log.h"
#include "consensus/log_util.h"
#include "consensus/opid_anchor_registry.h"
#include "gutil/strings/substitute.h"
#include "gutil/sysinfo.h"
#include "tablet/transactions/alter_schema_transaction.h"
#include "tablet/transactions/change_config_transaction.h"
#include "tablet/transactions/write_transaction.h"
#include "tablet/tablet_metrics.h"
#include "tablet/tablet_bootstrap.h"
#include "tablet/tablet.pb.h"
#include "util/metrics.h"
#include "util/stopwatch.h"
#include "util/trace.h"

DEFINE_bool(log_gc_enable, true,
    "Enable garbage collection (deletion) of old write-ahead logs.");

DEFINE_int32(log_gc_sleep_delay_ms, 10000,
    "Number of milliseconds that each Log GC thread will sleep between runs.");

namespace kudu {
namespace tablet {

using consensus::Consensus;
using consensus::ConsensusRound;
using consensus::ConsensusOptions;
using consensus::LocalConsensus;
using consensus::OpId;
using log::CopyIfOpIdLessThan;
using log::Log;
using log::OpIdAnchorRegistry;
using metadata::QuorumPB;
using metadata::QuorumPeerPB;
using metadata::TabletMetadata;
using rpc::Messenger;
using rpc::RpcContext;
using strings::Substitute;
using tserver::TabletServerErrorPB;

// ============================================================================
//  Tablet Peer
// ============================================================================
TabletPeer::TabletPeer(const TabletMetadata& meta)
  : status_listener_(new TabletStatusListener(meta)),
    // prepare executor has a single thread as prepare must be done in order
    // of submission
    prepare_executor_(TaskExecutor::CreateNew("prepare exec", 1)),
    log_gc_executor_(TaskExecutor::CreateNew("log gc exec", 1)),
    log_gc_shutdown_latch_(1),
    config_sem_(1) {
  apply_executor_.reset(TaskExecutor::CreateNew("apply exec", base::NumCPUs()));
  state_ = metadata::BOOTSTRAPPING;
}

TabletPeer::~TabletPeer() {
  boost::lock_guard<simple_spinlock> lock(internal_state_lock_);
  CHECK_EQ(state_, metadata::SHUTDOWN);
}

Status TabletPeer::Init(const shared_ptr<Tablet>& tablet,
                        const scoped_refptr<server::Clock>& clock,
                        const shared_ptr<Messenger>& messenger,
                        const QuorumPeerPB& quorum_peer,
                        gscoped_ptr<Log> log,
                        OpIdAnchorRegistry* opid_anchor_registry,
                        bool local_peer) {

  {
    boost::lock_guard<simple_spinlock> lock(internal_state_lock_);
    CHECK_EQ(state_, metadata::BOOTSTRAPPING);
    state_ = metadata::CONFIGURING;
    tablet_ = tablet;
    clock_ = clock;
    quorum_peer_ = quorum_peer;
    messenger_ = messenger;
    log_.reset(log.release());
    opid_anchor_registry_ = opid_anchor_registry;
    // TODO support different consensus implementations (possibly by adding
    // a TabletPeerOptions).

    ConsensusOptions options;
    options.tablet_id = tablet_->metadata()->oid();
    consensus_.reset(new LocalConsensus(options));
  }

  DCHECK(tablet_) << "A TabletPeer must be provided with a Tablet";
  DCHECK(log_) << "A TabletPeer must be provided with a Log";
  DCHECK(opid_anchor_registry_) << "A TabletPeer must be provided with a OpIdAnchorRegistry";

  RETURN_NOT_OK_PREPEND(consensus_->Init(quorum_peer, clock_, this, log_.get()),
                        "Could not initialize consensus");

  // set consensus on the tablet to that it can store local state changes
  // in the log.
  tablet_->SetConsensus(consensus_.get());
  return Status::OK();
}

Status TabletPeer::Start(const QuorumPB& quorum) {
  // Prevent any SubmitChangeConfig calls to try and modify the config
  // until consensus is booted and the actual configuration is stored in
  // the tablet meta.
  boost::lock_guard<Semaphore> config_lock(config_sem_);

  gscoped_ptr<QuorumPB> actual_config;
  TRACE("Starting consensus");
  RETURN_NOT_OK(consensus_->Start(quorum, &actual_config));
  tablet_->metadata()->SetQuorum(*actual_config.get());

  TRACE("Flushing metadata");
  RETURN_NOT_OK(tablet_->metadata()->Flush());

  {
    boost::lock_guard<simple_spinlock> lock(internal_state_lock_);
    CHECK_EQ(state_, metadata::CONFIGURING);
    state_ = metadata::RUNNING;
  }

  RETURN_NOT_OK(StartLogGCTask());

  return Status::OK();
}

void TabletPeer::Shutdown() {
  {
    boost::lock_guard<simple_spinlock> lock(internal_state_lock_);
    state_ = metadata::QUIESCING;
  }
  tablet_->UnregisterMaintenanceOps();

  // TODO: KUDU-183: Keep track of the pending tasks and send an "abort" message.
  LOG_SLOW_EXECUTION(WARNING, 1000,
      Substitute("TabletPeer: tablet $0: Waiting for Transactions to complete", tablet_id())) {
    txn_tracker_.WaitForAllToFinish();
  }

  // Stop Log GC thread before we close the log.
  VLOG(1) << Substitute("TabletPeer: tablet $0: Shutting down Log GC thread...", tablet_id());
  log_gc_shutdown_latch_.CountDown();
  log_gc_executor_->Shutdown();

  consensus_->Shutdown();
  prepare_executor_->Shutdown();
  apply_executor_->Shutdown();

  if (VLOG_IS_ON(1)) {
    VLOG(1) << "TabletPeer: tablet " << tablet_id() << " shut down!";
  }

  {
    boost::lock_guard<simple_spinlock> lock(internal_state_lock_);
    state_ = metadata::SHUTDOWN;
  }
}

Status TabletPeer::CheckRunning() const {
  {
    boost::lock_guard<simple_spinlock> lock(internal_state_lock_);
    if (state_ != metadata::RUNNING) {
      return Status::ServiceUnavailable(Substitute("The tablet is not in a running state: $0",
                                                   metadata::TabletStatePB_Name(state_)));
    }
  }
  return Status::OK();
}

Status TabletPeer::SubmitWrite(WriteTransactionState *tx_state) {
  RETURN_NOT_OK(CheckRunning());

  // TODO keep track of the transaction somewhere so that we can cancel transactions
  // when we change leaders and/or want to quiesce a tablet.
  LeaderWriteTransaction* transaction = new LeaderWriteTransaction(&txn_tracker_, tx_state,
                                                                   consensus_.get(),
                                                                   prepare_executor_.get(),
                                                                   apply_executor_.get(),
                                                                   &prepare_replicate_lock_);
  // transaction deletes itself on delete/abort
  return transaction->Execute();
}

Status TabletPeer::SubmitAlterSchema(AlterSchemaTransactionState *tx_state) {
  RETURN_NOT_OK(CheckRunning());

  // TODO keep track of the transaction somewhere so that we can cancel transactions
  // when we change leaders and/or want to quiesce a tablet.
  LeaderAlterSchemaTransaction* transaction =
    new LeaderAlterSchemaTransaction(&txn_tracker_, tx_state, consensus_.get(),
                                     prepare_executor_.get(),
                                     apply_executor_.get(),
                                     &prepare_replicate_lock_);
  // transaction deletes itself on delete/abort
  return transaction->Execute();
}

Status TabletPeer::SubmitChangeConfig(ChangeConfigTransactionState *tx_state) {
  RETURN_NOT_OK(CheckRunning());

  // TODO keep track of the transaction somewhere so that we can cancel transactions
  // when we change leaders and/or want to quiesce a tablet.
  LeaderChangeConfigTransaction* transaction =
      new LeaderChangeConfigTransaction(&txn_tracker_, tx_state,
                                        consensus_.get(),
                                        prepare_executor_.get(),
                                        apply_executor_.get(),
                                        &prepare_replicate_lock_,
                                        &config_sem_);
  // transaction deletes itself on delete/abort
  return transaction->Execute();
}

Status TabletPeer::StartReplicaTransaction(gscoped_ptr<ConsensusRound> ctx) {
  return Status::NotSupported("Replica transactions are not supported with local consensus.");
}

void TabletPeer::GetTabletStatusPB(TabletStatusPB* status_pb_out) const {
  boost::lock_guard<simple_spinlock> lock(internal_state_lock_);
  DCHECK(status_pb_out != NULL);
  DCHECK(status_listener_.get() != NULL);
  status_pb_out->set_tablet_id(status_listener_->tablet_id());
  status_pb_out->set_table_name(status_listener_->table_name());
  status_pb_out->set_last_status(status_listener_->last_status());
  status_pb_out->set_start_key(status_listener_->start_key());
  status_pb_out->set_end_key(status_listener_->end_key());
  status_pb_out->set_state(state_);
  if (tablet() != NULL) {
    status_pb_out->set_estimated_on_disk_size(tablet()->EstimateOnDiskSize());
  }
}

Status TabletPeer::StartLogGCTask() {
  if (PREDICT_FALSE(!FLAGS_log_gc_enable)) {
    LOG(INFO) << "Log GC is disabled, not deleting old write-ahead logs!";
    return Status::OK();
  }
  shared_ptr<Future> future;
  return log_gc_executor_->Submit(boost::bind(&TabletPeer::RunLogGC, this), &future);
}

Status TabletPeer::RunLogGC() {
  do {
    OpId min_op_id;
    int32_t num_gced;
    GetEarliestNeededOpId(&min_op_id);
    Status s = log_->GC(min_op_id, &num_gced);
    if (!s.ok()) {
      s = s.CloneAndPrepend("Unexpected error while running Log GC from TabletPeer");
      LOG(ERROR) << s.ToString();
    }
    // TODO: Possibly back off if num_gced == 0.
  } while (!log_gc_shutdown_latch_.TimedWait(
               boost::posix_time::milliseconds(FLAGS_log_gc_sleep_delay_ms)));
  return Status::OK();
}

void TabletPeer::GetEarliestNeededOpId(consensus::OpId* min_op_id) const {
  min_op_id->Clear();

  // First, we anchor on the last OpId in the Log to establish a lower bound
  // and avoid racing with the other checks. This limits the Log GC candidate
  // segments before we check the anchors.
  Status s = log_->GetLastEntryOpId(min_op_id);

  // Next, we interrogate the anchor registry.
  // Returns OK if minimum known, NotFound if no anchors are registered.
  OpId min_anchor_op_id;
  s = opid_anchor_registry_->GetEarliestRegisteredOpId(&min_anchor_op_id);
  if (PREDICT_FALSE(!s.ok())) {
    DCHECK(s.IsNotFound()) << "Unexpected error calling OpIdAnchorRegistry: " << s.ToString();
  }
  CopyIfOpIdLessThan(min_anchor_op_id, min_op_id);

  // Next, interrogate the TransactionTracker.
  vector<scoped_refptr<Transaction> > pending_transactions;
  txn_tracker_.GetPendingTransactions(&pending_transactions);
  BOOST_FOREACH(const scoped_refptr<Transaction> tx, pending_transactions) {
    OpId tx_op_id;
    tx->GetOpId(&tx_op_id);
    CopyIfOpIdLessThan(tx_op_id, min_op_id);
  }

  // Finally, if nothing is known or registered, just don't delete anything.
  if (!min_op_id->IsInitialized()) {
    min_op_id->CopyFrom(log::MinimumOpId());
  }
}

string TabletPeer::tablet_id() const {
  return tablet_ != NULL ? tablet_->tablet_id() : "";
}

}  // namespace tablet
}  // namespace kudu
