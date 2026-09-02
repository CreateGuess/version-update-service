#include "version_update.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

std::mutex VersionUpdate::g_versionJsonMutex;
std::mutex VersionUpdate::g_cacheMutex;
std::map<std::string, VersionUpdate::CacheEntry> VersionUpdate::g_versionCache;

/**
 * @brief 获取软件包根目录
 * @return 返回软件包根目录，默认为 /home/codeit/update
 */
fs::path VersionUpdate::getPackageRoot()
{
    // 1. 优先读取环境变量中的软件包根目录
    const char *configuredRoot = std::getenv("CODEIT_PACKAGE_ROOT");
    if (configuredRoot != nullptr && configuredRoot[0] != '\0')
    {
        return fs::path(configuredRoot);
    }

    // 2. 返回默认软件包根目录
    return fs::path("/home/codeit/update");
}

namespace
{
bool isSafePathComponent(const std::string &value)
{
    static const std::regex componentRegex(R"(^[A-Za-z0-9][A-Za-z0-9._-]*$)");
    return std::regex_match(value, componentRegex);
}
} // namespace

/**
 * @brief 获取软件包目录
 * @param query 软件包查询维度
 * @return 返回软件包目录路径，如果参数不合法则返回 std::nullopt
 */
std::optional<fs::path> VersionUpdate::getPackageDirectory(const PackageQuery &query)
{
    // 每个字段必须是单个安全路径片段，禁止绝对路径和目录穿越。
    if (!isSafePathComponent(query.type) || !isSafePathComponent(query.channel))
    {
        return std::nullopt;
    }

    if (query.type == "codeit-deploy" || query.type == "codeit-lib")
    {
        if (!query.name.empty() || !isSafePathComponent(query.arch) ||
            !isSafePathComponent(query.platform) || !isSafePathComponent(query.os))
        {
            return std::nullopt;
        }
        return getPackageRoot() / query.type / query.arch / query.platform / query.os / query.channel;
    }

    if (query.type == "backend")
    {
        if (!isSafePathComponent(query.name) || !isSafePathComponent(query.arch) ||
            !query.platform.empty() || !query.os.empty())
        {
            return std::nullopt;
        }
        return getPackageRoot() / query.type / query.name / query.arch / query.channel;
    }

    if (query.type == "frontend")
    {
        if (!isSafePathComponent(query.name) || !query.arch.empty() ||
            !query.platform.empty() || !query.os.empty())
        {
            return std::nullopt;
        }
        return getPackageRoot() / query.type / query.name / query.channel;
    }

    return std::nullopt;
}

/**
 * @brief 解析软件包文件名，提取版本信息
 * @param filename 软件包文件名，如 v1.2.3.zip
 * @return 返回 VersionInfo 结构体，如果文件名不合法则返回 std::nullopt
 */
std::optional<VersionUpdate::VersionInfo> VersionUpdate::parsePackageFilename(const std::string &filename)
{
    // 1. 使用正则表达式匹配文件名
    static const std::regex packageRegex(R"(^v([0-9]+)\.([0-9]+)\.([0-9]+)\.zip$)");
    std::smatch match;
    if (!std::regex_match(filename, match, packageRegex))
    {
        return std::nullopt;
    }

    // 2. 将版本号转换为无符号整数
    try
    {
        VersionInfo version;
        version.major = std::stoull(match[1].str());
        version.minor = std::stoull(match[2].str());
        version.patch = std::stoull(match[3].str());
        return version;
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}

/**
 * @brief 查找最新的软件包，只返回最新的两个包
 * @param directory 软件包目录
 * @param count 要返回的软件包数量
 * @return 返回最新的软件包信息列表
 */
std::vector<VersionUpdate::PackageInfo> VersionUpdate::findLatestPackages(const fs::path &directory, std::size_t count)
{
    // 1. 检查目录是否存在
    std::error_code error;
    if (!fs::exists(directory, error) || error || !fs::is_directory(directory, error) || error)
    {
        return {};
    }
    // 2. 遍历目录，跳过没有权限访问的文件
    std::vector<PackageInfo> packages;
    fs::directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, error);
    const fs::directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error))
    {
        const auto &entry = *iterator;
        // 3. 只处理普通文件
        if (!entry.is_regular_file(error) || error)
        {
            error.clear();
            continue;
        }
        const std::string filename = entry.path().filename().string();
        // 4. 解析文件名，提取版本信息
        auto version = parsePackageFilename(filename);
        if (!version.has_value())
        {
            continue;
        }
        // 5. 构建 PackageInfo 对象
        PackageInfo packageInfo;
        packageInfo.version = version.value();
        packageInfo.path = entry.path();
        packages.push_back(packageInfo);
    }

    // 6. 按版本号排序，取最新的 count 个包
    std::sort(packages.begin(), packages.end(), [](const PackageInfo &left, const PackageInfo &right)
              { return right.version < left.version; });

    if (packages.size() > count)
    {
        packages.resize(count);
    }
    return packages;
}

