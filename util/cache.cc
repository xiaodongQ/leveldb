// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/hash.h"
#include "util/mutexlock.h"

namespace leveldb {

Cache::~Cache() {}

namespace {

// LRU cache implementation
//
// Cache entries have an "in_cache" boolean indicating whether the cache has a
// reference on the entry.  The only ways that this can become false without the
// entry being passed to its "deleter" are via Erase(), via Insert() when
// an element with a duplicate key is inserted, or on destruction of the cache.
//
// The cache keeps two linked lists of items in the cache.  All items in the
// cache are in one list or the other, and never both.  Items still referenced
// by clients but erased from the cache are in neither list.  The lists are:
// - in-use:  contains the items currently referenced by clients, in no
//   particular order.  (This list is used for invariant checking.  If we
//   removed the check, elements that would otherwise be on this list could be
//   left as disconnected singleton lists.)
// - LRU:  contains the items not currently referenced by clients, in LRU order
// Elements are moved between these lists by the Ref() and Unref() methods,
// when they detect an element in the cache acquiring or losing its only
// external reference.

// An entry is a variable length heap-allocated structure.  Entries
// are kept in a circular doubly linked list ordered by access time.
// LRUHandle 是双向链表和哈希表的基本构成单位，同时也是数据条目缓存和操作的基本单元。
struct LRUHandle {
  void* value;
  // 释放 key,value 空间的用户回调
  void (*deleter)(const Slice&, void* value);
  // 用于 hashtable 中链表处理冲突，哈希表中用来处理冲突的链表节点与双向链表中的节点使用的是同一个数据结构（LRUHandle）
  LRUHandle* next_hash;
  // 用于双向链表中维护 LRU 顺序
  // 前驱节点
  LRUHandle* next;
  // 后继节点
  LRUHandle* prev;
  size_t charge;  // TODO(opt): Only allow uint32_t?
  size_t key_length;
  // 该 handle 是否在 cache table 中
  bool in_cache;     // Whether entry is in the cache.
  // 该 handle 被引用的次数
  uint32_t refs;     // References, including cache reference, if present.
  // key 的 hash 值，用于确定分片和快速比较
  uint32_t hash;     // Hash of key(); used for fast sharding and comparisons
  // key 的起始
  char key_data[1];  // Beginning of key

  Slice key() const {
    // next is only equal to this if the LRU handle is the list head of an
    // empty list. List heads never have meaningful keys.
    assert(next != this);

    return Slice(key_data, key_length);
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
class HandleTable {
 public:
  HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
  ~HandleTable() { delete[] list_; }

  LRUHandle* Lookup(const Slice& key, uint32_t hash) {
    return *FindPointer(key, hash);
  }

  // 哈希表插入节点
  LRUHandle* Insert(LRUHandle* h) {
    // 找到要插入节点需要的位置
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    *ptr = h;
    if (old == nullptr) {
      ++elems_;
      // 冲突节点超出阈值，需要rehash
      if (elems_ > length_) {
        // Since each cache entry is fairly large, we aim for a small
        // average linked list length (<= 1).
        Resize();
      }
    }
    return old;
  }

  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != nullptr) {
      *ptr = result->next_hash;
      --elems_;
    }
    return result;
  }

 private:
  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  // 用于rehash（resize）的判定，rehash后会按*2方式扩大
  uint32_t length_;
  uint32_t elems_;
  LRUHandle** list_;

  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  // 首先使用 hash 值通过位运算，定位到某个桶。然后在该桶中逐个遍历节点
  LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    // 根据hash找到slot槽位
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    // 遍历同slot下哈希冲突的节点，找一个可插入的位置
    while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
      // next_hash也是一个LRUHandle*基本处理单元，这里是hash冲突的节点链表
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }

  void Resize() {
    uint32_t new_length = 4;
    while (new_length < elems_) {
      new_length *= 2;
    }
    // 创建新hash链表
    LRUHandle** new_list = new LRUHandle*[new_length];
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    // 原来哈希表list_中的记录都hash转移调整到新哈希表
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      while (h != nullptr) {
        LRUHandle* next = h->next_hash;
        uint32_t hash = h->hash;
        LRUHandle** ptr = &new_list[hash & (new_length - 1)];
        h->next_hash = *ptr;
        *ptr = h;
        h = next;
        count++;
      }
    }
    assert(elems_ == count);
    // 释放原来的哈希表空间
    delete[] list_;
    // 指向扩容后的新哈希表
    list_ = new_list;
    length_ = new_length;
  }
};

// A single shard of sharded cache.
class LRUCache {
 public:
  LRUCache();
  ~LRUCache();

