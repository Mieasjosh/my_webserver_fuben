#ifndef UPLOAD_HANDLER_H
#define UPLOAD_HANDLER_H

/******************************************************************************
 * upload_handler.h — 上传状态管理器
 *
 * 职责：
 *   管理所有正在进行的文件上传的元数据（文件名、大小、MD5、已收字节等），
 *   提供线程安全的 init → chunk → complete 三阶段上传接口，
 *   以及超时上传的清理功能。
 *
 * 线程安全设计：
 *   所有公共方法内部都先 lock 互斥锁再操作，保证多线程并发上传时数据一致。
 *   write_chunk 使用 pwrite()（定位写），天然线程安全——即使两个线程同时给
 *   同一个文件写不同偏移量的数据，也不会互相干扰。
 *
 * 与 MD5 的关系：
 *   complete_upload() 调用 md5_file() 计算接收文件的 MD5，
 *   与 init 时客户端提供的预期 MD5 比对，验证文件完整性。
 ******************************************************************************/

#include <string>
#include <map>
#include <stddef.h>    // size_t
#include <unistd.h>    // ssize_t（有符号的 size_t，用于表示 write/read 返回值）
#include "../lock/locker.h"

// ---------------------------------------------------------------------------
// UploadMeta —— 单个上传任务的元数据
// ---------------------------------------------------------------------------
// 每个上传文件对应一个 UploadMeta 对象，存储在 UploadManager 的 map 中。
// 生命周期：init_upload() 创建 → write_chunk() 更新 → complete_upload() 销毁，
//          或超时后被 cleanup_stale() 清理。
struct UploadMeta
{
    std::string filename;        // 客户端传来的原始文件名（已经过安全过滤，只保留 basename）
    std::string temp_path;       // 临时文件完整路径（例如 ./uploads/video.mp4.tmp）
    std::string final_path;      // 最终文件完整路径（例如 ./uploads/video.mp4）
    std::string expected_md5;    // 客户端提供的预期 MD5 值（32 位小写 hex 字符串）

    size_t total_size;           // 客户端声明的文件总大小（字节）
    size_t received_bytes;       // 服务器已确认接收并写入磁盘的字节数
                                 // 注意：这就是断点续传的"断点"——客户端从此偏移量继续发
    int    fd;                   // 临时文件的文件描述符（open 时获得，close 后置 -1）
    time_t created_at;           // 上传任务创建时间（Unix 时间戳，秒），用于超时判断

    // 构造函数：给所有成员安全初始值
    UploadMeta() : total_size(0), received_bytes(0), fd(-1), created_at(0) {}
};

// ---------------------------------------------------------------------------
// UploadManager —— 上传任务管理器（单例模式）
// ---------------------------------------------------------------------------
// 单例模式（与项目中 connection_pool、Log 一致）：
//   UploadManager::get_instance() 返回全局唯一实例。
//
// 使用示例：
//   UploadManager *mgr = UploadManager::get_instance();
//   mgr->set_upload_dir("./uploads");
//   string err;
//   mgr->init_upload("test.txt", 1024, "abc123...", err);
//   mgr->write_chunk("test.txt", data, 512, 0);
//   mgr->complete_upload("test.txt", err);
class UploadManager
{
public:
    // -----------------------------------------------------------------------
    // 获取单例实例（C++11 线程安全：static 局部变量初始化只执行一次）
    // -----------------------------------------------------------------------
    static UploadManager *get_instance()
    {
        static UploadManager instance;
        return &instance;
    }

    // -----------------------------------------------------------------------
    // 设置上传文件的存放目录
    // -----------------------------------------------------------------------
    // 必须在 init_upload() 之前调用（通常在服务器启动时由 WebServer 调用）。
    // dir —— 目录路径（例如 "./uploads"），不会拷贝字符串，只存到 std::string 中
    void set_upload_dir(const std::string &dir) { m_upload_dir = dir; }