/**
 * @brief 计算文件的 SHA256 哈希值
 * @param filePath 文件路径
 * @return 返回文件的 SHA256 哈希值的十六进制字符串表示
 */
std::string VersionUpdate::calculateSha256(const fs::path &filePath)
{
    // 打开文件
    std::ifstream file(filePath, std::ios::binary);
    // 判断文件是否成功打开
    if (!file)
    {
        throw std::runtime_error("无法打开文件: " + filePath.string());
    }
    // 创建 SHA256 上下文
    EVP_MD_CTX *context = EVP_MD_CTX_new();

    if (context == nullptr)
    {
        throw std::runtime_error("EVP_MD_CTX_new失败");
    }

    // 初始化SHA256
    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1)
    {
        EVP_MD_CTX_free(context);

        throw std::runtime_error("EVP_DigestInit_ex失败");
    }

    constexpr std::size_t BUFFER_SIZE = 1024 * 1024;

    char buffer[BUFFER_SIZE];

    // 分块读取文件
    while (file)
    {
        file.read(buffer, sizeof(buffer));

        const std::streamsize readSize = file.gcount();

        if (readSize <= 0)
        {
            continue;
        }

        if (EVP_DigestUpdate(context, buffer, static_cast<std::size_t>(readSize)) != 1)
        {
            EVP_MD_CTX_free(context);

            throw std::runtime_error("EVP_DigestUpdate失败");
        }
    }

    if (file.bad())
    {
        EVP_MD_CTX_free(context);
        throw std::runtime_error("读取文件失败: " + filePath.string());
    }

    // 获得最终SHA256
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digestLength = 0;

    if (EVP_DigestFinal_ex(context, digest, &digestLength) != 1)
    {
        EVP_MD_CTX_free(context);

        throw std::runtime_error("EVP_DigestFinal_ex失败");
    }

    EVP_MD_CTX_free(context);

    // 二进制转十六进制字符串
    std::ostringstream output;

    for (unsigned int i = 0; i < digestLength; ++i)
    {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }

    return output.str();
}

/**
 * @brief 获取当前UTC时间，格式为ISO 8601
 * @return 当前UTC时间的字符串表示，例如：2024-06-01T12:34:56Z
 */
std::string VersionUpdate::getCurrentUtcTime()
{
    // 获取当前UTC时间
    const std::time_t now = std::time(nullptr);

    std::tm timeInfo{};

#ifdef _WIN32
    gmtime_s(&timeInfo, &now);
#else
    gmtime_r(&now, &timeInfo);
#endif

    std::ostringstream output;

    output << std::put_time(&timeInfo, "%Y-%m-%dT%H:%M:%SZ");

    return output.str();
}

/**
 * @brief 将软件包查询维度转换为JSON
 */
json VersionUpdate::createQueryJson(const PackageQuery &query)
{
    json result = {{"type", query.type}, {"channel", query.channel}};
    if (!query.name.empty()) result["name"] = query.name;
    if (!query.arch.empty()) result["arch"] = query.arch;
    if (!query.platform.empty()) result["platform"] = query.platform;
    if (!query.os.empty()) result["os"] = query.os;
    return result;
}

/**
 * @brief 创建version.json
 * @param query 软件包查询维度
 * @param packages 软件包信息列表
 * @return 返回生成的version.json对象
 */
json VersionUpdate::createVersionJson(const PackageQuery &query, const std::vector<VersionUpdate::PackageInfo> &packages)
{
    // 1. 合理性检查
    if (packages.empty())
    {
        throw std::runtime_error("没有可用的软件包");
    }

    // 2. 构建version.json对象
    json result;
    result["query"] = createQueryJson(query);

    // 第0个就是最新版本
    result["latest_version"] = packages.front().version.toString();
    result["packages"] = json::array();

    // 写入最近两个版本
    for (std::size_t i = 0; i < packages.size(); ++i)
    {
        const PackageInfo &package = packages[i];
        const std::string filename = package.path.filename().string();

        json item;
        item["version"] = package.version.toString();
        item["is_latest"] = (i == 0);
        item["filename"] = filename;
        json downloadBody = createQueryJson(query);
        downloadBody["filename"] = filename;
        item["download"] = {
            {"method", "POST"},
            {"url", "/api/package"},
            {"body", std::move(downloadBody)}};
        item["size"] = fs::file_size(package.path);
        item["sha256"] = calculateSha256(package.path);
        result["packages"].push_back(std::move(item));
    }

    result["generated_at"] = getCurrentUtcTime();
    return result;
}

/**
 * @brief 创建软件包目录快照
 * @param packages 软件包信息列表
 * @return 返回包含文件名、文件大小和修改时间的软件包快照
 */
