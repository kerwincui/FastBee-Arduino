#pragma once
#include <cstdint>
#include <cstddef>
// Deterministic byte-sensitive SDK seam. These tests validate storage calling
// and verification behavior, not the mbedTLS cryptographic implementation.
inline bool migrationHashFails=false;
struct mbedtls_sha256_context {uint64_t hash=1469598103934665603ULL;size_t size=0;};
inline void mbedtls_sha256_init(mbedtls_sha256_context* c){*c={};}
inline void mbedtls_sha256_free(mbedtls_sha256_context*){}
inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context*,int){return migrationHashFails?-1:0;}
inline int mbedtls_sha256_update_ret(mbedtls_sha256_context* c,const unsigned char* bytes,size_t count){
    if(migrationHashFails)return -1;
    for(size_t i=0;i<count;++i){c->hash^=bytes[i];c->hash*=1099511628211ULL;}
    c->size+=count;return 0;
}
inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context* c,unsigned char* out){
    if(migrationHashFails)return -1;
    for(size_t i=0;i<32;++i){out[i]=static_cast<uint8_t>(c->hash>>((i%8)*8));c->hash^=c->size+i;c->hash*=1099511628211ULL;}
    return 0;
}
