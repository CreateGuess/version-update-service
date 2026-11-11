# Codeit Version Server

## 安装依赖并编译

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    libssl-dev \
    nlohmann-json3-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

服务固定监听 `127.0.0.1:28001`，软件包根目录默认为 `/home/codeit/update`。目录结构为：

```text
/home/codeit/update/codeit-deploy/<架构>/<平台>/<系统>/<通道>/v1.2.3.zip
/home/codeit/update/codeit-lib/<架构>/<平台>/<系统>/<通道>/v1.2.3.zip
/home/codeit/update/backend/<组件>/<架构>/<通道>/v1.2.3.zip
/home/codeit/update/frontend/<组件>/<通道>/v1.2.3.zip
```

例如：

```text
/home/codeit/update/codeit-deploy/aarch64/rk3588/ubuntu-22.04/test/v1.2.3.zip
/home/codeit/update/codeit-deploy/x86_64/generic/ubuntu-22.04/test/v1.2.3.zip
/home/codeit/update/codeit-lib/x86_64/generic/ubuntu-22.04/test/v1.2.3.zip
/home/codeit/update/backend/rpc_gateway/x86_64/test/v1.2.3.zip
/home/codeit/update/frontend/robot-platform/test/v2.0.0.zip
```

软件包文件名支持以下格式：

```text
v<主版本>.<次版本>.<修订版本>.zip
<软件名>-<主版本>.<次版本>.<修订版本>.zip
<软件名>-v<主版本>.<次版本>.<修订版本>.zip
```

例如 `v2.0.0.zip`、`codeit-1.3.48.zip` 和 `codeit-v2.10.3.zip` 都能被识别。`2.0.0.zip`、`v2.0.zip` 和 `.tar.gz` 文件不会被识别。版本按数字比较，因此 `v2.10.0` 高于 `v2.9.0`。`filename` 始终保留磁盘上的原文件名；`codeit-lib` 的 `version` 和 `latest_version` 也直接使用完整文件名（如 `codeit-1.3.48.zip`），其他类型仍使用 `vX.Y.Z`。

查询维度：

- `codeit-deploy`、`codeit-lib`：`arch`、`platform`、`os`、`channel`
- `backend`：`name`、`arch`、`channel`
- `frontend`：`name`、`channel`（按照当前目录结构不区分架构）

如需修改软件包根目录，可设置 `CODEIT_PACKAGE_ROOT`，其值应直接指向 `update` 目录。

版本查询使用内存缓存。服务首次收到查询，或最新两个软件包的文件名、大小、修改时间发生变化时，才会重新计算 SHA-256 并尝试更新软件包目录中的 `version.json`；软件包没有变化时直接返回缓存结果。如果软件包目录为只读，接口仍会正常返回，`version.json` 仅保存在内存缓存中，并在服务日志中记录警告。运行用户至少需要对 ZIP 文件有读取权限；如需生成 `version.json`，还需要对软件包目录有写入权限。

## API 接口总览

服务默认地址为 `http://127.0.0.1:28001`。配置 Nginx 后，对外地址通常为 `http://<服务器地址>:28000`。

| HTTP 方法 | URL | 用途 | 请求体 |
| --- | --- | --- | --- |
| `GET` | `/health` | 服务健康检查 | 无 |
| `POST` | `/api/version` | 查询指定目录下最新的两个版本 | JSON |
| `POST` | `/api/package` | 下载指定版本的软件包 | JSON |
| `GET` | `/api/files?path=<相对目录>` | 网页使用的目录与 ZIP 列表 | 无 |
| `GET` | `/api/files/download?path=<相对文件>` | 浏览器直接下载 ZIP | 无 |
| `POST` | `/api/files/upload?path=<相对目录>` | 上传 ZIP 到指定目录 | `multipart/form-data` |

`/api/version` 和 `/api/package` 必须使用请求头：

```http
Content-Type: application/json
```

## Web 管理页面

项目的 `web/` 目录包含无需构建的管理页面。部署后访问：

```text
http://127.0.0.1:28001/
```

通过本文 Nginx 配置访问：

```text
http://<服务器地址>:28000/
```

