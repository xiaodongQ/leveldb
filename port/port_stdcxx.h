// Copyright (c) 2018 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_PORT_PORT_STDCXX_H_
#define STORAGE_LEVELDB_PORT_PORT_STDCXX_H_

// port/port_config.h availability is automatically detected via __has_include
// in newer compilers. If LEVELDB_HAS_PORT_CONFIG_H is defined, it overrides the
// configuration detection.
#if defined(LEVELDB_HAS_PORT_CONFIG_H)

#if LEVELDB_HAS_PORT_CONFIG_H
#include "port/port_config.h"
#endif  // LEVELDB_HAS_PORT_CONFIG_H

#elif defined(__has_include)

#if __has_include("port/port_config.h")
#include "port/port_config.h"
#endif  // __has_include("port/port_config.h")

#endif  // defined(LEVELDB_HAS_PORT_CONFIG_H)

#if HAVE_CRC32C
#include <crc32c/crc32c.h>
#endif  // HAVE_CRC32C
#if HAVE_SNAPPY
#include <snappy.h>
#endif  // HAVE_SNAPPY
#if HAVE_ZSTD
#define ZSTD_STATIC_LINKING_ONLY  // For ZSTD_compressionParameters.
#include <zstd.h>
#endif  // HAVE_ZSTD

#include <cassert>
#include <condition_variable>  // NOLINT
#include <cstddef>
#include <cstdint>
#include <mutex>  // NOLINT
#include <string>

#include "port/thread_annotations.h"

namespace leveldb {
namespace port {

class CondVar;

// Thinly wraps std::mutex.
class LOCKABLE Mutex {
 public:
  Mutex() = default;
  ~Mutex() = default;

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  /* 
    `__attribute__` 是 GCC（GNU 编译器集合）提供的一种机制，用于设置函数、变量、类型或结构的特定属性。
    通过这些属性，你可以优化代码、生成警告或者改变默认的编译行为。
    一些常用属性：
      函数属性
        `aligned(alignment)`: 指定函数的对齐方式
        `noinline`: 禁止函数内联
        `packed`：表明函数的返回值或参数结构需要紧凑排列，不要对其成员进行对齐
      变量属性
        `packed`: 表明变量结构需要紧凑排列
        `section("section_name")`: 指定变量放置在特定的段中
        `visibility("visibility_type")`: 控制符号在共享库中的可见性
  */
  // 通过`__attribute__`来设置`exclusive_lock_function`属性
  // `exclusive_lock_function` 是 GCC 中的一个函数属性，用于指定函数将获取一个独占锁（exclusive lock），这通常是指一个不会被多个线程同时持有的锁。
  // `exclusive_lock_function` 属性告知编译器，当调用该函数时，函数会获取一个特定的锁。这有助于编译器进行静态分析，确保程序的线程安全性。
  /*

    设置属性（如 `exclusive_lock_function` 和 `unlock_function`）并不是强制性的，但它们确实可以为你的代码提供额外的好处，特别是在大型代码库和多线程环境中。
    以下是一些使用这些属性来为加锁和解锁函数提供标注的原因：
    1、提高代码的可读性和维护性，让其他开发者更容易理解代码
    2、静态分析和检查，这些属性能帮助编译器和静态分析工具更好地理解你的代码结构
    是否使用这些属性取决于你代码的复杂性和团队的开发规范。在简单的项目或小型团队中，手动管理锁的获取和释放可能已经足够。但在大型项目中，属性的使用能显著提高代码的可靠性和可维护性。
  */
  void Lock() EXCLUSIVE_LOCK_FUNCTION() { mu_.lock(); }
  // UNLOCK_FUNCTION会设置 unlock_function 属性
  void Unlock() UNLOCK_FUNCTION() { mu_.unlock(); }
  void AssertHeld() ASSERT_EXCLUSIVE_LOCK() {}

 private:
  friend class CondVar;
  std::mutex mu_;
};

// Thinly wraps std::condition_variable.
// 封装了 std::condition_variable，确保了线程安全的等待与通知机制。
/*
  `std::condition_variable`是C++11标准库中提供的一种线程同步机制，用于多线程环境下的条件变量。它允许一个线程等待某个条件的变化，并在条件满足时进行通知。
  `std::condition_variable`与互斥锁（`std::mutex`）配合使用，以确保线程间安全地修改可共享的状态。
*/
class CondVar {
 public:
  explicit CondVar(Mutex* mu) : mu_(mu) { assert(mu != nullptr); }
  ~CondVar() = default;

  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;

