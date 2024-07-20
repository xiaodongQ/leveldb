// Copyright (c) 2017 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_EXPORT_H_
#define STORAGE_LEVELDB_INCLUDE_EXPORT_H_

#if !defined(LEVELDB_EXPORT)

#if defined(LEVELDB_SHARED_LIBRARY)
    #if defined(_WIN32)

        #if defined(LEVELDB_COMPILE_LIBRARY)
            #define LEVELDB_EXPORT __declspec(dllexport)
        #else
            #define LEVELDB_EXPORT __declspec(dllimport)
        #endif  // defined(LEVELDB_COMPILE_LIBRARY)

    #else  // defined(_WIN32)
        #if defined(LEVELDB_COMPILE_LIBRARY)
            /* `__attribute__((visibility("default")))`是 GCC 和 Clang 编译器的一个特殊语法，用于控制库中符号（函数、变量等）的可见性。
                    `visibility("default")` 表示定义的符号是默认可见的，这意味着这些符号可以被其他的共享库或可执行文件访问。
               在构建共享库（动态链接库）时，编译器默认只导出某些符号。为了提高性能和安全性，开发者可以通过设置符号的可见性来控制哪些符号可以被外部访问。
                    **`default`**：指定为默认可见性，表示这个符号会被导出到共享库中，外部程序可以链接并使用这个符号。
                    **`hidden`**：与 `default` 相对，`hidden` 表示该符号不会被导出，外部程序无法使用。
                    **`internal`**：指示该符号仅对定义它的共享库中的其他符号可见，对外不可见，类似于 `hidden`。
            */
            #define LEVELDB_EXPORT __attribute__((visibility("default")))
        #else
            #define LEVELDB_EXPORT
        #endif
    #endif  // defined(_WIN32)

    #else  // defined(LEVELDB_SHARED_LIBRARY)
        #define LEVELDB_EXPORT
    #endif

#endif  // !defined(LEVELDB_EXPORT)

#endif  // STORAGE_LEVELDB_INCLUDE_EXPORT_H_