页面提供：

- 从 `/home/codeit/update` 开始逐级浏览目录；
- 显示当前目录下所有符合命名规则的 ZIP、识别版本和文件大小；
- 点击 ZIP 直接下载；
- 将最大 2 GiB 的 ZIP 流式上传到当前目录；
- 显示上传进度，并拒绝覆盖同名文件。

服务以 `codeit` 用户运行时，上传目标目录必须允许该用户写入。如果整个 `update` 目录都由该服务管理，可统一设置：

```bash
sudo chown -R codeit:codeit /home/codeit/update
sudo find /home/codeit/update -type d -exec chmod 755 {} \;
sudo find /home/codeit/update -type f -name '*.zip' -exec chmod 644 {} \;
```

网页接口中的 `path` 始终是相对于软件包根目录的路径，例如：

```text
codeit-lib/x86_64/generic/ubuntu-20.04/test
```

服务会拒绝绝对路径、反斜杠、`.`、`..` 以及通过符号链接跳出软件包根目录的路径。

### 上传令牌

可选环境变量 `CODEIT_UPLOAD_TOKEN` 用于保护网页上传接口。配置后，页面上传时必须在“上传令牌”输入框中填写相同内容；页面只把令牌保存在当前浏览器标签页的 `sessionStorage` 中。

```ini
Environment=CODEIT_UPLOAD_TOKEN=请替换为足够长的随机令牌
```

如果未配置该变量，任何能访问服务的人都能上传文件。生产环境强烈建议配置令牌，并通过 HTTPS 使用管理页面。

也可以不经过页面，直接调用上传接口：

```bash
curl -fS -X POST \
  'http://127.0.0.1:28001/api/files/upload?path=codeit-lib%2Fx86_64%2Fgeneric%2Fubuntu-20.04%2Ftest' \
  -H 'Authorization: Bearer 请替换为上传令牌' \
  -F 'file=@codeit-1.3.49.zip'
```

### JSON 字段说明

| 字段 | 含义 | 示例 |
| --- | --- | --- |
| `type` | 软件包大类 | `codeit-deploy`、`codeit-lib`、`backend`、`frontend` |
| `name` | 后端或前端组件名 | `rpc_gateway`、`robot-platform` |
| `arch` | CPU 架构 | `x86_64`、`aarch64` |
| `platform` | 硬件平台 | `generic`、`rk3588` |
| `os` | 操作系统或系统版本 | `ubuntu-22.04` |
| `channel` | 发布通道 | `test`、`release` |
| `filename` | 下载接口指定的文件名 | `v2.2.0.zip` |

所有字段值必须是字符串。目录字段只允许英文字母、数字、点、下划线和连字符，并且必须以英文字母或数字开头。`/`、反斜杠、空格及 `..` 路径不允许使用。

### 各软件包类型的参数要求

“—”表示必须省略该字段或传空字符串。

| `type` | `name` | `arch` | `platform` | `os` | `channel` | 最终目录 |
| --- | --- | --- | --- | --- | --- | --- |
| `codeit-deploy` | — | 必填 | 必填 | 必填 | 必填 | `codeit-deploy/<arch>/<platform>/<os>/<channel>` |
| `codeit-lib` | — | 必填 | 必填 | 必填 | 必填 | `codeit-lib/<arch>/<platform>/<os>/<channel>` |
| `backend` | 必填 | 必填 | — | — | 必填 | `backend/<name>/<arch>/<channel>` |
| `frontend` | 必填 | — | — | — | 必填 | `frontend/<name>/<channel>` |

按照当前目录结构，前端区分组件和渠道，不区分架构。

## GET /health

检查服务是否正在运行：

```bash
curl -i http://127.0.0.1:28001/health
```

成功时返回：

```json
{
  "status": "ok"
}
```

## POST /api/version

查询指定软件包目录。服务只返回版本号最高的两个合法 ZIP 文件，不在该接口中传输 ZIP 内容。

### 请求示例

