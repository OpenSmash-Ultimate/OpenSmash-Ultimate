/* tests/port/common: stand-in for <openssl/evp.h>, the declarations plat_posix.c names, so the tests/port suites build
 * without OpenSSL's headers (a device or Mac without the -dev package). No test calls them: the suites link abort() stubs. */
#ifndef TP_STUB_OPENSSL_EVP_H
#define TP_STUB_OPENSSL_EVP_H
#include <stddef.h>
typedef struct evp_md_st EVP_MD;
typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct engine_st ENGINE;
const EVP_MD *EVP_md5(void);
const EVP_MD *EVP_sha1(void);
const EVP_MD *EVP_sha256(void);
EVP_MD_CTX *EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX *c);
int EVP_DigestInit_ex(EVP_MD_CTX *c, const EVP_MD *md, ENGINE *e);
int EVP_DigestUpdate(EVP_MD_CTX *c, const void *d, size_t n);
int EVP_DigestFinal_ex(EVP_MD_CTX *c, unsigned char *md, unsigned int *n);
/* EVP_MD_CTX_get_size is a macro in OpenSSL 3 (not an exported symbol; 3.0 spells it with the two functions below, later 3.x with
 * EVP_MD_CTX_get_size_ex): the long form exists in every 3.x, so a build against a real libcrypto.so.3 links. */
const EVP_MD *EVP_MD_CTX_get0_md(const EVP_MD_CTX *c);
int EVP_MD_get_size(const EVP_MD *md);
#define EVP_MD_CTX_get_size(c) EVP_MD_get_size(EVP_MD_CTX_get0_md(c))
int EVP_MD_CTX_copy_ex(EVP_MD_CTX *out, const EVP_MD_CTX *in);
#endif
