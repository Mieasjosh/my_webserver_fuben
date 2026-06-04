/******************************************************************************
 * md5.cpp — MD5 消息摘要算法实现（RFC 1321）
 *
 * 参考：RFC 1321 "The MD5 Message-Digest Algorithm" by Ronald L. Rivest
 *
 * 算法简介：
 *   MD5 将任意长度的消息压缩为 128 位（16 字节）的"指纹"。
 *   输入被分成 512 bit（64 字节）的块，每个块经过 4 轮 × 16 步 = 64 步的
 *   非线性变换，每步涉及一个 32 位字的置换、移位和加法。
 *
 * 安全性说明：
 *   MD5 已不再适合安全用途（2004 年被发现碰撞攻击），但对于本项目的用途
 *   ——校验文件传输完整性——完全足够。碰撞攻击需要攻击者精心构造两个不同的
 *   文件使其 MD5 相同，而这里攻击者只控制网络传输，无法同时控制磁盘上的文件。
 *
 * 本文件实现内容：
 *   1. MD5_Init   — 初始化 A/B/C/D 寄存器和计数器
 *   2. MD5_Update — 接收数据，攒够 64 字节做一次块压缩
 *   3. MD5_Final  — 填充剩余数据，输出 16 字节摘要
 *   4. md5_file   — 上层封装：打开文件 → 8KB 分块读取 → 计算 → 转 hex 字符串
 ******************************************************************************/

#include "md5.h"

#include <stdio.h>    // fopen, fread, fclose
#include <string.h>   // memset, memcpy

// ---------------------------------------------------------------------------
// 基础宏定义
// ---------------------------------------------------------------------------

// 按 RFC 1321 定义的四个非线性函数（每轮用一个）
#define F(x, y, z)  (((x) & (y)) | ((~x) & (z)))          // 第一轮：条件选择
#define G(x, y, z)  (((x) & (z)) | ((y) & (~z)))           // 第二轮：反向条件选择
#define H(x, y, z)  ((x) ^ (y) ^ (z))                       // 第三轮：奇偶校验
#define I(x, y, z)  ((y) ^ ((x) | (~z)))                    // 第四轮：复杂的非线性

// 循环左移（32 位）：将 x 向左旋转 n 位
// 如 ROTATE_LEFT(0x12345678, 4) → 0x23456781
#define ROTATE_LEFT(x, n)  (((x) << (n)) | ((x) >> (32 - (n))))

// ---------------------------------------------------------------------------
// FF / GG / HH / II —— 每轮中一步的变换宏
// ---------------------------------------------------------------------------
// 每一步做：a = b + ROTATE_LEFT(a + 本轮函数(b,c,d) + X[k] + T[i], s)
// 其中：
//   a,b,c,d  — 四个 32 位寄存器
//   X[k]     — 当前 512 位块中的第 k 个 32 位字（共 16 个字，编号 0~15）
//   T[i]     — 预计算的常量表（sin 函数值取整），共 64 个
//   s        — 左移位数，每步不同

#define FF(a, b, c, d, x, s, ac)  {                         \
    (a) += F((b), (c), (d)) + (x) + (uint32_t)(ac);        \
    (a)  = ROTATE_LEFT((a), (s));                            \
    (a) += (b);                                              \
}

#define GG(a, b, c, d, x, s, ac)  {                         \
    (a) += G((b), (c), (d)) + (x) + (uint32_t)(ac);        \
    (a)  = ROTATE_LEFT((a), (s));                            \
    (a) += (b);                                              \
}

#define HH(a, b, c, d, x, s, ac)  {                         \
    (a) += H((b), (c), (d)) + (x) + (uint32_t)(ac);        \
    (a)  = ROTATE_LEFT((a), (s));                            \
    (a) += (b);                                              \
}

#define II(a, b, c, d, x, s, ac)  {                         \
    (a) += I((b), (c), (d)) + (x) + (uint32_t)(ac);        \
    (a)  = ROTATE_LEFT((a), (s));                            \
    (a) += (b);                                              \
}

