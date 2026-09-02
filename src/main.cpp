#include "version_update.hpp"

#include <atomic>
#include <csignal>
#include <iostream>

namespace
{
constexpr std::size_t PublishedPackageCount = 2;          // 版本清单中保留的软件包数量
constexpr char ListenHost[] = "127.0.0.1";               // 服务监听地址
constexpr int ListenPort = 28001;                         // 服务监听端口
std::atomic<httplib::Server *> activeServer{nullptr};     // 当前运行的HTTP服务

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
} // namespace

int main()
{
    try
    {
        // 1. 创建HTTP服务并注册退出信号
        httplib::Server server;
        activeServer.store(&server);
        std::signal(SIGINT, stopServer);
        std::signal(SIGTERM, stopServer);

        // 2. 注册HTTP接口
        server.Get("/health", [](const httplib::Request &, httplib::Response &response)
                   { VersionUpdate::sendJson(response, 200, {{"status", "ok"}}); });
        server.Post("/api/version", handleVersion);
        server.Post("/api/package", handlePackage);

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
        server.set_error_handler([](const httplib::Request &, httplib::Response &response)
                                 {
            if (response.status == 404)
                VersionUpdate::sendJson(response, 404, errorBody("not_found", "接口不存在")); });

        // 5. 启动HTTP服务
        std::cout << "Codeit version server listening on " << ListenHost << ':' << ListenPort
                  << ", package root: " << VersionUpdate::getPackageRoot() << std::endl;
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
