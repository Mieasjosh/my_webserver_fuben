/******************************************************************************
 * upload_handler.cpp — 上传状态管理器实现
 *
 * 本文件实现三阶段断点续传逻辑：
 *   第一阶段 init_upload    —— 创建/恢复上传任务，打开临时文件
 *   第二阶段 write_chunk    —— 把网络收到的数据块写入磁盘
 *   第三阶段 complete_upload —— fsync → MD5 校验 → rename
 *
 * 文件 I/O 说明：
 *   - 临时文件以 O_RDWR | O_CREAT 方式打开，不截断（方便断点续传）
 *   - 数据写入使用 pwrite() 而非 write()，因为 pwrite 自带偏移量参数，
 *     多线程同时写同一文件的不同位置时不需要 lseek + write 两步（避免竞态）
 *   - 完成时先 fsync() 再 close()，确保数据真正落到磁盘再算 MD5
 *
 * 错误处理策略：
 *   - 任何失败都通过 out_error 出参返回中文描述
 *   - 失败时临时文件保留（以便重新尝试），但 map 中记录可能被移除
 *   - cleanup_stale 是兜底机制：超过 1 小时未完成的上传自动清理
 ******************************************************************************/

#include "upload_handler.h"
#include "md5.h"

#include <stdio.h>       // snprintf
#include <string.h>      // memset
#include <fcntl.h>       // open, O_RDWR, O_CREAT
#include <unistd.h>      // close, pwrite, fsync, unlink
#include <sys/stat.h>    // stat, struct stat
#include <time.h>         // time, time_t
#include <errno.h>        // errno

// ===========================================================================
// 辅助函数：把错误号转成可读字符串
// ===========================================================================
// 在 Linux 上可以用 strerror_r()，但这里用简单的描述文本避免线程安全问题
static const char *err_to_str(int err)
{
    switch (err)
    {
        case EACCES:       return "权限不足";
        case EEXIST:       return "文件已存在";
        case ENOENT:       return "文件或目录不存在";
        case ENOSPC:       return "磁盘空间不足";
        case EIO:          return "磁盘 I/O 错误";
        case EROFS:        return "文件系统只读";
        case ENAMETOOLONG: return "文件名过长";
        default:           return "未知错误";
    }
}

// ===========================================================================
// 辅助函数：创建目录（如果不存在）
// ===========================================================================
// mkdir 第二个参数是权限位（八进制），0755 = rwxr-xr-x
// 如果目录已存在（errno == EEXIST），不报错
// 如果创建失败（权限不足等），返回 false
static bool ensure_directory(const char *path)
{
    if (mkdir(path, 0755) == 0)
        return true;           // 创建成功
    if (errno == EEXIST)
        return true;           // 目录本来就存在，也算成功
    return false;              // 其他错误（如权限不足）
}

// ===========================================================================
// 辅助函数：检查文件是否存在 + 获取其大小
// ===========================================================================
// 返回 -1 表示文件不存在或无法访问，>= 0 表示文件大小
static long get_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;             // 文件不存在或不可访问
    return (long)st.st_size;
}

// ===========================================================================
// 析构函数
// ===========================================================================
// 服务器退出时，遍历 map 关闭所有打开的文件描述符（不删临时文件，留给下次重启断点续传）
UploadManager::~UploadManager()
{
    // 不在这里加锁：析构时不应该再有其他线程在调用 UploadManager 的方法
    for (std::map<std::string, UploadMeta>::iterator it = m_uploads.begin();
         it != m_uploads.end(); ++it)
    {
        if (it->second.fd != -1)
        {
            close(it->second.fd);
            it->second.fd = -1;
        }
    }
    m_uploads.clear();
    // 互斥锁由 locker 析构函数自动销毁，无需手动 pthread_mutex_destroy
}

