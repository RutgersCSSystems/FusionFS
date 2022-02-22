// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef DEVFS_STORAGE_LEVELDB_UTIL_CRC32C_H_
#define DEVFS_STORAGE_LEVELDB_UTIL_CRC32C_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Return the crc32c of concat(A, data[0,n-1]) where init_crc is the
// crc32c of some string A.  Extend() is often used to maintain the
// crc32c of a stream of data.
extern uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

// Return the crc32c of data[0,n-1]
uint32_t Value(const char* data, size_t n); 

// Return a masked representation of crc.
//
// Motivation: it is problematic to compute the CRC of a string that
// contains embedded CRCs.  Therefore we recommend that CRCs stored
// somewhere (e.g., in files) should be masked before being stored.
uint32_t Mask(uint32_t crc);

// Return the crc whose masked representation is masked_crc.
uint32_t Unmask(uint32_t masked_crc); 

uint32_t DecodeFixed32(const char* ptr); 

void EncodeFixed32(char* buf, uint32_t value); 

#endif  // STORAGE_LEVELDB_UTIL_CRC32C_H_
