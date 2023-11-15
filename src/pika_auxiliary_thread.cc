// Copyright (c) 2019-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "include/pika_auxiliary_thread.h"

#include "include/pika_define.h"
#include "include/pika_rm.h"
#include "include/pika_server.h"


//  Server和ReplicationServer都是一个全局的变量
extern PikaServer* g_pika_server;
extern std::unique_ptr<PikaReplicaManager> g_pika_rm;

using namespace std::chrono_literals;

PikaAuxiliaryThread::~PikaAuxiliaryThread() {
  StopThread();
  LOG(INFO) << "PikaAuxiliary thread " << thread_id() << " exit!!!";
}

void* PikaAuxiliaryThread::ThreadMain() {
  while (!should_stop()) {
    if (g_pika_server->ShouldMetaSync()) {
      //  调用本地副本管理器给其他机器的副本管理器发送请求
      g_pika_rm->SendMetaSyncRequest();
      //  g_pika_rm->SendMetaSyncRequest();这里对返回结果没有做任何处理
    } else if (g_pika_server->MetaSyncDone()) {
      //  调用本地副本管理器开始和目标机器进行同步
      g_pika_rm->RunSyncSlaveSlotStateMachine();
    }
  //  主从节点应该都会进行超时时间的检查
    pstd::Status s = g_pika_rm->CheckSyncTimeout(pstd::NowMicros());
    if (!s.ok()) {
      LOG(WARNING) << s.ToString();
    }

    //  这个应该暂时用不到。pika目前并没有主从切换的
    g_pika_server->CheckLeaderProtectedMode();

    // TODO(whoiami) timeout
    s = g_pika_server->TriggerSendBinlogSync();
    if (!s.ok()) {
      LOG(WARNING) << s.ToString();
    }
    // send to peer
    int res = g_pika_server->SendToPeer();
    if (res == 0) {
      // sleep 100 ms
      std::unique_lock lock(mu_);
      cv_.wait_for(lock, 100ms);
    } else {
      // LOG_EVERY_N(INFO, 1000) << "Consume binlog number " << res;
    }
  }
  return nullptr;
}
