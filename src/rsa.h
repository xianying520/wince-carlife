/* rsa.h — ADB 认证所需的 RSA-2048 / SHA-1 签名（自足实现）
 *
 * 为什么自己写：ADB 协议要求车机在认证阶段对手机下发的随机 token 做 RSA
 * 签名。WinCE 上没有可用的密码学库，而 EasyConnected 是靠静态链接 OpenSSL
 * 才做到的。我们只需要「用一把固定私钥签名」这一个功能，不值得为此搬进
 * 一整个 OpenSSL，所以自己实现最小可用的一套。
 *
 * 只需要签名，不需要验签、不需要生成密钥 —— 私钥是预先算好硬编码进来的。
 */
#ifndef RSA_H
#define RSA_H

/* SHA-1 摘要，20 字节 */
void sha1(const unsigned char *data, int len, unsigned char out[20]);

/* 用固定 2048 位私钥做 PKCS#1 v1.5 (SHA-1) 签名。
 *    n, d : 各 256 字节，大端
 *    sig  : 输出 256 字节
 * 返回 0 成功，-1 失败。
 * 注意：签名的是 msg 本身，内部会先做 SHA-1。 */
int rsa_sign_sha1(const unsigned char *n, const unsigned char *d,
                  const unsigned char *msg, int msglen,
                  unsigned char *sig);

/* 按 ADB 的二进制格式生成公钥 blob（524 字节），AUTH 包的 ADB_PUBKEY 用它。
 * 返回写入字节数（524），cap 不足返回 -1。 */
int adb_public_key_blob(const unsigned char *n, unsigned int e,
                        unsigned char *out, int cap);

#endif
