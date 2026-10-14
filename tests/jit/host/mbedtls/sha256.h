// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <openssl/sha.h>
inline int mbedtls_sha256_ret(const unsigned char *in,size_t size,unsigned char *out,int is224) {
    if(is224) return -1;
    return SHA256(in,size,out) ? 0 : -1;
}