  // Separate from constructor so caller can easily make an array of LRUCache
  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  // Like Cache methods, but with an extra "hash" parameter.
  Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value,
                        size_t charge,
                        void (*deleter)(const Slice& key, void* value));
  Cache::Handle* Lookup(const Slice& key, uint32_t hash);
  void Release(Cache::Handle* handle);
  void Erase(const Slice& key, uint32_t hash);
  void Prune();
  size_t TotalCharge() const {
    MutexLock l(&mutex_);
    return usage_;
  }

 private:
  void LRU_Remove(LRUHandle* e);
  void LRU_Append(LRUHandle* list, LRUHandle* e);
  void Ref(LRUHandle* e);
  void Unref(LRUHandle* e);
  bool FinishErase(LRUHandle* e) EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Initialized before use.
  size_t capacity_;

  // mutex_ protects the following state.
  mutable port::Mutex mutex_;
  size_t usage_ GUARDED_BY(mutex_);

  // LevelDB 使用两个双向链表保存数据，缓存中的所有数据要么在一个链表中，要么在另一个链表中，但不可能同时存在于两个链表中。
  // 即下面的 lru链表 和 in-use链表
  // 哈希表中用来处理冲突的链表节点与双向链表中的节点使用的是同一个数据结构（LRUHandle）

  // Dummy head of LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // Entries have refs==1 and in_cache==true.
  // 所有已经不再为客户端使用的条目都放在 lru 链表中，该链表按最近使用时间有序，当容量不够用时，会驱逐此链表中最久没有被使用的条目
  // LRUHandle 是双向链表和哈希表的基本构成单位，同时也是数据条目缓存和操作的基本单元。
  LRUHandle lru_ GUARDED_BY(mutex_);

  // Dummy head of in-use list.
  // Entries are in use by clients, and have refs >= 2 and in_cache==true.
  // 所有正在被客户端使用的数据条目（an kv item）都存在该链表中，
  // 该链表是无序的，因为在容量不够时，此链表中的条目是一定不能够被驱逐的，因此也并不需要维持一个驱逐顺序。
  LRUHandle in_use_ GUARDED_BY(mutex_);

  // 哈希表索引
  HandleTable table_ GUARDED_BY(mutex_);
};

LRUCache::LRUCache() : capacity_(0), usage_(0) {
  // Make empty circular linked lists.
  lru_.next = &lru_;
  lru_.prev = &lru_;
  in_use_.next = &in_use_;
  in_use_.prev = &in_use_;
}

LRUCache::~LRUCache() {
  assert(in_use_.next == &in_use_);  // Error if caller has an unreleased handle
  for (LRUHandle* e = lru_.next; e != &lru_;) {
    LRUHandle* next = e->next;
    assert(e->in_cache);
    e->in_cache = false;
    assert(e->refs == 1);  // Invariant of lru_ list.
    Unref(e);
    e = next;
  }
}

void LRUCache::Ref(LRUHandle* e) {
  if (e->refs == 1 && e->in_cache) {  // If on lru_ list, move to in_use_ list.
    LRU_Remove(e);
    LRU_Append(&in_use_, e);
  }
  e->refs++;
}

void LRUCache::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  if (e->refs == 0) {  // Deallocate.
    assert(!e->in_cache);
    (*e->deleter)(e->key(), e->value);
    free(e);
  } else if (e->in_cache && e->refs == 1) {
    // No longer in use; move to lru_ list.
    LRU_Remove(e);
    LRU_Append(&lru_, e);
  }
}

void LRUCache::LRU_Remove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}

void LRUCache::LRU_Append(LRUHandle* list, LRUHandle* e) {
  // Make "e" newest entry by inserting just before *list
  // 链表操作，要新增的节点e，插入到list节点前面
  e->next = list;
  e->prev = list->prev;
  // 前驱后继节点均调整对应指向
  e->prev->next = e;
  e->next->prev = e;
}

Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  LRUHandle* e = table_.Lookup(key, hash);
  if (e != nullptr) {
    Ref(e);
  }
  return reinterpret_cast<Cache::Handle*>(e);
}

void LRUCache::Release(Cache::Handle* handle) {
  MutexLock l(&mutex_);
  Unref(reinterpret_cast<LRUHandle*>(handle));
}

