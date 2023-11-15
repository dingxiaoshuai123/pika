// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef PIKA_CLIENT_CONN_H_
#define PIKA_CLIENT_CONN_H_

#include <utility>

#include "include/pika_command.h"


//  封装了一个时间戳
// TODO: stat time costing in write out data to connfd
struct TimeStat {
  TimeStat() = default;
  void Reset() {
    enqueue_ts_ = dequeue_ts_ = 0;
    process_done_ts_ = 0;
  }

  uint64_t start_ts() const {
    return enqueue_ts_;
  }

  uint64_t total_time() const {
    return process_done_ts_ > enqueue_ts_ ? process_done_ts_ - enqueue_ts_ : 0;
  }

  uint64_t queue_time() const {
    return dequeue_ts_ > enqueue_ts_ ? dequeue_ts_ - enqueue_ts_ : 0;
  }

  uint64_t process_time() const {
    return process_done_ts_ > dequeue_ts_ ? process_done_ts_ - dequeue_ts_ : 0;
  }

  //  记录三个时间，入队时间、出队时间和执行完成时间
  uint64_t enqueue_ts_;
  uint64_t dequeue_ts_;
  uint64_t process_done_ts_;
};

//  一个客户连接的数据封装
//  继承自RedisConn，当解析出多条完整的命令存放在vector中后，就会调用completecb。
class PikaClientConn : public net::RedisConn {
 public:
  using WriteCompleteCallback = std::function<void()>;

  //  一个连接中用于执行命令的任务的数据封装
  //  里面会有这次的命令对象的指针、所属的连接、用于保存返回结果的string指针以及要进行操作的db和slot
  //  还有一些东西在用到的时候在说。
  struct BgTaskArg {
    std::shared_ptr<Cmd> cmd_ptr;
    std::shared_ptr<PikaClientConn> conn_ptr;
    std::vector<net::RedisCmdArgsType> redis_cmds;
    std::shared_ptr<std::string> resp_ptr;
    LogOffset offset;
    std::string db_name;
    uint32_t slot_id;
  };

  // Auth related
  // 一个连接中与身份验证有关的数据封装
  // 就包含一个状态
  class AuthStat {
   public:
    void Init();// 如果requirepass为空，那么初始状态为kAdminAuthed，否则为kNoAuthed
    bool IsAuthed(const std::shared_ptr<Cmd>& cmd_ptr);//  验证一个命令是否有资格执行
    bool ChecknUpdate(const std::string& message); //  根据message_更新stat_

   private:
    enum StatType {
      kNoAuthed = 0,
      kAdminAuthed,
      kLimitAuthed,
    };
    StatType stat_;
  };

  PikaClientConn(int fd, const std::string& ip_port, net::Thread* server_thread, net::NetMultiplexer* mpx,
                 const net::HandleType& handle_type, int max_conn_rbuf_size);
  ~PikaClientConn() override = default;


  //  当redisparse解析出一条或者多条数据之后，就会调用该函数。
  void ProcessRedisCmds(const std::vector<net::RedisCmdArgsType>& argvs, bool async,
                                std::string* response) override;

  void BatchExecRedisCmd(const std::vector<net::RedisCmdArgsType>& argvs);
  int DealMessage(const net::RedisCmdArgsType& argv, std::string* response) override { return 0; }
  static void DoBackgroundTask(void* arg);
  static void DoExecTask(void* arg);

  bool IsPubSub() { return is_pubsub_; }
  void SetIsPubSub(bool is_pubsub) { is_pubsub_ = is_pubsub; }
  void SetCurrentTable(const std::string& db_name) { current_db_ = db_name; }
  const std::string& GetCurrentTable() override{ return current_db_; }
  void SetWriteCompleteCallback(WriteCompleteCallback cb) { write_completed_cb_ = std::move(cb); }

  net::ServerThread* server_thread() { return server_thread_; }

  AuthStat& auth_stat() { return auth_stat_; }

  //  保存一批命令的个数，也是返回结果的个数
  std::atomic<int> resp_num;
  //  保存一批命令的返回结果
  std::vector<std::shared_ptr<std::string>> resp_array;

  std::shared_ptr<TimeStat> time_stat_;
 private:
  net::ServerThread* const server_thread_;
  std::string current_db_;
  WriteCompleteCallback write_completed_cb_;
  bool is_pubsub_ = false;

  //   执行该命令的时候，只需要传递命令的名称以及参数和要保存结果的指针。
  //   该函数回返回一个命令对象，与执行结果有关的一切都会存放在命名对象中。
  std::shared_ptr<Cmd> DoCmd(const PikaCmdArgsType& argv, const std::string& opt,
                             const std::shared_ptr<std::string>& resp_ptr);

  void ProcessSlowlog(const PikaCmdArgsType& argv, uint64_t do_duration);
  void ProcessMonitor(const PikaCmdArgsType& argv);

  void ExecRedisCmd(const PikaCmdArgsType& argv, const std::shared_ptr<std::string>& resp_ptr);
  void TryWriteResp();

  AuthStat auth_stat_; // 一个连接的身份验证
};

//  代表一个客户端的信息
//  首先是文件描述，用于标识该进程中的一个资源（怀疑是套接字文件描述符，有待确认）
//  其次是客户端的ip + port 以及最后一次互动时间（用于关闭超时连接）
//  最后使用shared_ptr管理一个conn对象。
struct ClientInfo {
  int fd;
  std::string ip_port;
  int64_t last_interaction = 0;
  std::shared_ptr<PikaClientConn> conn;
};

extern bool AddrCompare(const ClientInfo& lhs, const ClientInfo& rhs);
extern bool IdleCompare(const ClientInfo& lhs, const ClientInfo& rhs);

#endif