  void Wait() {
    // std::adopt_lock 定义时即加锁
    std::unique_lock<std::mutex> lock(mu_->mu_, std::adopt_lock);
    // 等待条件变量，直到其他线程通过Signal或SignalAll唤醒它
    // 在等待过程中，lock会被临时释放，允许其他线程访问被保护的资源
    cv_.wait(lock);
    // 调用lock.release()是在wait返回后释放锁的一种不常见方式。
    // 实际上，在此上下文中，由于std::unique_lock会在其作用域结束时自动释放锁，这里的release调用是冗余的
    // 一般让 std::unique_lock自动管理锁的生命周期即可，无需手动调用release
    lock.release();
  }
  // 唤醒一个正在等待该条件变量的线程，哪个线程被唤醒是不确定的，由实现决定。
  void Signal() { cv_.notify_one(); }
  // 唤醒所有等待该条件变量的线程
  void SignalAll() { cv_.notify_all(); }

 private:
  std::condition_variable cv_;
  Mutex* const mu_;
};

inline bool Snappy_Compress(const char* input, size_t length,
                            std::string* output) {
#if HAVE_SNAPPY
  output->resize(snappy::MaxCompressedLength(length));
  size_t outlen;
  snappy::RawCompress(input, length, &(*output)[0], &outlen);
  output->resize(outlen);
  return true;
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)output;
#endif  // HAVE_SNAPPY

  return false;
}

inline bool Snappy_GetUncompressedLength(const char* input, size_t length,
                                         size_t* result) {
#if HAVE_SNAPPY
  return snappy::GetUncompressedLength(input, length, result);
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)result;
  return false;
#endif  // HAVE_SNAPPY
}

inline bool Snappy_Uncompress(const char* input, size_t length, char* output) {
#if HAVE_SNAPPY
  return snappy::RawUncompress(input, length, output);
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif  // HAVE_SNAPPY
}

inline bool Zstd_Compress(int level, const char* input, size_t length,
                          std::string* output) {
#if HAVE_ZSTD
  // Get the MaxCompressedLength.
  size_t outlen = ZSTD_compressBound(length);
  if (ZSTD_isError(outlen)) {
    return false;
  }
  output->resize(outlen);
  ZSTD_CCtx* ctx = ZSTD_createCCtx();
  ZSTD_compressionParameters parameters =
      ZSTD_getCParams(level, std::max(length, size_t{1}), /*dictSize=*/0);
  ZSTD_CCtx_setCParams(ctx, parameters);
  outlen = ZSTD_compress2(ctx, &(*output)[0], output->size(), input, length);
  ZSTD_freeCCtx(ctx);
  if (ZSTD_isError(outlen)) {
    return false;
  }
  output->resize(outlen);
  return true;
#else
  // Silence compiler warnings about unused arguments.
  (void)level;
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif  // HAVE_ZSTD
}

inline bool Zstd_GetUncompressedLength(const char* input, size_t length,
                                       size_t* result) {
#if HAVE_ZSTD
  size_t size = ZSTD_getFrameContentSize(input, length);
  if (size == 0) return false;
  *result = size;
  return true;
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)result;
  return false;
#endif  // HAVE_ZSTD
}

inline bool Zstd_Uncompress(const char* input, size_t length, char* output) {
#if HAVE_ZSTD
  size_t outlen;
  if (!Zstd_GetUncompressedLength(input, length, &outlen)) {
    return false;
  }
  ZSTD_DCtx* ctx = ZSTD_createDCtx();
  outlen = ZSTD_decompressDCtx(ctx, output, outlen, input, length);
  ZSTD_freeDCtx(ctx);
  if (ZSTD_isError(outlen)) {
    return false;
  }
  return true;
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif  // HAVE_ZSTD
}

inline bool GetHeapProfile(void (*func)(void*, const char*, int), void* arg) {
  // Silence compiler warnings about unused arguments.
  (void)func;
  (void)arg;
  return false;
}

inline uint32_t AcceleratedCRC32C(uint32_t crc, const char* buf, size_t size) {
#if HAVE_CRC32C
  return ::crc32c::Extend(crc, reinterpret_cast<const uint8_t*>(buf), size);
#else
  // Silence compiler warnings about unused arguments.
  (void)crc;
  (void)buf;
  (void)size;
  return 0;
#endif  // HAVE_CRC32C
}

}  // namespace port
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_PORT_PORT_STDCXX_H_
