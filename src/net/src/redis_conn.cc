// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "net/include/redis_conn.h"

#include <cstdlib>
#include <sstream>

#include <glog/logging.h>

#include "net/include/net_stats.h"
#include "pstd/include/pstd_string.h"
#include "pstd/include/xdebug.h"

extern std::unique_ptr<net::NetworkStatistic> g_network_statistic;

namespace net {

RedisConn::RedisConn(const int fd, const std::string& ip_port, Thread* thread, NetMultiplexer* net_mpx,
                     const HandleType& handle_type, const int rbuf_max_len)
    : NetConn(fd, ip_port, thread, net_mpx),
      handle_type_(handle_type),
      
      rbuf_max_len_(rbuf_max_len)
      {
  //  创建一个conn的时候，已经设置好了相应的cb
  RedisParserSettings settings;
  settings.DealMessage = ParserDealMessageCb;
  settings.Complete = ParserCompleteCb;
  redis_parser_.RedisParserInit(REDIS_PARSER_REQUEST, settings);
  redis_parser_.data = this;
}

RedisConn::~RedisConn() { free(rbuf_); }

ReadStatus RedisConn::ParseRedisParserStatus(RedisParserStatus status) {
  if (status == kRedisParserInitDone) {
    return kOk;
  } else if (status == kRedisParserHalf) {
    return kReadHalf;
  } else if (status == kRedisParserDone) {
    return kReadAll;
  } else if (status == kRedisParserError) {
    RedisParserError error_code = redis_parser_.get_error_code();
    switch (error_code) {
      case kRedisParserOk:
        return kReadError;
      case kRedisParserInitError:
        return kReadError;
      case kRedisParserFullError:
        return kFullError;
      case kRedisParserProtoError:
        return kParseError;
      case kRedisParserDealError:
        return kDealError;
      default:
        return kReadError;
    }
  } else {
    return kReadError;
  }
}

ReadStatus RedisConn::GetRequest() {
  ssize_t nread = 0;
  //  上一次读取数据的位置的后一位就是最新要读取数据的位置
  int next_read_pos = last_read_pos_ + 1;

  //   rbuf中剩余的空间
  int64_t remain = rbuf_len_ - next_read_pos;  // Remain buffer size
  int64_t new_size = 0;
  if (remain == 0) {
    //  如果剩余空间为0，那么增加rbuf的长度，增加REDIS_IOBUF_LEN
    new_size = rbuf_len_ + REDIS_IOBUF_LEN;
    remain += REDIS_IOBUF_LEN;
  } else if (remain < bulk_len_) {
    //  剩余的空间不够装下bulk_len,那么增加长度bulk_len
    new_size = next_read_pos + bulk_len_;
    //  且将remain置为bulk_len_
    remain = bulk_len_;
  }
  //  判断是否需要重新申请空间
  if (new_size > rbuf_len_) {
    if (new_size > rbuf_max_len_) {
      //   超出最大限制直接返回错误
      return kFullError;
    }
    //  否则重新reallocate空间，其用法非常灵活，可以在原来分配的地址上进行大小的变化
    rbuf_ = static_cast<char*>(realloc(rbuf_, new_size));  // NOLINT
    if (!rbuf_) {
      return kFullError;
    }
    rbuf_len_ = static_cast<int32_t>(new_size);
  }

  //  读取数据到rbuf中
  nread = read(fd(), rbuf_ + next_read_pos, remain);
  g_network_statistic->IncrRedisInputBytes(nread);
  if (nread == -1) {
    //  这些知识在网络系统编程中在复习
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      nread = 0;
      //  没有读到完整的数据
      return kReadHalf;  // HALF
    } else {
      // error happened, close client
      return kReadError;
    }
  } else if (nread == 0) {
    // client closed, close client
    return kReadClose;
  }
  // assert(nread > 0);
  //  更新已经读到的数据的位置
  last_read_pos_ += static_cast<int32_t>(nread);
  //  msg_peak记录最新数据的末尾
  msg_peak_ = last_read_pos_;
  //  代表一个命令的长度
  command_len_ += static_cast<int32_t> (nread);
  //  命令长度太长
  if (command_len_ >= rbuf_max_len_) {
    LOG(INFO) << "close conn command_len " << command_len_ << ", rbuf_max_len " << rbuf_max_len_;
    return kFullError;
  }
  int processed_len = 0;
  //  然后调用redis命令解析对象，将读到的命令作为参数传入
  RedisParserStatus ret = redis_parser_.ProcessInputBuffer(rbuf_ + next_read_pos, static_cast<int32_t>(nread), &processed_len);
  ReadStatus read_status = ParseRedisParserStatus(ret);
  if (read_status == kReadAll || read_status == kReadHalf) {
    if (read_status == kReadAll) {
      command_len_ = 0;
    }
    last_read_pos_ = -1;
    bulk_len_ = redis_parser_.get_bulk_len();
  }
  if (!response_.empty()) {
    set_is_reply(true);
  }
  return read_status;  // OK || HALF || FULL_ERROR || PARSE_ERROR
}