Cache::Handle* LRUCache::Insert(const Slice& key, uint32_t hash, void* value,
                                size_t charge,
                                void (*deleter)(const Slice& key,
                                                void* value)) {
  MutexLock l(&mutex_);

  // 创建一个 LRUHandle 基本处理单元
  LRUHandle* e =
      reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->in_cache = false;
  // 传入引用也算一个计数，后面若在LRU缓存则再+1（即2）
  e->refs = 1;  // for the returned handle.
  std::memcpy(e->key_data, key.data(), key.size());

  if (capacity_ > 0) {
    e->refs++;  // for the cache's reference.
    e->in_cache = true;
    // 加到 in-use 对应的LRU缓存里，加到in_use_链表头部
    LRU_Append(&in_use_, e);
    usage_ += charge;
    // 新节点加到哈希表里去：`table_.Insert(e)`；
    // 并将当前节点从 _lru 缓存里移除（为便于区分，统一按in-use和_lru的说法来区分两个缓存链表）
      // 移除操作只是先修改前后节点指向，从链表里断开该节点，不做空间释放
    FinishErase(table_.Insert(e));
  } else {  // don't cache. (capacity_==0 is supported and turns off caching.)
    // next is read by key() in an assert, so it must be initialized
    e->next = nullptr;
  }
  // 使用量超出缓存总量，做LRU清理操作
  while (usage_ > capacity_ && lru_.next != &lru_) {
    LRUHandle* old = lru_.next;
    assert(old->refs == 1);
    bool erased = FinishErase(table_.Remove(old->key(), old->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }

  return reinterpret_cast<Cache::Handle*>(e);
}

// If e != nullptr, finish removing *e from the cache; it has already been
// removed from the hash table.  Return whether e != nullptr.
bool LRUCache::FinishErase(LRUHandle* e) {
  if (e != nullptr) {
    assert(e->in_cache);
    // 从双向链表里移除该节点，此处仅先修改前后节点指向，不做空间释放
    LRU_Remove(e);
    e->in_cache = false;
    usage_ -= e->charge;
    Unref(e);
  }
  return e != nullptr;
}

void LRUCache::Erase(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  FinishErase(table_.Remove(key, hash));
}

void LRUCache::Prune() {
  MutexLock l(&mutex_);
  while (lru_.next != &lru_) {
    LRUHandle* e = lru_.next;
    assert(e->refs == 1);
    bool erased = FinishErase(table_.Remove(e->key(), e->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }
}

static const int kNumShardBits = 4;
// 即16
static const int kNumShards = 1 << kNumShardBits;

// LRU缓存实现类，包含了 LRUCache 数组
class ShardedLRUCache : public Cache {
 private:
  // 由一组 LRUCache 组成，每个 LRUCache 作为一个分片，同时是一个加锁的粒度，他们都实现了 Cache 接口
  // LRUCache 数组，此处为16个
  LRUCache shard_[kNumShards];
  port::Mutex id_mutex_;
  uint64_t last_id_;

  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }

  static uint32_t Shard(uint32_t hash) { return hash >> (32 - kNumShardBits); }

 public:
  explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].SetCapacity(per_shard);
    }
  }
  ~ShardedLRUCache() override {}
  // 插入一个键值对（key，value）到缓存（cache）中
  // 返回指向该键值对的句柄（handle），调用者在用完句柄后，需要调用 this->Release(handle) 进行释放
  // 在键值对不再被使用时，键值对会被传入的 deleter 参数释放
  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice& key, void* value)) override {
    // 基于key计算hash
    const uint32_t hash = HashSlice(key);
    // 选择哪一个 LRUCache 来执行Insert操作
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }

  // 如果缓存中没有相应键（key），则返回 nullptr；否则返回指向对应键值对的句柄（Handle）。
  // 调用者用完句柄后，要记得调用 this->Release(handle) 进行释放
  Handle* Lookup(const Slice& key) override {
    // 查找也是跟上面一样，key算hash，然后选择某个LRUCache进行处理
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Lookup(key, hash);
  }

  // 释放 Insert/Lookup 函数返回的句柄
  void Release(Handle* handle) override {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    shard_[Shard(h->hash)].Release(handle);
  }

  // 如果缓存中包含给定键所指向的条目，则删除
  // 需要注意的是，只有在所有持有该条目句柄都释放时，该条目所占空间才会真正被释放
  void Erase(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    shard_[Shard(hash)].Erase(key, hash);
  }

  // 获取句柄中的值，类型为 void*（表示任意用户自定义类型）
  void* Value(Handle* handle) override {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }

  // 返回一个自增的数值 id。当一个缓存实例由多个客户端共享时，
  // 为了避免多个客户端的键冲突，每个客户端可能想获取一个独有的 id，并将其作为键的前缀。类似于给每个客户端一个单独的命名空间。
  uint64_t NewId() override {
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }

  // 驱逐全部没有被使用的数据条目
  void Prune() override {
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].Prune();
    }
  }

  // 返回当前缓存中所有数据条目所占容量总和的一个预估
  size_t TotalCharge() const override {
    size_t total = 0;
    for (int s = 0; s < kNumShards; s++) {
      total += shard_[s].TotalCharge();
    }
    return total;
  }
};

}  // end anonymous namespace

// 传入entries（即990）
Cache* NewLRUCache(size_t capacity) { return new ShardedLRUCache(capacity); }

}  // namespace leveldb
