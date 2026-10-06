/* test_rsa.c — 主机上验证 RSA 签名模块
 *
 * 验证方式刻意做成"两套独立实现交叉核对"：
 *   本程序用 C 签名 → 把签名交给 Python → Python 用 pow() 独立验签。
 * 两边不共用任何代码，所以能真正发现错误（共用代码的话，同一个理解错两次
 * 会互相抵消）。SHA-1 则直接对照公开的标准测试向量。
 */
#include "../src/rsa.h"
#include "../src/adbkey.h"

#include <stdio.h>
#include <string.h>

static void hex(const unsigned char *p, int n)
{
    int i;
    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

int main(void)
{
    unsigned char d[20], sig[256];
    unsigned char blob[524];
    char line[1024];
    const char *abc = "abc";
    const char *multi = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    int i, bl;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("SHA1 abc=");
    sha1((const unsigned char *)abc, 3, d);
    hex(d, 20);

    printf("SHA1 empty=");
    sha1((const unsigned char *)"", 0, d);
    hex(d, 20);

    printf("SHA1 multi=");
    sha1((const unsigned char *)multi, (int)strlen(multi), d);
    hex(d, 20);

    /* ADB 认证时手机下发的 token 是 20 字节随机数，这里用固定值测 */
    {
        const char *token = "0123456789abcdefghij";   /* 20 字节 */
        if (rsa_sign_sha1(ADB_KEY_N, ADB_KEY_D,
                          (const unsigned char *)token, 20, sig) != 0) {
            printf("SIGN_FAIL\n");
            return 1;
        }
        printf("TOKEN=");
        for (i = 0; i < 20; i++)
            printf("%02x", (unsigned char)token[i]);
        printf("\nSIG=");
        hex(sig, 256);

        /* 签名的也是 token 本身，Python 侧按同样规则复现 */
        printf("SIGNED_MSG=");
        for (i = 0; i < 20; i++)
            printf("%02x", (unsigned char)token[i]);
        printf("\n");
    }

    bl = adb_public_key_blob(ADB_KEY_N, ADB_KEY_E, blob, sizeof(blob));
    printf("PUBBLOB_LEN=%d\n", bl);
    if (bl == 524) {
        printf("PUBBLOB=");
        hex(blob, 524);
    } else {
        printf("PUBBLOB_FAIL\n");
    }

    /* 让 Python 侧能拿到模数，避免两处各写一份 */
    printf("N=");
    hex(ADB_KEY_N, 256);
    printf("E=%u\n", (unsigned)ADB_KEY_E);

    (void)line; (void)abc;
    return 0;
}