// ===========================================================================
// 辅助函数：将 64 字节的数据块编码为 16 个 32 位小端字
// ===========================================================================
// MD5 以 32 位字为单位处理，需要把 64 字节的字节数组转为 16 个 uint32_t。
// MD5 使用 little-endian 字节序编码：
//   字节 [0x01, 0x02, 0x03, 0x04] → uint32_t 0x04030201
//
// 为什么不用直接指针强转？
//   ① 跨平台兼容（不依赖 CPU 字节序）
//   ② 当输入缓冲区不是 4 字节对齐时，直接解引用 uint32_t* 在某些 CPU（ARM）上会崩溃
static void encode_32bit_words(uint32_t *output, const unsigned char *input, size_t len)
{
    size_t i, j;
    for (i = 0, j = 0; j < len; i++, j += 4)
    {
        output[i] = ((uint32_t)input[j])
                  | ((uint32_t)input[j + 1] << 8)
                  | ((uint32_t)input[j + 2] << 16)
                  | ((uint32_t)input[j + 3] << 24);
    }
}

// ===========================================================================
// 辅助函数：把 16 字节 raw digest 转为 32 字符小写 hex 字符串
// ===========================================================================
// 每个字节用两个字符表示（高 4 位 + 低 4 位），例如 0x1A → "1a"
static void digest_to_hex(const unsigned char digest[16], char hex_output[33])
{
    for (int i = 0; i < 16; i++)
    {
        // snprintf 写入 "XX"（两个字符 + '\0'）
        // hex_output + i*2 = 当前写入位置，3 = 每次最多写 2 字符 + '\0'
        snprintf(hex_output + i * 2, 3, "%02x", digest[i]);
    }
    hex_output[32] = '\0';  // 确保结尾有终止符
}

