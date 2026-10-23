// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal archive-only EVP adapter. Crypto primitives are pinned Mbed TLS;
// this does not expose OpenSSL or TLS functionality to other port components.
#pragma once
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <cstdlib>
#include <cstring>

struct EVP_MD { mbedtls_md_type_t type; };
struct EVP_MD_CTX { mbedtls_sha1_context sha1; mbedtls_sha256_context sha256; mbedtls_md_type_t type; };
static inline const EVP_MD* EVP_sha1() { static const EVP_MD value{MBEDTLS_MD_SHA1}; return &value; }
static inline const EVP_MD* EVP_sha256() { static const EVP_MD value{MBEDTLS_MD_SHA256}; return &value; }
static inline EVP_MD_CTX* EVP_MD_CTX_new() {
  auto* context=static_cast<EVP_MD_CTX*>(std::calloc(1,sizeof(EVP_MD_CTX)));
  if(context){mbedtls_sha1_init(&context->sha1);mbedtls_sha256_init(&context->sha256);}
  return context;
}
static inline void EVP_MD_CTX_free(EVP_MD_CTX* context) {
  if(!context)return;
  mbedtls_sha1_free(&context->sha1);mbedtls_sha256_free(&context->sha256);std::free(context);
}
static inline int EVP_DigestInit_ex(EVP_MD_CTX* context,const EVP_MD* type,void*) {
  if(!context||!type)return 0;
  context->type=type->type;
  return (context->type==MBEDTLS_MD_SHA1?mbedtls_sha1_starts_ret(&context->sha1):
    context->type==MBEDTLS_MD_SHA256?mbedtls_sha256_starts_ret(&context->sha256,0):-1)==0;
}
static inline int EVP_DigestUpdate(EVP_MD_CTX* context,const void* data,std::size_t size) {
  if(!context)return 0;
  const auto* bytes=static_cast<const unsigned char*>(data);
  return (context->type==MBEDTLS_MD_SHA1?mbedtls_sha1_update_ret(&context->sha1,bytes,size):
    context->type==MBEDTLS_MD_SHA256?mbedtls_sha256_update_ret(&context->sha256,bytes,size):-1)==0;
}
static inline int EVP_DigestFinal_ex(EVP_MD_CTX* context,unsigned char* digest,unsigned* size) {
  if(!context||!digest||!size)return 0;
  *size=context->type==MBEDTLS_MD_SHA1?20:32;
  return (context->type==MBEDTLS_MD_SHA1?mbedtls_sha1_finish_ret(&context->sha1,digest):
    context->type==MBEDTLS_MD_SHA256?mbedtls_sha256_finish_ret(&context->sha256,digest):-1)==0;
}
struct EVP_CIPHER {};
struct EVP_CIPHER_CTX { mbedtls_aes_context aes; unsigned char iv[16]; bool keyed; };
static inline const EVP_CIPHER* EVP_aes_256_cbc() { static const EVP_CIPHER value; return &value; }
static inline EVP_CIPHER_CTX* EVP_CIPHER_CTX_new() {
  auto* context=static_cast<EVP_CIPHER_CTX*>(std::calloc(1,sizeof(EVP_CIPHER_CTX)));
  if(context)mbedtls_aes_init(&context->aes);
  return context;
}
static inline void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX* context) {
  if(context){mbedtls_aes_free(&context->aes);std::memset(context->iv,0,sizeof context->iv);std::free(context);}
}
static inline int EVP_DecryptInit_ex(EVP_CIPHER_CTX* context,const EVP_CIPHER*,void*,const unsigned char* key,const unsigned char* iv) {
  if(!context)return 0;
  if(key){context->keyed=mbedtls_aes_setkey_dec(&context->aes,key,256)==0;if(!context->keyed)return 0;}
  if(iv)std::memcpy(context->iv,iv,16);
  return context->keyed;
}
static inline int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX* context,int enabled) { return context&&context->keyed&&!enabled; }
static inline int EVP_DecryptUpdate(EVP_CIPHER_CTX* context,unsigned char* out,int* size,const unsigned char* data,int bytes) {
  if(!context||!context->keyed||!size||bytes<0||bytes%16)return 0;
  *size=0;
  if(mbedtls_aes_crypt_cbc(&context->aes,MBEDTLS_AES_DECRYPT,static_cast<std::size_t>(bytes),context->iv,data,out))return 0;
  *size=bytes;return 1;
}
static inline int PKCS5_PBKDF2_HMAC(const char* password,int password_size,const unsigned char* salt,int salt_size,int rounds,
                                   const EVP_MD* type,int bytes,unsigned char* out) {
  if(!type||password_size<0||salt_size<0||rounds<1||bytes<0)return 0;
  const auto* info=mbedtls_md_info_from_type(type->type);
  if(!info)return 0;
  mbedtls_md_context_t context;mbedtls_md_init(&context);
  int result=mbedtls_md_setup(&context,info,1);
  if(!result)result=mbedtls_pkcs5_pbkdf2_hmac(&context,reinterpret_cast<const unsigned char*>(password),password_size,
                                          salt,salt_size,rounds,bytes,out);
  mbedtls_md_free(&context);return result==0;
}
