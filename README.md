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

查询维度：

- `codeit-deploy`、`codeit-lib`：`arch`、`platform`、`os`、`channel`
- `backend`：`name`、`arch`、`channel`
- `frontend`：`name`、`channel`（按照当前目录结构不区分架构）

如需修改软件包根目录，可设置 `CODEIT_PACKAGE_ROOT`，其值应直接指向 `update` 目录。

版本查询使用内存缓存。服务首次收到查询，或最新两个软件包的文件名、大小、修改时间发生变化时，才会重新计算 SHA-256 并尝试更新 `version.json`；软件包没有变化时直接返回缓存结果。如果软件包目录为只读，接口仍会正常返回，`version.json` 仅保存在内存缓存中，并在服务日志中记录警告。

## 接口

```bash
curl http://127.0.0.1:28001/health

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

版本响应中的每个软件包都包含固定下载地址及请求体，例如：

```json
{
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
```

下载软件包：

```bash
curl -sS -X POST http://127.0.0.1:28001/api/package \
  -H 'Content-Type: application/json' \
  -d '{"type":"frontend","name":"robot-platform","channel":"test","filename":"v2.2.0.zip"}' \
  -o v2.2.0.zip
```

## 安装成 systemd 服务

```bash
sudo mkdir -p /opt/version-server
sudo cp build/version-server /opt/version-server/
sudo chown -R codeit:codeit /opt/version-server
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
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

启动服务：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now version-server
```

排查接口错误时可查看服务日志和目录权限：

```bash
sudo journalctl -u version-server -n 50 --no-pager
namei -l /home/codeit/update/frontend/robot-platform/test
sudo -u codeit test -r /home/codeit/update/frontend/robot-platform/test/v2.2.0.zip && echo readable
sudo -u codeit test -w /home/codeit/update/frontend/robot-platform/test && echo writable
```

## 配置 Nginx 转发

```nginx
server
{
    listen 28000;
    server_name _;

    location /api/
    {
        proxy_pass http://127.0.0.1:28001;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_buffering off;
    }

    location = /health
    {
        proxy_pass http://127.0.0.1:28001/health;
    }
}
```