// ===========================================================================
// init_upload —— 第一阶段：初始化上传
// ===========================================================================
bool UploadManager::init_upload(const std::string &filename, size_t total_size,
                                const std::string &md5, std::string &out_error)
{
    // ---- 1. 加锁 ----
    m_mutex.lock();

    // ---- 2. 确保上传目录存在 ----
    if (!ensure_directory(m_upload_dir.c_str()))
    {
        out_error = "无法创建上传目录：";
        out_error += err_to_str(errno);
        m_mutex.unlock();
        return false;
    }

    // ---- 3. 构建临时文件和最终文件的路径 ----
    // 临时文件名 = 原文件名 + ".tmp"，例如 "video.mp4" → "video.mp4.tmp"
    char temp_buf[512];
    char final_buf[512];
    snprintf(temp_buf, sizeof(temp_buf), "%s/%s.tmp",
             m_upload_dir.c_str(), filename.c_str());
    snprintf(final_buf, sizeof(final_buf), "%s/%s",
             m_upload_dir.c_str(), filename.c_str());

    std::string temp_path(temp_buf);
    std::string final_path(final_buf);

    // ---- 4. 检查最终文件是否已存在（相同文件名已上传完毕） ----
    if (get_file_size(final_path.c_str()) >= 0)
    {
        out_error = "文件已存在：" + filename;
        m_mutex.unlock();
        return false;
    }

    // ---- 5. 检查 map 中是否已有记录（客户端重复调用 init） ----
    std::map<std::string, UploadMeta>::iterator it = m_uploads.find(filename);
    if (it != m_uploads.end())
    {
        // 已有记录 → 这是"查询进度"请求（断点续传的第一步）
        // 如果客户端传的 MD5 和之前不一样，说明可能换了文件，拒绝
        if (it->second.expected_md5 != md5)
        {
            out_error = "MD5 与之前注册的不一致，可能是不同文件";
            m_mutex.unlock();
            return false;
        }
        // 正常续传：out_error 留空表示成功，调用方用 get_upload 查 received_bytes
        m_mutex.unlock();
        return true;
    }

    // ---- 6. 这是全新上传：创建 UploadMeta 记录 ----
    UploadMeta meta;
    meta.filename      = filename;
    meta.temp_path     = temp_path;
    meta.final_path    = final_path;
    meta.expected_md5  = md5;
    meta.total_size    = total_size;
    meta.received_bytes = 0;
    meta.created_at     = time(NULL);

    // ---- 7. 打开或创建临时文件 ----
    // 先用 stat 检查临时文件是否已存在（可能是上次中断后留下的）
    long existing_size = get_file_size(temp_path.c_str());

    if (existing_size > 0)
    {
        // 临时文件已存在 → 断点续传场景（服务器重启后 tmp 文件还在）
        // 以现有文件大小作为已接收字节数
        meta.received_bytes = (size_t)existing_size;
    }

    // 打开临时文件：
    //   O_RDWR    — 读写模式
    //   O_CREAT   — 如果不存在则创建
    //   不设置 O_TRUNC — 不截断！保留已有数据，用于断点续传
    //   0644       — 权限：owner 可读写，group/other 只读
    meta.fd = open(temp_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (meta.fd == -1)
    {
        out_error = "无法创建临时文件：";
        out_error += err_to_str(errno);
        m_mutex.unlock();
        return false;
    }

    // ---- 8. 记录到 map 中 ----
    m_uploads[filename] = meta;

    m_mutex.unlock();
    return true;
}

// ===========================================================================
// write_chunk —— 第二阶段：写入数据块
// ===========================================================================
ssize_t UploadManager::write_chunk(const std::string &filename, const char *data,
                                    size_t len, size_t offset)
{
    m_mutex.lock();

    // ---- 1. 查找上传记录 ----
    std::map<std::string, UploadMeta>::iterator it = m_uploads.find(filename);
    if (it == m_uploads.end())
    {
        m_mutex.unlock();
        return -1;   // 找不到此文件的上传记录
    }

    UploadMeta &meta = it->second;

    // ---- 2. 检查偏移量是否与预期一致 ----
    // received_bytes 记录了已经确认接收的字节数，下一个分块的 offset 必须等于它。
    // 如果不等，说明分块乱序了（客户端 bug 或网络重传导致乱序到达）。
    if (offset != meta.received_bytes)
    {
        // 不拒绝写入（宽松处理），但日志应该能暴露这个问题
        // 实际上：如果 offset 小于 received_bytes，说明是重复分块（幂等，可以忽略）
        //         如果 offset 大于 received_bytes，说明跳过了数据（客户端 bug）
        // 当前策略：直接拒绝乱序，让客户端按顺序发送
        m_mutex.unlock();
        return -1;
    }

    // ---- 3. pwrite 写入数据 ----
    // pwrite(fd, buf, count, offset)：
    //   在文件的 offset 位置写入 count 字节，不改变 fd 的当前文件偏移量。
    //   这避免了 lseek + write 两步操作在并发场景下的竞态条件。
    //   即使多个线程同时给同一个 fd 写不同位置，pwrite 也是原子的（内核保证）。
    ssize_t written = pwrite(meta.fd, data, len, (off_t)offset);
    if (written < 0)
    {
        m_mutex.unlock();
        return -1;   // 磁盘写入失败（磁盘满 / I/O 错误等）
    }

    // ---- 4. 更新已接收字节数 ----
    // 注意：写成多少就加多少（written 可能小于 len，虽然正常情况不会）
    meta.received_bytes += (size_t)written;

    m_mutex.unlock();
    return written;
}

// ===========================================================================
// complete_upload —— 第三阶段：完成上传 + MD5 校验
// ===========================================================================
bool UploadManager::complete_upload(const std::string &filename,
                                     std::string &out_error)
{
    m_mutex.lock();

    // ---- 1. 查找上传记录 ----
    std::map<std::string, UploadMeta>::iterator it = m_uploads.find(filename);
    if (it == m_uploads.end())
    {
        out_error = "没有此文件的上传记录：" + filename;
        m_mutex.unlock();
        return false;
    }

    UploadMeta &meta = it->second;

    // ---- 2. 检查是否接收了全部数据 ----
    if (meta.received_bytes != meta.total_size)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "文件未接收完整：预期 %zu 字节，实际收到 %zu 字节",
                 meta.total_size, meta.received_bytes);
        out_error = buf;
        m_mutex.unlock();
        return false;
    }

    // ---- 3. fsync 确保数据落到磁盘 ----
    // fsync 会把内核缓冲区中的文件数据 + 元数据强制写入磁盘。
    // 如果不调 fsync，后面的 md5_file 可能读到的是旧数据（还在内核缓存里没落盘）。
    if (fsync(meta.fd) != 0)
    {
        out_error = "fsync 失败：";
        out_error += err_to_str(errno);
        m_mutex.unlock();
        return false;
    }

    // ---- 4. 关闭文件描述符 ----
    // 关闭后 md5_file 才能安全地打开同一个文件读（否则可能读到不一致的状态）
    close(meta.fd);
    meta.fd = -1;

    // ---- 5. 计算 MD5 并比对 ----
    std::string computed_md5 = md5_file(meta.temp_path.c_str());
    if (computed_md5.empty())
    {
        out_error = "无法计算文件 MD5（临时文件可能已被删除）";
        // 清理失败的上传记录（但保留临时文件，方便排查问题）
        m_uploads.erase(it);
        m_mutex.unlock();
        return false;
    }

    if (computed_md5 != meta.expected_md5)
    {
        // MD5 不一致 —— 文件在传输过程中损坏了
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "MD5 校验失败！预期：%s，实际：%s。请重新上传。",
                 meta.expected_md5.c_str(), computed_md5.c_str());
        out_error = buf;

        // 删除损坏的临时文件 + 移除记录
        unlink(meta.temp_path.c_str());
        m_uploads.erase(it);

        m_mutex.unlock();
        return false;
    }

    // ---- 6. MD5 一致：rename 临时文件为正式文件 ----
    // rename() 在同一文件系统内是原子操作，不会出现"移动到一半断电导致文件损坏"
    if (rename(meta.temp_path.c_str(), meta.final_path.c_str()) != 0)
    {
        out_error = "rename 失败：";
        out_error += err_to_str(errno);
        // rename 失败不删临时文件，数据还在，可以手动处理
        m_uploads.erase(it);
        m_mutex.unlock();
        return false;
    }

    // ---- 7. 成功！从 map 中移除记录 ----
    m_uploads.erase(it);

    m_mutex.unlock();
    return true;
}