```bash
# codeit-deploy
curl -sS -X POST http://127.0.0.1:28001/api/version \
  -H 'Content-Type: application/json' \
  -d '{"type":"codeit-deploy","arch":"x86_64","platform":"generic","os":"ubuntu-22.04","channel":"test"}'

# codeit-lib
curl -sS -X POST http://127.0.0.1:28001/api/version \
  -H 'Content-Type: application/json' \
  -d '{"type":"codeit-lib","arch":"x86_64","platform":"generic","os":"ubuntu-22.04","channel":"test"}'

# backend/rpc_gateway
curl -sS -X POST http://127.0.0.1:28001/api/version \
  -H 'Content-Type: application/json' \
  -d '{"type":"backend","name":"rpc_gateway","arch":"x86_64","channel":"test"}'

# frontend/robot-platform
curl -sS -X POST http://127.0.0.1:28001/api/version \
  -H 'Content-Type: application/json' \
  -d '{"type":"frontend","name":"robot-platform","channel":"test"}'
```

### 成功响应

`size` 单位为字节，`sha256` 是 ZIP 文件的 SHA-256 十六进制值，`generated_at` 使用 UTC 时间。每个软件包的 `download` 对象给出了下载接口、HTTP 方法和完整请求体。

```json
{
  "query": {
    "type": "frontend",
    "name": "robot-platform",
    "channel": "test"
  },
  "latest_version": "v2.2.0",
  "packages": [
    {
      "version": "v2.2.0",
      "is_latest": true,
      "filename": "v2.2.0.zip",
      "size": 117153665,
      "sha256": "文件的SHA-256值",
      "download": {
        "method": "POST",
        "url": "/api/package",
        "body": {
          "type": "frontend",
          "name": "robot-platform",
          "channel": "test",
          "filename": "v2.2.0.zip"
        }
      }
    }
  ],
  "generated_at": "2026-09-03T03:00:00Z"
}
```

客户端可以用 `latest_version` 判断是否需要升级，再从 `packages` 中选择对应版本并按照 `download` 对象调用下载接口。

## POST /api/package

下载指定 ZIP。请求参数与 `/api/version` 相同，并额外要求 `filename`。`filename` 必须使用前述任一合法软件包命名格式，并且必须与磁盘上的文件名完全一致。

### 下载前端软件包

```bash
curl -fS -X POST http://127.0.0.1:28001/api/package \
  -H 'Content-Type: application/json' \
  -d '{"type":"frontend","name":"robot-platform","channel":"test","filename":"v2.2.0.zip"}' \
  -o v2.2.0.zip
```

### 下载 codeit-deploy 软件包

```bash
curl -fS -X POST http://127.0.0.1:28001/api/package \
  -H 'Content-Type: application/json' \
  -d '{"type":"codeit-deploy","arch":"x86_64","platform":"generic","os":"ubuntu-22.04","channel":"test","filename":"v2.2.0.zip"}' \
  -o v2.2.0.zip
```

成功时响应头类似：

```http
HTTP/1.1 200 OK
Content-Type: application/zip
Content-Disposition: attachment; filename="v2.2.0.zip"
Cache-Control: public, max-age=3600
```

下载后可以执行：

```bash
sha256sum v2.2.0.zip
```

结果应当与 `/api/version` 返回的 `sha256` 一致。

## 错误响应

错误统一返回 JSON：

```json
{
  "error": "错误码",
  "message": "错误说明"
}
```

| HTTP 状态 | `error` | 说明 |
| --- | --- | --- |
| `400` | `invalid_json` | 请求体不是合法 JSON，或 JSON 顶层不是对象 |
| `400` | `invalid_parameters` | 字段不是字符串、必填字段缺失、字段组合错误或含非法路径字符 |
| `400` | `invalid_upload` | 上传文件名不合法、文件过大或目标目录已有同名文件 |
| `401` | `unauthorized` | 服务配置了上传令牌，但请求未提供正确令牌 |
| `404` | `package_not_found` | 目录不存在、目录不可读、没有合法 ZIP，或指定下载文件不存在 |
| `404` | `directory_not_found` | 网页浏览或上传的目标目录不存在、越界或不可访问 |
| `404` | `not_found` | URL 或 HTTP 方法没有匹配任何接口 |
| `500` | `directory_read_failed` | 网页目录遍历过程中发生错误 |
| `500` | `upload_failed` | 上传临时文件无法保存或重命名 |
| `500` | `internal_error` | 读取文件、计算 SHA-256 等操作发生内部异常 |