WriteStatus RedisConn::SendReply() {
  ssize_t nwritten = 0;
  size_t wbuf_len = response_.size();
  while (wbuf_len > 0) {
    //  直接一口气全部写回
    nwritten = write(fd(), response_.data() + wbuf_pos_, wbuf_len - wbuf_pos_);
    g_network_statistic->IncrRedisOutputBytes(nwritten);
    if (nwritten <= 0) {
      break;
    }
    wbuf_pos_ += nwritten;
    if (wbuf_pos_ == wbuf_len) {
      //  写完了所有的数据
      // Have sended all response data
      //  晴空response
      if (wbuf_len > DEFAULT_WBUF_SIZE) {
        std::string buf;
        buf.reserve(DEFAULT_WBUF_SIZE);
        response_.swap(buf);
      }
      response_.clear();

      wbuf_len = 0;
      wbuf_pos_ = 0;
    }
    //  否则继续找机会写
  }
  if (nwritten == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return kWriteHalf;
    } else {
      // Here we should close the connection
      return kWriteError;
    }
  }
  if (wbuf_len == 0) {
    return kWriteAll;
  } else {
    return kWriteHalf;
  }
}

int RedisConn::WriteResp(const std::string& resp) {
  //  在response中不断的append
  response_.append(resp);
  set_is_reply(true);
  return 0;
}

void RedisConn::TryResizeBuffer() {
  struct timeval now;
  gettimeofday(&now, nullptr);
  //  计算出空闲时间
  time_t idletime = now.tv_sec - last_interaction().tv_sec;
  if (rbuf_len_ > REDIS_MBULK_BIG_ARG && ((rbuf_len_ / (msg_peak_ + 1)) > 2 || idletime > 2)) {
    int new_size = ((last_read_pos_ + REDIS_IOBUF_LEN) / REDIS_IOBUF_LEN) * REDIS_IOBUF_LEN;
    if (new_size < rbuf_len_) {
      rbuf_ = static_cast<char*>(realloc(rbuf_, new_size));
      rbuf_len_ = new_size;
      LOG(INFO) << "Resize buffer to " << rbuf_len_ << ", last_read_pos_: " << last_read_pos_;
    }
    msg_peak_ = 0;
  }
}

void RedisConn::SetHandleType(const HandleType& handle_type) { handle_type_ = handle_type; }

HandleType RedisConn::GetHandleType() { return handle_type_; }

void RedisConn::ProcessRedisCmds(const std::vector<RedisCmdArgsType>& argvs, bool async, std::string* response) {}

void RedisConn::NotifyEpoll(bool success) {
  //  然后通知其所属的epoll模型对象（将ti放入queue，并写入一个字节。让其注册读和写事件
  NetItem ti(fd(), ip_port(), success ? kNotiEpolloutAndEpollin : kNotiClose);
  net_multiplexer()->Register(ti, true);
  //  此时要写回的一批结果都在conn的resp中，看WorkerThread如何处理
}

int RedisConn::ParserDealMessageCb(RedisParser* parser, const RedisCmdArgsType& argv) {
  auto conn = reinterpret_cast<RedisConn*>(parser->data);
  if (conn->GetHandleType() == HandleType::kSynchronous) {
    return conn->DealMessage(argv, &(conn->response_));
  } else {
    return 0;
  }
}


//  传递的参数为解析器和命令vector
int RedisConn::ParserCompleteCb(RedisParser* parser, const std::vector<RedisCmdArgsType>& argvs) {
  auto conn = reinterpret_cast<RedisConn*>(parser->data);
  bool async = conn->GetHandleType() == HandleType::kAsynchronous;
  // 所以redisparse的作用仅仅也是解析出一个命令数组。最后依然是使用conn中的ProcessRedisCmds，传入命令参数、同步or异步以及conn中的一个response（存放结果的地方的地址）
  conn->ProcessRedisCmds(argvs, async, &(conn->response_));
  return 0;
}

}  // namespace net
