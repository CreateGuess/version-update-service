#include "version_update.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace
{
constexpr std::size_t PublishedPackageCount = 2;          // 版本清单中保留的软件包数量
constexpr std::size_t MaxUploadBytes = 2ULL * 1024 * 1024 * 1024; // 单个上传最大2GiB
constexpr char ListenHost[] = "127.0.0.1";               // 服务监听地址
constexpr int ListenPort = 28001;                         // 服务监听端口
std::atomic<httplib::Server *> activeServer{nullptr};     // 当前运行的HTTP服务
std::atomic<unsigned long long> uploadSequence{0};         // 上传临时文件序号

/**
 * @brief 构建错误响应JSON
 * @param code 错误码
 * @param message 错误说明
 * @return 返回错误响应JSON对象
 */
json errorBody(const std::string &code, const std::string &message)
{
    return {{"error", code}, {"message", message}};
}

/**
 * @brief 从JSON对象中读取可选字符串字段
 * @param body JSON请求体
 * @param field 字段名
 * @param value 输出字段值；字段不存在时清空
 * @return 字段不存在或类型为字符串时返回true，否则返回false
 */
bool readString(const json &body, const char *field, std::string &value)
{
    const auto iterator = body.find(field);
    if (iterator == body.end())
    {
        value.clear();
        return true;
    }
    if (!iterator->is_string())
    {
        return false;
    }
    value = iterator->get<std::string>();
    return true;
}

/**
 * @brief 解析并检查HTTP请求中的JSON对象
 * @return 解析成功返回true；失败时写入400响应并返回false
 */
bool parseRequestJson(const httplib::Request &request, json &body, httplib::Response &response)
{
    body = json::parse(request.body, nullptr, false);
    if (body.is_discarded() || !body.is_object())
    {
        VersionUpdate::sendJson(response, 400, errorBody("invalid_json", "请求体必须是JSON对象"));
        return false;
    }
    return true;
}

/**
 * @brief 从JSON请求体提取软件包查询维度
 * @return 所有已提供字段均为字符串时返回true
 */
bool parsePackageQuery(const json &body, VersionUpdate::PackageQuery &query, httplib::Response &response)
{
    if (!readString(body, "type", query.type) ||
        !readString(body, "name", query.name) ||
        !readString(body, "arch", query.arch) ||
        !readString(body, "platform", query.platform) ||
        !readString(body, "os", query.os) ||
        !readString(body, "channel", query.channel))
    {
        VersionUpdate::sendJson(response, 400, errorBody("invalid_parameters", "查询字段必须是字符串"));
        return false;
    }
    return true;
}

/**
 * @brief 获取网页文件接口的path查询参数
 * @return URL解码后的相对路径；未提供参数时返回空字符串，表示软件包根目录
 */
std::string requestPathParameter(const httplib::Request &request)
{
    return request.has_param("path") ? request.get_param_value("path") : std::string{};
}

/**
 * @brief 检查浏览路径是否由安全的相对路径片段组成
 *
 * 只允许字母、数字、点、下划线和连字符，拒绝绝对路径、反斜杠、
 * 当前目录和父目录片段，避免通过请求参数构造目录穿越路径。
 */
bool isSafeRelativePath(const std::string &relativePath)
{
    if (relativePath.find('\\') != std::string::npos)
    {
        return false;
    }

    const fs::path path(relativePath);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
    {
        return false;
    }
    // 按目录层级逐段检查，空路径没有层级，代表根目录，是合法输入。
    for (const auto &part : path)
    {
        const std::string value = part.string();
        if (value.empty() || value == "." || value == ".." ||
            !std::isalnum(static_cast<unsigned char>(value.front())))
        {
            return false;
        }
        for (const unsigned char character : value)
        {
            if (!std::isalnum(character) && character != '.' &&
                character != '_' && character != '-')
            {
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief 判断规范化后的候选路径是否仍位于软件包根目录中
 *
 * 此检查与相对路径字符检查配合使用，用于阻止目录中的符号链接把
 * 请求导向根目录之外。
 */
bool isWithinRoot(const fs::path &root, const fs::path &candidate)
{
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty())
    {
        return false;
    }
    const auto first = relative.begin();
    return first == relative.end() || *first != "..";
}

/**
 * @brief 将网页传入的相对路径安全解析为服务器绝对路径
 * @return 路径合法且没有越过软件包根目录时返回规范化路径
 */
std::optional<fs::path> resolvePackagePath(const std::string &relativePath)
{
    if (!isSafeRelativePath(relativePath))
    {
        return std::nullopt;
    }

    std::error_code error;
    // weakly_canonical会解析已存在部分中的符号链接，同时允许末尾文件尚不存在。
    const fs::path root = fs::weakly_canonical(VersionUpdate::getPackageRoot(), error);
    if (error)
    {
        return std::nullopt;
    }
    const fs::path candidate = fs::weakly_canonical(root / fs::path(relativePath), error);
    if (error || !isWithinRoot(root, candidate))
    {
        return std::nullopt;
    }
    return candidate;
}

/**
 * @brief 校验网页上传接口的Bearer令牌
 *
 * 未设置CODEIT_UPLOAD_TOKEN时保持向后兼容，允许上传；设置后必须提供
 * Authorization: Bearer <token>。
 */
bool isUploadAuthorized(const httplib::Request &request)
{
    const char *configuredToken = std::getenv("CODEIT_UPLOAD_TOKEN");
    if (configuredToken == nullptr || configuredToken[0] == '\0')
    {
        return true;
    }
    return request.get_header_value("Authorization") ==
           std::string("Bearer ") + configuredToken;
}

/**
 * @brief 获取静态网页目录
 *
 * 优先使用CODEIT_WEB_ROOT；开发环境使用当前目录下的web；systemd部署
 * 默认使用/opt/version-server/web。
 */
fs::path getWebRoot()
{
    const char *configuredRoot = std::getenv("CODEIT_WEB_ROOT");
    if (configuredRoot != nullptr && configuredRoot[0] != '\0')
    {
        return fs::path(configuredRoot);
    }

    std::error_code error;
    const fs::path localWeb = fs::current_path(error) / "web";
    if (!error && fs::is_directory(localWeb, error) && !error)
    {
        return localWeb;
    }
    return fs::path("/opt/version-server/web");
}

/**
 * @brief 停止HTTP服务
 * @param signal 系统信号
 */
void stopServer(int)
{
    httplib::Server *server = activeServer.load();
    if (server != nullptr)
    {
        server->stop();
    }
}

/**
 * @brief 处理版本信息查询请求
 * @param request HTTP请求对象
 * @param response HTTP响应对象
 */
void handleVersion(const httplib::Request &request, httplib::Response &response)
{
    // 1. 从JSON请求体获取并检查软件包维度
    json body;
    VersionUpdate::PackageQuery query;
    if (!parseRequestJson(request, body, response) ||
        !parsePackageQuery(body, query, response))
    {
        return;
    }

    const auto directory = VersionUpdate::getPackageDirectory(query);
    if (!directory)
    {
        VersionUpdate::sendJson(response, 400, errorBody("invalid_parameters", "软件包查询参数不完整或不合法"));
        return;
    }

    // 2. 查找最新的软件包
    const auto packages = VersionUpdate::findLatestPackages(*directory, PublishedPackageCount);
    if (packages.empty())
    {
        std::cerr << "没有找到软件包，扫描目录: " << directory->string() << std::endl;
        VersionUpdate::sendJson(response, 404, errorBody("package_not_found", "没有可用的软件包"));
        return;
    }

    // 3. 获取缓存或重新创建版本信息
    json result = VersionUpdate::getOrCreateVersionJson(query, *directory, packages);

    // 4. 返回版本信息
    VersionUpdate::sendJson(response, 200, result);
}

/**
 * @brief 处理软件包下载请求
 * @param request HTTP请求对象
 * @param response HTTP响应对象
 */
void handlePackage(const httplib::Request &request, httplib::Response &response)
{
    // 1. 从JSON请求体获取软件包维度和文件名
    json body;
    VersionUpdate::PackageQuery query;
    std::string filename;
    if (!parseRequestJson(request, body, response) ||
        !parsePackageQuery(body, query, response))
    {
        return;
    }
    if (!readString(body, "filename", filename) || filename.empty())
    {
        VersionUpdate::sendJson(response, 400, errorBody("invalid_parameters", "filename必须是非空字符串"));
        return;
    }

    const auto package = VersionUpdate::getPackageFile(query, filename);
    if (!package)
    {
        VersionUpdate::sendJson(response, 404, errorBody("package_not_found", "软件包不存在"));
        return;
    }

    // 2. 设置下载响应头并发送文件
    response.set_header("Cache-Control", "public, max-age=3600");
    response.set_header("Content-Disposition", "attachment; filename=\"" + package->filename().string() + "\"");
    response.set_file_content(package->string(), "application/zip");
}

/**
 * @brief 处理网页目录浏览请求
 *
 * 返回当前相对目录下的子目录和符合版本命名规则的ZIP文件。为避免网页
 * 暴露无关文件，version.json、临时文件和无法识别版本的ZIP均不返回。
 */
void handleFileList(const httplib::Request &request, httplib::Response &response)
{
    // 1. 将path参数转换为软件包根目录内的安全绝对路径。
    const std::string relativePath = requestPathParameter(request);
    const auto directory = resolvePackagePath(relativePath);
    std::error_code error;
    if (!directory || !fs::is_directory(*directory, error) || error)
    {
        VersionUpdate::sendJson(response, 404, errorBody("directory_not_found", "目录不存在或不可访问"));
        return;
    }

    // 2. 遍历目录，将子目录和软件包分别放入数组。
    json directories = json::array();
    json files = json::array();
    fs::directory_iterator iterator(*directory, fs::directory_options::skip_permission_denied, error);
    const fs::directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error))
    {
        const auto &entry = *iterator;
        const std::string name = entry.path().filename().string();
        const std::string childPath = relativePath.empty() ? name : relativePath + "/" + name;
        std::error_code entryError;
        // 子目录仅返回名称和继续浏览时使用的相对路径。
        if (entry.is_directory(entryError) && !entryError)
        {
            directories.push_back({{"name", name}, {"path", childPath}});
            continue;
        }
        if (!entry.is_regular_file(entryError) || entryError ||
            entry.path().extension() != ".zip")
        {
            continue;
        }

        // 只展示版本解析器能够识别的ZIP，保证页面展示和版本接口一致。
        const auto version = VersionUpdate::parsePackageFilename(name);
        if (!version)
        {
            continue;
        }

        json item = {{"name", name},
                     {"path", childPath},
                     {"size", entry.file_size(entryError)}};
        if (entryError)
        {
            item["size"] = 0;
        }
        item["version"] = version->toString();
        files.push_back(std::move(item));
    }
    if (error)
    {
        VersionUpdate::sendJson(response, 500, errorBody("directory_read_failed", "读取目录失败"));
        return;
    }

    // 3. 固定输出顺序，避免文件系统遍历顺序变化导致页面跳动。
    auto byName = [](const json &left, const json &right)
    { return left.at("name").get<std::string>() < right.at("name").get<std::string>(); };
    std::sort(directories.begin(), directories.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    json result = {{"path", relativePath},
                   {"directories", std::move(directories)},
                   {"files", std::move(files)}};
    VersionUpdate::sendJson(response, 200, result);
}

/**
 * @brief 处理网页中的ZIP直接下载请求
 *
 * path参数必须指向软件包根目录内、且名称能够被版本解析器识别的普通文件。
 */
void handleBrowserDownload(const httplib::Request &request, httplib::Response &response)
{
    // 1. 解析并校验下载目标，避免任意文件下载。
    const auto filePath = resolvePackagePath(requestPathParameter(request));
    std::error_code error;
    if (!filePath || !fs::is_regular_file(*filePath, error) || error ||
        !VersionUpdate::parsePackageFilename(filePath->filename().string()))
    {
        VersionUpdate::sendJson(response, 404, errorBody("package_not_found", "软件包不存在"));
        return;
    }

    // 2. 通过Content-Disposition让浏览器使用磁盘上的原文件名保存。
    response.set_header("Cache-Control", "public, max-age=3600");
    response.set_header("Content-Disposition", "attachment; filename=\"" + filePath->filename().string() + "\"");
    response.set_file_content(filePath->string(), "application/zip");
}

/**
 * @brief 处理网页multipart/form-data流式上传
 * @param contentReader cpp-httplib提供的请求体流式读取器
 *
 * 上传内容直接分块写入目标目录中的唯一临时文件，不把大型ZIP完整放入
 * 内存。全部接收成功后再重命名为正式文件，失败时删除临时文件。
 */
void handleBrowserUpload(const httplib::Request &request, httplib::Response &response,
                         const httplib::ContentReader &contentReader)
{
    // 1. 如果服务配置了上传令牌，先完成鉴权再读取大型请求体。
    if (!isUploadAuthorized(request))
    {
        VersionUpdate::sendJson(response, 401, errorBody("unauthorized", "上传令牌无效"));
        return;
    }

    // 2. 上传只能写入已存在且位于软件包根目录内的目录。
    const auto directory = resolvePackagePath(requestPathParameter(request));
    std::error_code error;
    if (!directory || !fs::is_directory(*directory, error) || error)
    {
        VersionUpdate::sendJson(response, 404, errorBody("directory_not_found", "上传目录不存在或不可访问"));
        return;
    }

    // 3. 为流式接收准备状态；接口一次只接受字段名为file的一个文件。
    bool receivedFile = false;
    bool invalidUpload = false;
    std::size_t uploadedBytes = 0;
    std::string filename;
    fs::path temporaryPath;
    fs::path targetPath;
    std::ofstream output;

    const bool readSucceeded = contentReader(
        // multipart头回调：校验文件名、重复文件并创建临时文件。
        [&](const httplib::FormData &file)
        {
            if (receivedFile || file.name != "file" || file.filename.empty() ||
                file.filename.find('/') != std::string::npos ||
                file.filename.find('\\') != std::string::npos ||
                !VersionUpdate::parsePackageFilename(file.filename))
            {
                invalidUpload = true;
                return false;
            }

            receivedFile = true;
            filename = file.filename;
            targetPath = *directory / filename;
            if (fs::exists(targetPath, error) || error)
            {
                invalidUpload = true;
                return false;
            }

            // 每次上传使用不同临时文件名，避免并发上传相互覆盖。
            temporaryPath = *directory /
                            (filename + ".upload-" + std::to_string(++uploadSequence) + ".tmp");
            output.open(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                invalidUpload = true;
                return false;
            }
            return true;
        },
        // 数据回调：逐块写盘，并独立限制实际文件内容不超过2GiB。
        [&](const char *data, std::size_t length)
        {
            if (!output || length > MaxUploadBytes || uploadedBytes > MaxUploadBytes - length)
            {
                invalidUpload = true;
                return false;
            }
            uploadedBytes += length;
            output.write(data, static_cast<std::streamsize>(length));
            if (!output)
            {
                invalidUpload = true;
                return false;
            }
            return true;
        });

    // 4. 无论读取结果如何都先关闭文件，再进行清理或重命名。
    if (output.is_open())
    {
        output.close();
    }
    if (!readSucceeded || invalidUpload || !receivedFile)
    {
        if (!temporaryPath.empty()) fs::remove(temporaryPath, error);
        VersionUpdate::sendJson(response, 400, errorBody("invalid_upload", "请选择合法且未存在的ZIP软件包"));
        return;
    }

    // 5. 全部写入成功后原子重命名，页面不会看到未完成的ZIP。
    fs::rename(temporaryPath, targetPath, error);
    if (error)
    {
        fs::remove(temporaryPath, error);
        VersionUpdate::sendJson(response, 500, errorBody("upload_failed", "保存上传文件失败"));
        return;
    }

    VersionUpdate::sendJson(response, 201,
                            {{"filename", filename}, {"size", uploadedBytes}, {"message", "上传成功"}});
}
} // namespace

int main()
{
    try
    {
        // 1. 创建HTTP服务并注册退出信号
        httplib::Server server;
        // multipart解析器和流式上传处理器共同限制请求体最大为2GiB。
        server.set_payload_max_length(MaxUploadBytes);
        activeServer.store(&server);
        std::signal(SIGINT, stopServer);
        std::signal(SIGTERM, stopServer);

        // 2. 注册HTTP接口
        server.Get("/health", [](const httplib::Request &, httplib::Response &response)
                   { VersionUpdate::sendJson(response, 200, {{"status", "ok"}}); });
        // 原有JSON版本查询与下载接口。
        server.Post("/api/version", handleVersion);
        server.Post("/api/package", handlePackage);
        // Web管理页面使用的目录浏览、直接下载和流式上传接口。
        server.Get("/api/files", handleFileList);
        server.Get("/api/files/download", handleBrowserDownload);
        server.Post("/api/files/upload", handleBrowserUpload);

        // 将web目录挂载到站点根路径；API路由不存在同名静态文件，不受影响。
        const fs::path webRoot = getWebRoot();
        if (!server.set_mount_point("/", webRoot.string()))
        {
            std::cerr << "Web目录不存在，管理页面不可用: " << webRoot << std::endl;
        }

        // 3. 注册请求异常处理器
        server.set_exception_handler([](const httplib::Request &, httplib::Response &response, std::exception_ptr exception)
                                     {
            std::string message = "服务器内部错误";
            try
            {
                if (exception) std::rethrow_exception(exception);
            }
            catch (const std::exception &error)
            {
                std::cerr << "请求处理失败: " << error.what() << std::endl;
            }
            VersionUpdate::sendJson(response, 500, errorBody("internal_error", message)); });

        // 4. 注册未找到接口处理器
        server.set_error_handler([](const httplib::Request &request, httplib::Response &response)
                                 {
            // 仅将未匹配到任何路由的404转换为“接口不存在”。业务处理器
            // 返回的package_not_found等404必须保留原始响应。
            if (response.status == 404 && request.matched_route.empty())
                VersionUpdate::sendJson(response, 404, errorBody("not_found", "接口不存在")); });

        // 5. 启动HTTP服务
        std::cout << "Codeit version server listening on " << ListenHost << ':' << ListenPort
                  << ", package root: " << VersionUpdate::getPackageRoot()
                  << ", web root: " << webRoot << std::endl;
        if (!server.listen(ListenHost, ListenPort))
        {
            std::cerr << "监听失败: " << ListenHost << ':' << ListenPort << std::endl;
            activeServer.store(nullptr);
            return 1;
        }

        // 6. 服务停止后清除服务指针
        activeServer.store(nullptr);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "启动失败: " << error.what() << std::endl;
        activeServer.store(nullptr);
        return 1;
    }
}
