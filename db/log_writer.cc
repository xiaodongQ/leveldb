// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/log_writer.h"

#include <cstdint>

#include "leveldb/env.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {
namespace log {

static void InitTypeCrc(uint32_t* type_crc) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    char t = static_cast<char>(i);
    type_crc[i] = crc32c::Value(&t, 1);
  }
}

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {
  InitTypeCrc(type_crc_);
}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(dest_length % kBlockSize) {
  InitTypeCrc(type_crc_);
}

Writer::~Writer() = default;

Status Writer::AddRecord(const Slice& slice) {
  const char* ptr = slice.data();
  size_t left = slice.size();

  // Fragment the record if necessary and emit it.  Note that if slice
  // is empty, we still want to iterate once to emit a single
  // zero-length record
  Status s;
  bool begin = true;
  // 循环写 dest_（定义为`WritableFile* dest_;`）
  // 一条日志记录可能包含多个block，一个block包含一个或多个完整的chunk。
  // 可查看日志结构[示意图](https://leveldb-handbook.readthedocs.io/zh/latest/_images/journal.jpeg)
  do {
    // 日志文件中按照block进行划分，每个block的大小为32KiB，32KB对齐这是为了提升读取时的效率
    // kBlockSize默认为32KB，block_offset_在下面的 EmitPhysicalRecord 里会赋值 fragment_length+头长度，表示数据偏移
    const int leftover = kBlockSize - block_offset_;
    assert(leftover >= 0);
    if (leftover < kHeaderSize) {
      // 一个block里剩余不足写7字节头，则填充空字符
      // Switch to a new block
      if (leftover > 0) {
        // Fill the trailer (literal below relies on kHeaderSize being 7)
        static_assert(kHeaderSize == 7, "");
        // 小数据写buffer，大数据直接::write写盘
        dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", leftover));
      }
      // 32KB后重置偏移，重新写一个block
      block_offset_ = 0;
    }

    // 断言：block里剩余的空间肯定 >= 7字节，留空间给下面的写入 (block剩余空间即kBlockSize - block_offset_)
    // Invariant: we never leave < kHeaderSize bytes in a block.
    assert(kBlockSize - block_offset_ - kHeaderSize >= 0);

    // 剩余可以给数据用的空间（头占有的7字节预留好了）
    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    // 若要写的数据 < block剩余空间，写要写的数据长度
    // 若要写的数据 >= block剩余空间，写block剩余空间（可能只写个头，数据为0长度）
    const size_t fragment_length = (left < avail) ? left : avail;

    RecordType type;
    // 根据 写的数据 和 block剩余空间的关系，判断chunk所处位置是 开始/结束/中间/满
    const bool end = (left == fragment_length);
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    // 里面涉及组装日志结构：7字节大小的header + 数据(数据可能为0字节)
    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);
  return s;
}

Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr,
                                  size_t length) {
  assert(length <= 0xffff);  // Must fit in two bytes
  assert(block_offset_ + kHeaderSize + length <= kBlockSize);

  // 每个chunk包含了一个7字节大小的header，前4字节是该chunk的校验码，紧接的2字节是该chunk数据的长度，以及最后一个字节是该chunk的类型。
  // 可查看日志结构[示意图](https://leveldb-handbook.readthedocs.io/zh/latest/_images/journal.jpeg)
  // Format the header
  char buf[kHeaderSize];
  buf[4] = static_cast<char>(length & 0xff);
  buf[5] = static_cast<char>(length >> 8);
  buf[6] = static_cast<char>(t);

  // Compute the crc of the record type and the payload.
  // 计算 数据+类型 对应的 CRC
  uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
  crc = crc32c::Mask(crc);  // Adjust for storage
  EncodeFixed32(buf, crc);

  // Write the header and the payload
  // 写7字节大小的header 到 dest_，buffer够则只写buffer
  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) {
    // 写 数据 到 dest_，buffer够则只写buffer
    s = dest_->Append(Slice(ptr, length));
    if (s.ok()) {
      // 系统::write接口写磁盘（里面并不会调::flush）
      s = dest_->Flush();
    }
  }
  // 本次写入的 7 + 数据长度，下次继续再这个偏移基础上写
  block_offset_ += kHeaderSize + length;
  return s;
}

}  // namespace log
}  // namespace leveldb
