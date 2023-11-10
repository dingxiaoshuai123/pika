// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef NET_INCLUDE_SERVER_THREAD_H_
#define NET_INCLUDE_SERVER_THREAD_H_

#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef __ENABLE_SSL
#  include <openssl/conf.h>
#  include <openssl/err.h>
#  include <openssl/ssl.h>
#endif

#include "net/include/net_define.h"
#include "net/include/net_thread.h"
#include "net/src/net_multiplexer.h"
#include "pstd/include/env.h"
#include "pstd/include/pstd_mutex.h"
#include "pstd/include/pstd_status.h"

// remove 'unused parameter' warning
#define UNUSED(expr) \
  do {               \
    (void)(expr);    \
  } while (0)

namespace net {

class ServerSocket;

class NetConn;
struct NetFiredEvent;
class ConnFactory;
class WorkerThread;

/*
 *  ServerHandle will be invoked at appropriate occasion
 *  in server thread's main loop.
 */
class ServerHandle {
 public:
  ServerHandle() = default;
  virtual ~ServerHandle() = default;

  /*
   *  CronHandle() will be invoked on every cron_interval elapsed.
   */
  virtual void CronHandle() const {}

  /*
   *  FdTimeoutHandle(...) will be invoked after connection timeout.
   */
  virtual void FdTimeoutHandle(int fd, const std::string& ip_port) const {
    UNUSED(fd);
    UNUSED(ip_port);
  }

  /*
   *  FdClosedHandle(...) will be invoked before connection closed.
   */
  virtual void FdClosedHandle(int fd, const std::string& ip_port) const {
    UNUSED(fd);
    UNUSED(ip_port);
  }

  /*
   *  AccessHandle(...) will be invoked after client fd accept()
   *  but before handled.
   */
  virtual bool AccessHandle(std::string& ip) const {
    UNUSED(ip);
    return true;
  }

  virtual bool AccessHandle(int fd, std::string& ip) const {
    UNUSED(fd);
    UNUSED(ip);
    return true;
  }

  /*
   *  CreateWorkerSpecificData(...) will be invoked in StartThread() routine.
   *  'data' pointer should be assigned, we will pass the pointer as parameter
   *  in every connection's factory create function.
   */
  virtual int CreateWorkerSpecificData(void** data) const {
    UNUSED(data);
    return 0;
  }

  /*
   *  DeleteWorkerSpecificData(...) is related to CreateWorkerSpecificData(...),
   *  it will be invoked in StopThread(...) routine,
   *  resources assigned in CreateWorkerSpecificData(...) should be deleted in
   *  this handle
   */
  virtual int DeleteWorkerSpecificData(void* data) const {
    UNUSED(data);
    return 0;
  }
};

const char kKillAllConnsTask[] = "kill_all_conns";

const int kDefaultKeepAliveTime = 60;  // (s)

class ServerThread : public Thread {
 public:
  ServerThread(int port, int cron_interval, const ServerHandle* handle);
  ServerThread(const std::string& bind_ip, int port, int cron_interval, const ServerHandle* handle);
  ServerThread(const std::set<std::string>& bind_ips, int port, int cron_interval, const ServerHandle* handle);

#ifdef __ENABLE_SSL
  /*
   * Enable TLS, set before StartThread, default: false
   * Just HTTPConn has supported for now.
   */
  int EnableSecurity(const std::string& cert_file, const std::string& key_file);
  SSL_CTX* ssl_ctx() { return ssl_ctx_; }
  bool security() { return security_; }
#endif

  int SetTcpNoDelay(int connfd);

  /*
   * StartThread will return the error code as pthread_create
   * Return 0 if success
   */
  int StartThread() override;

  virtual void set_keepalive_timeout(int timeout) = 0;

  virtual int conn_num() const = 0;

  struct ConnInfo {
    int fd; // 套接字文件描述符
    std::string ip_port; //  ip和port
    struct timeval last_interaction; //  最后互动时间，用于超时判断
  };
  virtual std::vector<ConnInfo> conns_info() const = 0;