std::vector<VersionUpdate::PackageSnapshot> VersionUpdate::createPackageSnapshot(const std::vector<VersionUpdate::PackageInfo> &packages)
{
    // 1. 创建软件包快照列表
    std::vector<PackageSnapshot> snapshot;
    snapshot.reserve(packages.size());

    // 2. 读取每个软件包的文件信息
    for (const PackageInfo &package : packages)
    {
        PackageSnapshot item;
        item.filename = package.path.filename().string();
        item.size = fs::file_size(package.path);
        item.modifiedTime = fs::last_write_time(package.path);
        snapshot.push_back(std::move(item));
    }

    return snapshot;
}

/**
 * @brief 获取或创建version.json
 * @param query 软件包查询维度
 * @param directory 软件包目录
 * @param packages 最新的软件包信息列表
 * @return 返回缓存或重新生成的version.json对象
 */
json VersionUpdate::getOrCreateVersionJson(const PackageQuery &query, const fs::path &directory, const std::vector<VersionUpdate::PackageInfo> &packages)
{
    // 1. 创建当前软件包目录快照和缓存键
    const std::vector<PackageSnapshot> currentSnapshot = createPackageSnapshot(packages);
    const std::string cacheKey = directory.lexically_normal().string();

    // 2. 加锁，避免多个请求同时更新缓存
    std::lock_guard<std::mutex> lock(g_cacheMutex);

    // 3. 软件包没有变化时直接返回缓存
    const auto iterator = g_versionCache.find(cacheKey);
    if (iterator != g_versionCache.end() && iterator->second.snapshot == currentSnapshot)
    {
        return iterator->second.versionJson;
    }

    // 4. 软件包发生变化时重新创建版本信息。version.json 只是磁盘缓存，
    //    压缩包目录只读时不应导致查询接口失败。
    json result = createVersionJson(query, packages);
    try
    {
        saveVersionJson(directory, result);
    }
    catch (const std::exception &error)
    {
        std::cerr << "保存version.json失败，将仅使用内存缓存: " << error.what() << std::endl;
    }

    // 5. 更新内存缓存
    CacheEntry entry;
    entry.snapshot = currentSnapshot;
    entry.versionJson = result;
    g_versionCache[cacheKey] = std::move(entry);

    return result;
}

/**
 * @brief 保存version.json到指定目录
 * @param directory 目录路径
 * @param data JSON数据
 */
void VersionUpdate::saveVersionJson(const fs::path &directory, const json &data)
{
    // 1. 加锁，避免多个请求同时写入version.json
    std::lock_guard<std::mutex> lock(g_versionJsonMutex);

    // 2. 确保目标目录存在
    std::error_code directoryError;
    fs::create_directories(directory, directoryError);
    if (directoryError)
    {
        throw std::runtime_error("无法创建目录: " + directory.string() + ": " + directoryError.message());
    }

    const fs::path temporaryPath = directory / "version.json.tmp";
    const fs::path versionPath = directory / "version.json";

    // 3. 先将JSON数据写入临时文件
    {
        std::ofstream output(temporaryPath, std::ios::trunc);

        if (!output)
        {
            throw std::runtime_error("无法创建文件: " + temporaryPath.string());
        }

        output << data.dump(4) << std::endl;

        if (!output)
        {
            throw std::runtime_error("写入version.json失败");
        }
    }

    // 4. 原子替换version.json
    std::error_code renameError;
    fs::rename(temporaryPath, versionPath, renameError);
    if (renameError)
    {
        fs::remove(temporaryPath);
        throw std::runtime_error("version.json替换失败: " + renameError.message());
    }
}

/**
 * @brief 发送JSON响应
 * @param response HTTP响应对象
 * @param status 状态码
 * @param data JSON数据
 */
void VersionUpdate::sendJson(httplib::Response &response, int status, const json &data)
{
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(data.dump(4), "application/json; charset=utf-8");
}

/**
 * @brief 获取软件包文件路径
 * @param query 软件包查询维度
 * @param filename 文件名
 * @return 软件包文件路径，如果不存在则返回std::nullopt
 */
std::optional<fs::path> VersionUpdate::getPackageFile(const PackageQuery &query, const std::string &filename)
{
    // 1. 检查文件名是否合法
    if (!parsePackageFilename(filename).has_value())
    {
        return std::nullopt;
    }
    // 2. 获取软件包目录
    auto directory = getPackageDirectory(query);
    // 3. 检查目录是否存在
    if (!directory.has_value())
    {
        return std::nullopt;
    }

    const fs::path filePath = directory.value() / filename;
    // 4. 检查文件是否存在且是普通文件
    std::error_code error;
    if (!fs::exists(filePath, error) || error || !fs::is_regular_file(filePath, error) || error)
    {
        return std::nullopt;
    }
    // 5. 返回文件路径
    return filePath;
}
