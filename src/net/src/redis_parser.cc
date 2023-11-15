// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "net/include/redis_parser.h"

#include <cassert> /* assert */

#include <glog/logging.h>

#include "pstd/include/pstd_string.h"
#include "pstd/include/xdebug.h"

namespace net {

static bool IsHexDigit(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

static int HexDigitToInt32(char ch) {
  if (ch <= '9' && ch >= '0') {
    return ch - '0';
  } else if (ch <= 'F' && ch >= 'A') {
    return ch - 'A';
  } else if (ch <= 'f' && ch >= 'a') {
    return ch - 'a';
  } else {
    return 0;
  }
}

static int split2args(const std::string& req_buf, RedisCmdArgsType& argv) {
  const char* p = req_buf.data();
  std::string arg;

  while (true) {
    // skip blanks
    while ((*p != 0) && (isspace(*p) != 0)) {
      p++;
    }
    if (*p != 0) {
      // get a token
      int inq = 0;   // set to 1 if we are in "quotes"
      int insq = 0;  // set to 1 if we are in 'single quotes'
      int done = 0;

      arg.clear();
      while (done == 0) {
        if (inq != 0) {
          if (*p == '\\' && *(p + 1) == 'x' && IsHexDigit(*(p + 2)) && IsHexDigit(*(p + 3))) {
            char byte = static_cast<char>(HexDigitToInt32(*(p + 2)) * 16 + HexDigitToInt32(*(p + 3)));
            arg.append(1, byte);
            p += 3;
          } else if (*p == '\\' && (*(p + 1) != 0)) {
            char c;

            p++;
            switch (*p) {
              case 'n':
                c = '\n';
                break;
              case 'r':
                c = '\r';
                break;
              case 't':
                c = '\t';
                break;
              case 'b':
                c = '\b';
                break;
              case 'a':
                c = '\a';
                break;
              default:
                c = *p;
                break;
            }
            arg.append(1, c);
          } else if (*p == '"') {
            /* closing quote must be followed by a space or
             * nothing at all. */
            if ((*(p + 1) != 0) && (isspace(*(p + 1)) == 0)) {
              argv.clear();
              return -1;
            }
            done = 1;
          } else if (*p == 0) {
            // unterminated quotes
            argv.clear();
            return -1;
          } else {
            arg.append(1, *p);
          }
        } else if (insq != 0) {
          if (*p == '\\' && *(p + 1) == '\'') {
            p++;
            arg.append(1, '\'');
          } else if (*p == '\'') {
            /* closing quote must be followed by a space or
             * nothing at all. */
            if ((*(p + 1) != 0) && (isspace(*(p + 1)) == 0)) {
              argv.clear();
              return -1;
            }
            done = 1;
          } else if (*p == 0) {
            // unterminated quotes
            argv.clear();
            return -1;
          } else {
            arg.append(1, *p);
          }
        } else {
          switch (*p) {
            case ' ':
            case '\n':
            case '\r':
            case '\t':
            case '\0':
              done = 1;
              break;
            case '"':
              inq = 1;
              break;
            case '\'':
              insq = 1;
              break;
            default:
              // current = sdscatlen(current,p,1);
              arg.append(1, *p);
              break;
          }
        }
        if (*p != 0) {
          p++;
        }
      }
      argv.push_back(arg);
    } else {
      return 0;
    }
  }
}

int RedisParser::FindNextSeparators() {
  if (cur_pos_ > length_ - 1) {
    return -1;
  }
  int pos = cur_pos_;
  while (pos <= length_ - 1) {
    if (input_buf_[pos] == '\n') {
      return pos;
    }
    pos++;
  }
  return -1;
}

int RedisParser::GetNextNum(int pos, long* value) {
  assert(pos > cur_pos_);
  //     cur_pos_       pos
  //      |    ----------|
  //      |    |
  //      *3\r\n
  // [cur_pos_ + 1, pos - cur_pos_ - 2]
  if (pstd::string2int(input_buf_ + cur_pos_ + 1, pos - cur_pos_ - 2, value) != 0) {
    return 0;  // Success
  }
  return -1;  // Failed
}

RedisParser::RedisParser()
    : redis_type_(0), bulk_len_(-1), redis_parser_type_(REDIS_PARSER_REQUEST) {}

void RedisParser::SetParserStatus(RedisParserStatus status, RedisParserError error) {
  if (status == kRedisParserHalf) {
    CacheHalfArgv();
  }
  status_code_ = status;
  error_code_ = error;
}

void RedisParser::CacheHalfArgv() {
  std::string tmp(input_buf_ + cur_pos_, length_ - cur_pos_);
  half_argv_ = tmp;
  cur_pos_ = length_;
}

RedisParserStatus RedisParser::RedisParserInit(RedisParserType type, const RedisParserSettings& settings) {
  if (status_code_ != kRedisParserNone) {
    //  初始化的时候，解析器的初始状态应该是None
    SetParserStatus(kRedisParserError, kRedisParserInitError);
    return status_code_;
  }
  if (type != REDIS_PARSER_REQUEST && type != REDIS_PARSER_RESPONSE) {
    SetParserStatus(kRedisParserError, kRedisParserInitError);
    return status_code_;
  }
  redis_parser_type_ = type;
  parser_settings_ = settings;
  SetParserStatus(kRedisParserInitDone);
  return status_code_;
}

RedisParserStatus RedisParser::ProcessInlineBuffer() {
  int pos;
  int ret;
  pos = FindNextSeparators();
  if (pos == -1) {
    // change rbuf_len_ to length_
    if (length_ > REDIS_INLINE_MAXLEN) {
      SetParserStatus(kRedisParserError, kRedisParserFullError);
      return status_code_;
    } else {
      SetParserStatus(kRedisParserHalf);
      return status_code_;
    }
  }
  // args \r\n
  std::string req_buf(input_buf_ + cur_pos_, pos + 1 - cur_pos_);

  argv_.clear();
  ret = split2args(req_buf, argv_);
  cur_pos_ = pos + 1;

  if (ret == -1) {
    SetParserStatus(kRedisParserError, kRedisParserProtoError);
    return status_code_;
  }
  SetParserStatus(kRedisParserDone);
  return status_code_;
}

//  解析字节流可以分为一下部分：一个完整的命令中包含多个完整的参数，而一个完整的参数由一个完整的参数长度和一个完整的参数内容两部分组成
//  根据参数的值来判断当前的解析在一个字节流中的什么位置，然后将应该有的结果表达出来.这就是协议的作用。
RedisParserStatus RedisParser::ProcessMultibulkBuffer() {
  int pos = 0;
  if (multibulk_len_ == 0) {
    //  如果multibulk_len_是0，那么说明刚解析完一个完整的数据或者是第一次开始解析
    /* The client should have been reset */
    //  找的 \n
    pos = FindNextSeparators();
    if (pos != -1) {
      //  成功找到\n，按照协议进行解析，首先找到len
      if (GetNextNum(pos, &multibulk_len_) != 0) {
        // Protocol error: invalid multibulk length
        // 如果找到\n的情况下，没有解析出multibulk_len_，那么说明这数据有问题， 返回kRedisParserProtoError
        SetParserStatus(kRedisParserError, kRedisParserProtoError);
        return status_code_;
      }
      cur_pos_ = pos + 1;
      argv_.clear();
      if (cur_pos_ > length_ - 1) {
        SetParserStatus(kRedisParserHalf);
        return status_code_;//HALF
      }
    } else {
      SetParserStatus(kRedisParserHalf);
      return status_code_;  // HALF
    }
  }
  //  开始解析参数
  while (multibulk_len_ != 0) {
    if (bulk_len_ == -1) {
      //  同样找到\n
      pos = FindNextSeparators();
      if (pos != -1) {
        if (input_buf_[cur_pos_] != '$') {
          SetParserStatus(kRedisParserError, kRedisParserProtoError);
          return status_code_;  // PARSE_ERROR
        }

        //  bulk是代表其中一个完整的参数的长度
        if (GetNextNum(pos, &bulk_len_) != 0) {
          // Protocol error: invalid bulk length
          SetParserStatus(kRedisParserError, kRedisParserProtoError);
          return status_code_;
        }
        cur_pos_ = pos + 1;
      }
      if (pos == -1 || cur_pos_ > length_ - 1) {
        SetParserStatus(kRedisParserHalf);
        return status_code_;
      }
    }
    //  已经获得了一个参数的长度bulk_len
    if ((length_ - 1) - cur_pos_ + 1 < bulk_len_ + 2) {
      // Data not enough
      // 参数内容不完整
      break;
    } else {
      argv_.emplace_back(input_buf_ + cur_pos_, bulk_len_);
      cur_pos_ = static_cast<int32_t>(cur_pos_ + bulk_len_ + 2);
      bulk_len_ = -1;
      multibulk_len_--;
    }
  }

  if (multibulk_len_ == 0) {
    SetParserStatus(kRedisParserDone);
    return status_code_;  // OK
  } else {
    //  当参数内容不完整的情况下，也有可能跳出循环
    SetParserStatus(kRedisParserHalf);
    return status_code_;  // HALF
  }
}

void RedisParser::PrintCurrentStatus() {
  LOG(INFO) << "status_code " << status_code_ << " error_code " <<  error_code_;
  LOG(INFO) << "multibulk_len_ " << multibulk_len_ << "bulk_len " << bulk_len_ << " redis_type " << redis_type_ << " redis_parser_type " << redis_parser_type_;
  // for (auto& i : argv_) {
  //   UNUSED(i);
  //   log_info("parsed arguments: %s", i.c_str());
  // }
  LOG(INFO) << "cur_pos : " << cur_pos_;
  LOG(INFO) << "input_buf_ is clean ? " << (input_buf_ == nullptr);
  if (input_buf_) {
    LOG(INFO) << " input_buf " << input_buf_;
  }
  LOG(INFO) << "half_argv_ : " << half_argv_;
  LOG(INFO) << "input_buf len " << length_;
}

RedisParserStatus RedisParser::ProcessInputBuffer(const char* input_buf, int length, int* parsed_len) {
  //  解析器的状态只有这三种时正常的，其余的都是有问题的
  if (status_code_ == kRedisParserInitDone || status_code_ == kRedisParserHalf || status_code_ == kRedisParserDone) {
    // TODO(): AZ: avoid copy
    std::string tmp_str(input_buf, length);
    //  将这次传递进来的命令添加在之前不完整的命令后面
    input_str_ = half_argv_ + tmp_str;
    input_buf_ = input_str_.c_str();
    length_ = static_cast<int32_t>(length + half_argv_.size());

    //  初始化时，pikaClientConn中的解析器的redis_parser_type_就被设置为REDIS_PARSER_REQUEST
    if (redis_parser_type_ == REDIS_PARSER_REQUEST) {
      ProcessRequestBuffer();
    } else if (redis_parser_type_ == REDIS_PARSER_RESPONSE) {
      ProcessResponseBuffer();
    } else {
      SetParserStatus(kRedisParserError, kRedisParserInitError);
      return status_code_;
    }
    // cur_pos_ starts from 0, val of cur_pos_ is the parsed_len
    *parsed_len = cur_pos_;
    ResetRedisParser();
    // PrintCurrentStatus();
    return status_code_;
  }
  SetParserStatus(kRedisParserError, kRedisParserInitError);
  return status_code_;
}

// TODO(): AZ
RedisParserStatus RedisParser::ProcessResponseBuffer() {
  SetParserStatus(kRedisParserDone);
  return status_code_;
}

RedisParserStatus RedisParser::ProcessRequestBuffer() {
  RedisParserStatus ret;
  //  length是目前解析器中所有数据长度
  while (cur_pos_ <= length_ - 1) {
    if (redis_type_ == 0) {
      //  每当处理完一个完整的命令，那么redis_type就被置为0，
      //  然后根据一个完整命令的第一个字符决定redis_type_的类型
      if (input_buf_[cur_pos_] == '*') {
        redis_type_ = REDIS_REQ_MULTIBULK;
      } else {
        redis_type_ = REDIS_REQ_INLINE;
      }
    }

    if (redis_type_ == REDIS_REQ_INLINE) {
      ret = ProcessInlineBuffer();
      if (ret != kRedisParserDone) {
        return ret;
      }
    } else if (redis_type_ == REDIS_REQ_MULTIBULK) {
      ret = ProcessMultibulkBuffer();
      if (ret != kRedisParserDone) {  // FULL_ERROR || HALF || PARSE_ERROR
        return ret;
      }
    } else {
      // Unknown requeset type;
      return kRedisParserError;
    }
    if (!argv_.empty()) {
      //  可能同时解析出多条完整的命令
      argvs_.push_back(argv_);
      //  每解析出一条完整的命令都会调用一次DealMessage
      if (parser_settings_.DealMessage) {
        if (parser_settings_.DealMessage(this, argv_) != 0) {
          SetParserStatus(kRedisParserError, kRedisParserDealError);
          return status_code_;
        }
      }
    }
    argv_.clear();
    // Reset
    ResetCommandStatus();
  }
  //  当解析出这次可以解析的所有完整命令后，调用Complete函数
  if (parser_settings_.Complete) {
    if (parser_settings_.Complete(this, argvs_) != 0) {
      SetParserStatus(kRedisParserError, kRedisParserCompleteError);
      return status_code_;
    }
  }
  argvs_.clear();
  SetParserStatus(kRedisParserDone);
  return status_code_;  // OK
}

//  当解析出一个完整命令后，将相关的参数重新初始化
void RedisParser::ResetCommandStatus() {
  redis_type_ = 0;
  multibulk_len_ = 0;
  bulk_len_ = -1;
  half_argv_.clear();
}

//
void RedisParser::ResetRedisParser() {
  cur_pos_ = 0;
  input_buf_ = nullptr;
  input_str_.clear();
  length_ = 0;
}

}  // namespace net