// ===========================================================================
// 核心函数：对 512 bit（64 字节）数据块做 MD5 压缩
// ===========================================================================
// 这是 MD5 算法的核心——每一轮 16 步，共 4 轮，变换 4 个状态寄存器。
//
// 参数说明：
//   state[4] — 当前 A/B/C/D 值，函数调用后会被更新
//   block[64] — 512 bit 数据块
//
// 每轮的 X 索引（k）的选取规律：
//   第 1 轮（FF）：k = i                    （0, 1, 2, ..., 15 依次）
//   第 2 轮（GG）：k = (5 * i + 1) % 16     （打乱顺序）
//   第 3 轮（HH）：k = (3 * i + 5) % 16     （另一种打乱）
//   第 4 轮（II）：k = (7 * i) % 16          （再一种打乱）
//   这样设计是为了让输入字的每一位都多次参与不同的变换路径。
//
// 每轮的左移位数（s）也是固定序列，见下方数组。
static void md5_transform(uint32_t state[4], const unsigned char block[64])
{
    // ---- 1. 保存当前状态（本轮结束后加到结果上） ----
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    // ---- 2. 把 64 字节块解码为 16 个 32 位小端字 ----
    uint32_t x[16];
    encode_32bit_words(x, block, 64);

    // ---- 3. 四轮 × 16 步 = 64 步变换 ----

    // 第一轮：使用 F(x,y,z) = (x & y) | (~x & z)
    // 这是一个条件函数：如果 x 为 1 则选 y，否则选 z
    FF(a, b, c, d, x[ 0],  7, 0xd76aa478);   // 步  1
    FF(d, a, b, c, x[ 1], 12, 0xe8c7b756);   // 步  2
    FF(c, d, a, b, x[ 2], 17, 0x242070db);   // 步  3
    FF(b, c, d, a, x[ 3], 22, 0xc1bdceee);   // 步  4
    FF(a, b, c, d, x[ 4],  7, 0xf57c0faf);   // 步  5
    FF(d, a, b, c, x[ 5], 12, 0x4787c62a);   // 步  6
    FF(c, d, a, b, x[ 6], 17, 0xa8304613);   // 步  7
    FF(b, c, d, a, x[ 7], 22, 0xfd469501);   // 步  8
    FF(a, b, c, d, x[ 8],  7, 0x698098d8);   // 步  9
    FF(d, a, b, c, x[ 9], 12, 0x8b44f7af);   // 步 10
    FF(c, d, a, b, x[10], 17, 0xffff5bb1);   // 步 11
    FF(b, c, d, a, x[11], 22, 0x895cd7be);   // 步 12
    FF(a, b, c, d, x[12],  7, 0x6b901122);   // 步 13
    FF(d, a, b, c, x[13], 12, 0xfd987193);   // 步 14
    FF(c, d, a, b, x[14], 17, 0xa679438e);   // 步 15
    FF(b, c, d, a, x[15], 22, 0x49b40821);   // 步 16

    // 第二轮：使用 G(x,y,z) = (x & z) | (y & ~z)
    GG(a, b, c, d, x[ 1],  5, 0xf61e2562);   // 步 17
    GG(d, a, b, c, x[ 6],  9, 0xc040b340);   // 步 18
    GG(c, d, a, b, x[11], 14, 0x265e5a51);   // 步 19
    GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa);   // 步 20
    GG(a, b, c, d, x[ 5],  5, 0xd62f105d);   // 步 21
    GG(d, a, b, c, x[10],  9, 0x02441453);   // 步 22
    GG(c, d, a, b, x[15], 14, 0xd8a1e681);   // 步 23
    GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8);   // 步 24
    GG(a, b, c, d, x[ 9],  5, 0x21e1cde6);   // 步 25
    GG(d, a, b, c, x[14],  9, 0xc33707d6);   // 步 26
    GG(c, d, a, b, x[ 3], 14, 0xf4d50d87);   // 步 27
    GG(b, c, d, a, x[ 8], 20, 0x455a14ed);   // 步 28
    GG(a, b, c, d, x[13],  5, 0xa9e3e905);   // 步 29
    GG(d, a, b, c, x[ 2],  9, 0xfcefa3f8);   // 步 30
    GG(c, d, a, b, x[ 7], 14, 0x676f02d9);   // 步 31
    GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);   // 步 32

    // 第三轮：使用 H(x,y,z) = x ^ y ^ z（异或）
    HH(a, b, c, d, x[ 5],  4, 0xfffa3942);   // 步 33
    HH(d, a, b, c, x[ 8], 11, 0x8771f681);   // 步 34
    HH(c, d, a, b, x[11], 16, 0x6d9d6122);   // 步 35
    HH(b, c, d, a, x[14], 23, 0xfde5380c);   // 步 36
    HH(a, b, c, d, x[ 1],  4, 0xa4beea44);   // 步 37
    HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9);   // 步 38
    HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60);   // 步 39
    HH(b, c, d, a, x[10], 23, 0xbebfbc70);   // 步 40
    HH(a, b, c, d, x[13],  4, 0x289b7ec6);   // 步 41
    HH(d, a, b, c, x[ 0], 11, 0xeaa127fa);   // 步 42
    HH(c, d, a, b, x[ 3], 16, 0xd4ef3085);   // 步 43
    HH(b, c, d, a, x[ 6], 23, 0x04881d05);   // 步 44
    HH(a, b, c, d, x[ 9],  4, 0xd9d4d039);   // 步 45
    HH(d, a, b, c, x[12], 11, 0xe6db99e5);   // 步 46
    HH(c, d, a, b, x[15], 16, 0x1fa27cf8);   // 步 47
    HH(b, c, d, a, x[ 2], 23, 0xc4ac5665);   // 步 48

    // 第四轮：使用 I(x,y,z) = y ^ (x | ~z)
    II(a, b, c, d, x[ 0],  6, 0xf4292244);   // 步 49
    II(d, a, b, c, x[ 7], 10, 0x432aff97);   // 步 50
    II(c, d, a, b, x[14], 15, 0xab9423a7);   // 步 51
    II(b, c, d, a, x[ 5], 21, 0xfc93a039);   // 步 52
    II(a, b, c, d, x[12],  6, 0x655b59c3);   // 步 53
    II(d, a, b, c, x[ 3], 10, 0x8f0ccc92);   // 步 54
    II(c, d, a, b, x[10], 15, 0xffeff47d);   // 步 55
    II(b, c, d, a, x[ 1], 21, 0x85845dd1);   // 步 56
    II(a, b, c, d, x[ 8],  6, 0x6fa87e4f);   // 步 57
    II(d, a, b, c, x[15], 10, 0xfe2ce6e0);   // 步 58
    II(c, d, a, b, x[ 6], 15, 0xa3014314);   // 步 59
    II(b, c, d, a, x[13], 21, 0x4e0811a1);   // 步 60
    II(a, b, c, d, x[ 4],  6, 0xf7537e82);   // 步 61
    II(d, a, b, c, x[11], 10, 0xbd3af235);   // 步 62
    II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb);   // 步 63
    II(b, c, d, a, x[ 9], 21, 0xeb86d391);   // 步 64

    // ---- 4. 更新状态：把本轮结果加到上一轮状态上 ----
    // 这是 MD5 的 Davies-Meyer 构造：state_new = state_old + compress(state_old, block)
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;

    // 清零 x[] 数组，防止敏感数据残留（虽然不是安全用途，但好的习惯）
    memset(x, 0, sizeof(x));
}

