// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef NET_SRC_WORKER_THREAD_H_
#define NET_SRC_WORKER_THREAD_H_

#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pstd/include/pstd_mutex.h"
#include "pstd/include/xdebug.h"
#include "net/include/net_define.h"
#include "net/include/net_thread.h"
#include "net/include/server_thread.h"
#include "net/src/net_multiplexer.h"
#include "net/src/dispatch_thread.h"
namespace net {

class NetItem;
class NetFiredEvent;
class NetConn;
class ConnFactory;

class WorkerThread : public Thread {
 public:
  explicit WorkerThread(ConnFactory* conn_factory, ServerThread* server_thread, int queue_limit, int cron_interval = 0);

  ~WorkerThread() override;

  void set_keepalive_timeout(int timeout) { keepalive_timeout_ = timeout; }

  int conn_num() const;

  std::vector<ServerThread::ConnInfo> conns_info() const;

  //  仅仅只是moveout，并没有关闭fd等等其他操作
  std::shared_ptr<NetConn> MoveConnOut(int fd);

  //  外部接口，内部调用了MoveConnIn(const NetItem& it, bool force)
  bool MoveConnIn(const std::shared_ptr<NetConn>& conn, const NotifyType& notify_type, bool force);

  bool MoveConnIn(const NetItem& it, bool force);

  NetMultiplexer* net_multiplexer() { return net_multiplexer_.get(); }
  //  这里的删除方式是:传入一个ipport值，然后寻找，如果能找到，将string加入一个vector，或者传入all，最后在定时函数中统一进行删除
  bool TryKillConn(const std::string& ip_port);

  mutable pstd::RWMutex rwlock_; /* For external statistics */
  std::map<int, std::shared_ptr<NetConn>> conns_;

  void* private_data_ = nullptr;

 private:
  //  所属的dispatch线程
  ServerThread* server_thread_ = nullptr;
  //  生产PikaClientConn类的工厂函数
  ConnFactory* conn_factory_ = nullptr;
  //  定时任务间隔时间
  int cron_interval_ = 0;

  /*
   * The epoll handler
   */
  std::unique_ptr<NetMultiplexer> net_multiplexer_;

  std::atomic<int> keepalive_timeout_;  // keepalive second

  void* ThreadMain() override;
  //  在定时任务中我猜测处理流程如下:检查所有的连接看是否超时，如果超时尝试删除。最后检查删除数组。删除所有无效连接
  void DoCronTask();

  pstd::Mutex killer_mutex_;
  //  需要删除的连接的ip_port
  std::set<std::string> deleting_conn_ipport_;

  // clean conns
  void CloseFd(const std::shared_ptr<NetConn>& conn);
  void Cleanup();
};  // class WorkerThread

}  // namespace net
#endif  // NET_SRC_WORKER_THREAD_H_