    // -----------------------------------------------------------------------
    // 第一阶段：初始化上传（或查询已有进度，实现断点续传）
    // -----------------------------------------------------------------------
    // 调用时机：客户端 POST /upload/init
    //
    // 参数：
    //   filename   —— 客户端传来的文件名（调用方应已过滤路径穿越，只保留 basename）
    //   total_size —— 客户端声明的文件总大小
    //   md5        —— 客户端提供的预期 MD5（32 位小写 hex）
    //   out_error  —— [出参] 失败时写入错误描述
    //
    // 返回值：true=成功（包括首次创建和新点续传），false=失败
    //
    // 内部逻辑：
    //   ① 加锁
    //   ② 构建 temp_path = m_upload_dir/filename.tmp
    //   ③ 如果最终文件已存在 → 失败（"文件已存在"）
    //   ④ 如果 map 中没有记录 → 首次上传：
    //        - 如果临时文件存在，读取其大小作为 received_bytes（断点续传场景）
    //        - 打开临时文件（O_CREAT | O_RDWR，append 模式不截断）
    //        - 保存元数据到 map
    //   ⑤ 解锁
    bool init_upload(const std::string &filename, size_t total_size,
                     const std::string &md5, std::string &out_error);

    // -----------------------------------------------------------------------
    // 第二阶段：写入一个数据块
    // -----------------------------------------------------------------------
    // 调用时机：客户端 POST /upload/chunk
    //
    // 参数：
    //   filename —— 文件名（与 init_upload 一致）
    //   data     —— 指向数据块起始位置的指针
    //   len      —— 数据块长度（字节）
    //   offset   —— 数据块在文件中的起始偏移量
    //
    // 返回值：
    //   成功 → 实际写入的字节数（通常等于 len）
    //   失败 → -1（原因：找不到上传记录 / offset 与预期不符即乱序分块 / 磁盘写入失败）
    //
    // 内部逻辑：
    //   ① 加锁，查找 filename 对应的上传记录
    //   ② 检查 offset 是否等于 meta.received_bytes（乱序分块拒绝）
    //   ③ pwrite(fd, data, len, offset) 写入数据
    //   ④ 更新 meta.received_bytes += 实际写入字节数
    //   ⑤ 解锁
    ssize_t write_chunk(const std::string &filename, const char *data,
                        size_t len, size_t offset);

    // -----------------------------------------------------------------------
    // 第三阶段：完成上传 + MD5 验证
    // -----------------------------------------------------------------------
    // 调用时机：客户端 POST /upload/complete
    //
    // 参数：
    //   filename  —— 文件名
    //   out_error —— [出参] 失败时写入错误描述
    //
    // 返回值：true=成功（MD5 一致 + 已 rename），false=失败
    //
    // 内部逻辑：
    //   ① 加锁，查找记录
    //   ② fsync(fd) 确保内核缓冲区刷入磁盘
    //   ③ close(fd)，fd 置 -1
    //   ④ 调用 md5_file(temp_path) 计算接收到的文件的 MD5
    //   ⑤ 与 expected_md5 比对 → 不一致则删除临时文件、移除记录、返回失败
    //   ⑥ rename(temp_path → final_path)
    //   ⑦ 从 map 中移除记录
    //   ⑧ 解锁
    bool complete_upload(const std::string &filename, std::string &out_error);

    // -----------------------------------------------------------------------
    // 查询上传记录（只读，不修改）
    // -----------------------------------------------------------------------
    // 返回指针：找到 → 指向 UploadMeta，找不到 → NULL
    // 注意：返回的指针只在当前锁持有期间有效，调用方用完后不要保存。
    const UploadMeta *get_upload(const std::string &filename);

    // -----------------------------------------------------------------------
    // 删除上传记录（出错或取消时使用）
    // -----------------------------------------------------------------------
    // 关闭临时文件的 fd，从 map 中删除记录。
    // 注意：不会删除磁盘上的临时文件（留给调用方决定是否 unlink）。
    void remove_upload(const std::string &filename);

    // -----------------------------------------------------------------------
    // 清理过期上传（由事件循环中的定时器 tick 周期性调用）
    // -----------------------------------------------------------------------
    // max_age_seconds —— 超过此时间的上传记录将被清理（关闭 fd、删除临时文件、移除记录）
    // 清理频率建议：每个 timer tick（5 秒）调用一次，max_age 设为 3600（1 小时）。
    void cleanup_stale(time_t max_age_seconds);

private:
    // 构造函数私有（单例模式）；locker 成员自动初始化互斥锁
    UploadManager() {}

    // 析构函数：遍历 map 关闭所有打开的 fd；locker 成员自动销毁互斥锁
    ~UploadManager();

    // 禁止拷贝
    UploadManager(const UploadManager &);
    UploadManager &operator=(const UploadManager &);

    // ---- 成员变量 ----
    std::map<std::string, UploadMeta> m_uploads;   // 文件名 → 上传元数据
    std::string m_upload_dir;                       // 上传文件目录路径
    locker m_mutex;                                 // 互斥锁（保护 m_uploads）
};

#endif