// ===========================================================================
// MD5_Init — 初始化 MD5 上下文
// ===========================================================================
// 设置 A/B/C/D 的初始值（RFC 1321 规定的魔数）：
//   A = 0x67452301
//   B = 0xEFCDAB89
//   C = 0x98BADCFE
//   D = 0x10325476
//
// 这些值是：
//   把 sin(1)~sin(4) 的绝对值 × 2^32 取整，再写成 little-endian 即得。
void MD5_Init(MD5_CTX *ctx)
{
    // 计数器清零：还没处理任何数据
    ctx->count[0] = 0;   // 低位字
    ctx->count[1] = 0;   // 高位字

    // 四个状态寄存器的初始魔数（little-endian 下的十六进制表示）
    ctx->state[0] = 0x67452301;   // A
    ctx->state[1] = 0xEFCDAB89;   // B
    ctx->state[2] = 0x98BADCFE;   // C
    ctx->state[3] = 0x10325476;   // D

    // 输入缓冲区清零（虽非必须，但避免脏数据）
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

// ===========================================================================
// MD5_Update — 喂入数据（可多次调用）
// ===========================================================================
// 核心逻辑：
//   1. 计算当前缓冲区中有多少字节还没处理（index = count / 8 % 64）
//   2. 把新数据拷贝到缓冲区，直到凑满 64 字节
//   3. 凑满一块（64 字节）就调用 md5_transform 压缩一次
//   4. 剩余不足 64 字节的数据留在缓冲区，等下次 Update 或 Final 处理
void MD5_Update(MD5_CTX *ctx, const unsigned char *input, size_t len)
{
    // ---- 1. 计算缓冲区中已有多少字节等待处理 ----
    // count[0] 是已处理的比特总数的低 32 位
    // (count[0] >> 3) 得到已处理的字节数，& 0x3F 相当于 % 64
    size_t index = (size_t)((ctx->count[0] >> 3) & 0x3F);

    // ---- 2. 更新总比特数 ----
    // MD5 按比特计数，用于最后的填充步骤
    uint32_t bits = (uint32_t)(len << 3);   // 新数据的比特数（len * 8）
    ctx->count[0] += bits;
    if (ctx->count[0] < bits)               // 进位：低 32 位溢出则高 32 位 + 1
        ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);  // len 的高位部分（len > 512MB 时才有）

    // ---- 3. 填补缓冲区并压缩 ----
    size_t part_len = 64 - index;   // 缓冲区还剩多少空间

    size_t i = 0;   // 已处理的字节数（在 if/else 外部声明，下面要复用）

    if (len >= part_len)
    {
        // 先填满已有的部分缓冲区
        memcpy(&ctx->buffer[index], input, part_len);
        md5_transform(ctx->state, ctx->buffer);   // 压缩满的 64 字节

        // 对于能凑整 64 字节的块，直接压缩，不需要经过缓冲区
        for (i = part_len; i + 63 < len; i += 64)
        {
            md5_transform(ctx->state, &input[i]);
        }

        index = 0;   // 缓冲区位置重置
    }
    // 注意：如果 len < part_len，说明新数据连一个缓冲区空位都填不满，
    //       此时 i 保持为 0，下面 memcpy 会把新数据追加到缓冲区尾部。

    // ---- 4. 剩余数据存入缓冲区，等下次处理 ----
    memcpy(&ctx->buffer[index], &input[i], len - i);
}