  // Move out from server thread
  // 移除一个连接
  virtual std::shared_ptr<NetConn> MoveConnOut(int fd) = 0;
  // Move into server thread
  // 添加一个连接
  virtual void MoveConnIn(std::shared_ptr<NetConn> conn, const NotifyType& type) = 0;

  virtual void KillAllConns() = 0;
  virtual bool KillConn(const std::string& ip_port) = 0;

  virtual void HandleNewConn(int connfd, const std::string& ip_port) = 0;

  virtual void SetQueueLimit(int queue_limit) {}

  ~ServerThread() override;

 protected:
  /*
   * The event handler
   */
  //  封装一个多路复用模型作为组件
  std::unique_ptr<NetMultiplexer> net_multiplexer_;

 private:
  friend class HolyThread;
  friend class DispatchThread;
  friend class WorkerThread;

  int cron_interval_ = 0;
  virtual void DoCronTask();

  // process events in notify_queue
  virtual void ProcessNotifyEvents(const NetFiredEvent* pfe);
  

  // 将需要的回调函数（与服务器本身相关的函数，比如处理一个连接的超时、关闭）封装在一个handle对象中。
  const ServerHandle* handle_;
  bool own_handle_ = false;

#ifdef __ENABLE_SSL
  bool security_;
  SSL_CTX* ssl_ctx_;
#endif

  /*
   * The tcp server port and address
   */
  int port_ = -1; //  port需要外界传入
  std::set<std::string> ips_; //  一个server可能有多个网卡
  std::vector<std::shared_ptr<ServerSocket>> server_sockets_; //  套接字结构
  std::set<int32_t> server_fds_;//  这个server可能监听多个套接字，这里存放该server监听的所有套接字

  virtual int InitHandle();
  void* ThreadMain() override;
  /*
   * The server event handle
   */
  virtual void HandleConnEvent(NetFiredEvent* pfe) = 0;
};

// !!!Attention: If u use this constructor, the keepalive_timeout_ will
// be equal to kDefaultKeepAliveTime(60s). In master-slave mode, the slave
// binlog receiver will close the binlog sync connection in HolyThread::DoCronTask
// if master did not send data in kDefaultKeepAliveTime.
extern ServerThread* NewHolyThread(int port, ConnFactory* conn_factory, int cron_interval = 0,
                                   const ServerHandle* handle = nullptr);
extern ServerThread* NewHolyThread(const std::string& bind_ip, int port, ConnFactory* conn_factory,
                                   int cron_interval = 0, const ServerHandle* handle = nullptr);
extern ServerThread* NewHolyThread(const std::set<std::string>& bind_ips, int port, ConnFactory* conn_factory,
                                   int cron_interval = 0, const ServerHandle* handle = nullptr);
extern ServerThread* NewHolyThread(const std::set<std::string>& bind_ips, int port, ConnFactory* conn_factory,
                                   bool async, int cron_interval = 0, const ServerHandle* handle = nullptr);

/**
 * This type Dispatch thread just get Connection and then Dispatch the fd to
 * worker thread
 *
 * @brief
 *
 * @param port          the port number
 * @param conn_factory  connection factory object
 * @param cron_interval the cron job interval
 * @param queue_limit   the size limit of workers' connection queue
 * @param handle        the server's handle (e.g. CronHandle, AccessHandle...)
 * @param ehandle       the worker's enviroment setting handle
 */
extern ServerThread* NewDispatchThread(int port, int work_num, ConnFactory* conn_factory, int cron_interval = 0,
                                       int queue_limit = 1000, const ServerHandle* handle = nullptr);
extern ServerThread* NewDispatchThread(const std::string& ip, int port, int work_num, ConnFactory* conn_factory,
                                       int cron_interval = 0, int queue_limit = 1000,
                                       const ServerHandle* handle = nullptr);
extern ServerThread* NewDispatchThread(const std::set<std::string>& ips, int port, int work_num,
                                       ConnFactory* conn_factory, int cron_interval = 0, int queue_limit = 1000,
                                       const ServerHandle* handle = nullptr);

}  // namespace net
#endif  // NET_INCLUDE_SERVER_THREAD_H_
