//===-- a.c -----------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
int __a_global = 1;

int a(int arg) {
    int result = arg + __a_global;
    return result; // Set file and line breakpoint inside a().
}

int aa(int arg1) {
    int result1 = arg1 - __a_global;
    return result1;
}