例如，`GET /api/version` 会返回 `not_found`，因为版本接口只接受 `POST`；正确 URL 但没有找到软件包时返回 `package_not_found`。

## 安装成 systemd 服务

```bash
sudo install -d -o codeit -g codeit /opt/version-server
sudo install -m 755 build/version-server /opt/version-server/version-server
sudo install -d -o codeit -g codeit /opt/version-server/web
sudo cp -R web/. /opt/version-server/web/
sudo vim /etc/systemd/system/version-server.service
```

`version-server.service`：

```ini
[Unit]
Description=Codeit Version Server
After=network.target

[Service]
Type=simple
User=codeit
Group=codeit
ExecStart=/opt/version-server/version-server
# 默认根目录已经是 /home/codeit/update；使用其他目录时取消下一行注释并修改
# Environment=CODEIT_PACKAGE_ROOT=/home/codeit/update
Environment=CODEIT_WEB_ROOT=/opt/version-server/web
# 生产环境建议设置上传令牌
# Environment=CODEIT_UPLOAD_TOKEN=请替换为足够长的随机令牌
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

启动服务：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now version-server
sudo systemctl status version-server --no-pager
```

更新二进制：

```bash
cmake --build build --clean-first -j
sudo systemctl stop version-server
sudo install -m 755 build/version-server /opt/version-server/version-server
sudo cp -R web/. /opt/version-server/web/
sudo systemctl start version-server
```

## 排查问题

查看日志、监听端口及实际运行程序：

```bash
sudo journalctl -u version-server -n 100 --no-pager
sudo ss -ltnp 'sport = :28001'
pid=$(systemctl show -p MainPID --value version-server)
sudo readlink -f "/proc/$pid/exe"
```

实际程序应为 `/opt/version-server/version-server`。确认编译产物已经部署：

```bash
sha256sum build/version-server /opt/version-server/version-server
```

两个 SHA-256 应一致。手动运行程序之前必须先停止 systemd 服务，否则旧进程仍会占用 `28001`。

检查目录和权限：

```bash
package_dir=/home/codeit/update/codeit-deploy/x86_64/generic/ubuntu-22.04/test
namei -l "$package_dir"
sudo -u codeit ls -la "$package_dir"
sudo -u codeit test -r "$package_dir/v2.2.0.zip" && echo readable
sudo -u codeit test -w "$package_dir" && echo writable
```

检查 curl 实际发送的方法和路径：

```bash
curl -v \
  --url http://127.0.0.1:28001/api/version \
  --header 'Content-Type: application/json' \
  --data-raw '{"type":"codeit-deploy","arch":"x86_64","platform":"generic","os":"ubuntu-22.04","channel":"test"}'
```

详细输出中应看到：

```text
> POST /api/version HTTP/1.1
```

URL 必须是纯地址，不能写成 `[地址](地址)` 形式；JSON 中应写 `x86_64`，不能写成 `x86\_64`。

## 配置 Nginx 转发

```nginx
server
{
    listen 28000;
    server_name _;
    client_max_body_size 2g;

    location /api/
    {
        proxy_pass http://127.0.0.1:28001;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_buffering off;
        proxy_request_buffering off;
        proxy_read_timeout 3600s;
        proxy_send_timeout 3600s;
    }

    location = /health
    {
        proxy_pass http://127.0.0.1:28001/health;
    }

    location /
    {
        proxy_pass http://127.0.0.1:28001;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
    }
}
```

检查并加载配置：

```bash
sudo nginx -t
sudo systemctl reload nginx
```

通过 Nginx 调用时将端口换成 `28000`，JSON 参数保持不变：

```bash
curl -sS -X POST http://127.0.0.1:28000/api/version \
  -H 'Content-Type: application/json' \
  -d '{"type":"frontend","name":"robot-platform","channel":"test"}'
```
