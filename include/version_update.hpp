#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

using json = nlohmann::json;

class VersionUpdate
{
public:
    struct VersionInfo
    {
        unsigned long long major{}; // 主版本号
        unsigned long long minor{}; // 次版本号
        unsigned long long patch{}; // 修订号
        std::string toString() const
        {
            return "v" + std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
        }

        bool operator<(const VersionInfo &other) const
        {
            return std::tie(major, minor, patch) < std::tie(other.major, other.minor, other.patch);
        }
    };

    struct PackageInfo
    {
        VersionInfo version;
        fs::path path;
    };

public:
    // 获取软件包目录，后端区分架构，frontend 固定使用 robot-platform 项目目录
    static std::optional<fs::path> getPackageDirectory(const std::string &arch, const std::string &channel, const std::string &type);
    // 获取软件包根目录（可通过 CODEIT_PACKAGE_ROOT 配置）
    static fs::path getPackageRoot();
    // 解析软件包文件名
    static std::optional<VersionInfo> parsePackageFilename(const std::string &filename);
    // 查找最新的软件包，只返回最新的两个包
    static std::vector<PackageInfo> findLatestPackages(const fs::path &directory, std::size_t count);
    // 计算文件的 SHA256 哈希值
    static std::string calculateSha256(const fs::path &filePath);
    // 获取当前UTC时间
    static std::string getCurrentUtcTime();
    // 构建软件包下载URL
    static std::string buildPackageUrl(const std::string &type, const std::string &arch, const std::string &channel, const std::string &filename);
    // 创建version.json
    static json createVersionJson(const std::string &type, const std::string &arch, const std::string &channel, const std::vector<PackageInfo> &packages);
    // 获取缓存中的version.json，软件包发生变化时重新创建
    static json getOrCreateVersionJson(const std::string &type, const std::string &arch, const std::string &channel, const fs::path &directory, const std::vector<PackageInfo> &packages);
    // 保存version.json到指定目录
    static void saveVersionJson(const fs::path &directory, const json &data);
    // 发送JSON响应
    static void sendJson(httplib::Response &response, int status, const json &data);
    // 获取软件包文件路径
    static std::optional<fs::path> getPackageFile(const std::string &type, const std::string &arch, const std::string &channel, const std::string &filename);

private:
    struct PackageSnapshot
    {
        std::string filename;                  // 软件包文件名
        std::uintmax_t size{};                 // 软件包文件大小
        fs::file_time_type modifiedTime;       // 软件包最后修改时间

        bool operator==(const PackageSnapshot &other) const
        {
            return filename == other.filename &&
                   size == other.size &&
                   modifiedTime == other.modifiedTime;
        }
    };

    struct CacheEntry
    {
        std::vector<PackageSnapshot> snapshot; // 软件包目录快照
        json versionJson;                      // 已生成的版本信息
    };

    // 创建软件包目录快照
    static std::vector<PackageSnapshot> createPackageSnapshot(const std::vector<PackageInfo> &packages);

    static std::mutex g_versionJsonMutex; // 保护version.json的互斥锁
    static std::mutex g_cacheMutex; // 保护版本信息缓存的互斥锁
    static std::map<std::string, CacheEntry> g_versionCache; // 版本信息缓存
};
