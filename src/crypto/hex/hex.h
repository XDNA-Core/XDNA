// Copyright (c) 2017-2018 The XDNA Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEXHASH_H
#define HEXHASH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void hex_hash(const void* data, size_t len, void* out);

#ifdef __cplusplus
}
#endif

#endif // HEXHASH_H