// ===========================================================================
// MD5_Final — 终结计算，输出 16 字节摘要
// ===========================================================================
// 填充（padding）规则（RFC 1321）：
//   1. 在消息末尾追加一个 0x80 字节（二进制 10000000）
//   2. 追加 0x00 直到总长度 ≡ 448 mod 512（即差 8 字节满 512 bit）
//   3. 最后 8 字节写入原始消息的比特长度（little-endian）
//
// 为什么填充到 448 mod 512？
//   最后一轮压缩时，64 字节 = 448 bit 数据 + 64 bit 长度 = 512 bit 完整块
void MD5_Final(unsigned char digest[16], MD5_CTX *ctx)
{
    // ---- 1. 计算填充起始位置（基于当前已处理的比特数） ----
    // index = 当前缓冲区中已有多少字节
    size_t index = (size_t)((ctx->count[0] >> 3) & 0x3F);

    // ---- 2. 确定填充长度 ----
    // pad_len 的范围是 1 ~ 64 字节
    //   - 如果 index < 56：在当前位置填满到 56 字节，pad_len = 56 - index
    //   - 如果 index >= 56：需要再开一个新块，pad_len = 64 + 56 - index = 120 - index
    size_t pad_len = (index < 56) ? (56 - index) : (120 - index);

    // ---- 3. 构造填充数据 ----
    // 填充格式：0x80 + 一串 0x00 + 8 字节原始长度
    // PADDING[0] = 0x80, PADDING[1]~PADDING[63] = 0x00
    static const unsigned char PADDING[64] = {
        0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    // ---- 4. 写入填充数据 ----
    MD5_Update(ctx, PADDING, pad_len);

    // ---- 5. 写入 64 位的原始消息比特长度（little-endian） ----
    // 用 8 字节表示：前 4 字节 = count[0]（低 32 位），后 4 字节 = count[1]（高 32 位）
    unsigned char bits[8];
    {
        // 手动编码为 little-endian（不依赖 CPU 字节序）
        bits[0] = (unsigned char)( ctx->count[0]        & 0xFF);
        bits[1] = (unsigned char)((ctx->count[0] >>  8) & 0xFF);
        bits[2] = (unsigned char)((ctx->count[0] >> 16) & 0xFF);
        bits[3] = (unsigned char)((ctx->count[0] >> 24) & 0xFF);
        bits[4] = (unsigned char)( ctx->count[1]        & 0xFF);
        bits[5] = (unsigned char)((ctx->count[1] >>  8) & 0xFF);
        bits[6] = (unsigned char)((ctx->count[1] >> 16) & 0xFF);
        bits[7] = (unsigned char)((ctx->count[1] >> 24) & 0xFF);
    }
    MD5_Update(ctx, bits, 8);

    // ---- 6. 输出最终摘要：把 4 个 32 位 state 编码为 16 字节（little-endian） ----
    // 每个 uint32_t 按 little-endian 拆成 4 个字节
    for (int i = 0; i < 4; i++)
    {
        digest[i * 4 + 0] = (unsigned char)( ctx->state[i]        & 0xFF);
        digest[i * 4 + 1] = (unsigned char)((ctx->state[i] >>  8) & 0xFF);
        digest[i * 4 + 2] = (unsigned char)((ctx->state[i] >> 16) & 0xFF);
        digest[i * 4 + 3] = (unsigned char)((ctx->state[i] >> 24) & 0xFF);
    }
}

// ===========================================================================
// md5_file — 计算文件的 MD5（一步到位）
// ===========================================================================
// 用于上传完成后验证文件完整性。
//
// 流程：
//   fopen（二进制读）→ 循环 fread（8KB 每次）→ MD5_Update 增量喂入
//   → MD5_Final 终结计算 → 转 32 字符小写 hex → 返回
//
// 为什么用 8192 字节作为块大小？
//   ① 太小（如 1KB）：系统调用次数过多，效率低
//   ② 太大（如 1MB）：栈上缓冲区太大，可能爆栈（本项目栈空间有限）
//   ③ 8192 = 128 × 64（MD5 块大小），正好是 128 个压缩块，且是常见的页大小倍数
std::string md5_file(const char *filepath)
{
    // 以二进制只读方式打开文件
    FILE *fp = fopen(filepath, "rb");
    if (fp == NULL)
    {
        return "";   // 文件不存在或打不开，返回空字符串表示失败
    }

    MD5_CTX ctx;
    MD5_Init(&ctx);

    // 8KB 栈缓冲区：大小适中，不会爆栈，且是 64 字节的整数倍
    unsigned char buf[8192];
    size_t n;

    // 循环读取文件，每次最多 8KB，逐块喂入 MD5_Update
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
        MD5_Update(&ctx, buf, n);
    }

    fclose(fp);

    // 终结计算，得到 16 字节原始摘要
    unsigned char raw_digest[16];
    MD5_Final(raw_digest, &ctx);

    // 转为 32 字符的小写 hex 字符串
    char hex[33];
    digest_to_hex(raw_digest, hex);

    return std::string(hex);
}