// ===========================================================================
// get_upload —— 查询上传记录
// ===========================================================================
const UploadMeta *UploadManager::get_upload(const std::string &filename)
{
    m_mutex.lock();

    std::map<std::string, UploadMeta>::iterator it = m_uploads.find(filename);
    const UploadMeta *result = NULL;
    if (it != m_uploads.end())
    {
        result = &(it->second);
    }

    m_mutex.unlock();
    return result;
}

// ===========================================================================
// remove_upload —— 删除上传记录
// ===========================================================================
void UploadManager::remove_upload(const std::string &filename)
{
    m_mutex.lock();

    std::map<std::string, UploadMeta>::iterator it = m_uploads.find(filename);
    if (it != m_uploads.end())
    {
        if (it->second.fd != -1)
        {
            close(it->second.fd);         // 关闭文件描述符
        }
        m_uploads.erase(it);              // 从 map 中删除
    }

    m_mutex.unlock();
}

// ===========================================================================
// cleanup_stale —— 清理过期上传记录
// ===========================================================================
// 由服务器的主事件循环在 timer tick 中周期性调用（例如每 5 秒一次）。
// 场景：客户端上传到一半断网了，再也不回来继续——这些"僵尸"记录需要清理。
void UploadManager::cleanup_stale(time_t max_age_seconds)
{
    m_mutex.lock();

    time_t now = time(NULL);

    // 使用迭代器遍历 map，边遍历边删（安全写法：先 ++ 再删）
    std::map<std::string, UploadMeta>::iterator it = m_uploads.begin();
    while (it != m_uploads.end())
    {
        time_t age = now - it->second.created_at;
        if (age > max_age_seconds)
        {
            // 这个上传记录太旧了，清理掉
            if (it->second.fd != -1)
            {
                close(it->second.fd);                      // 关闭 fd
            }
            unlink(it->second.temp_path.c_str());           // 删除临时文件
            m_uploads.erase(it++);                          // 从 map 移除，迭代器前进
        }
        else
        {
            ++it;   // 不过期，跳过
        }
    }

    m_mutex.unlock();
}
