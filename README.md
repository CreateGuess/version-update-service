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

服务固定监听 `127.0.0.1:28001`，软件包根目录默认为 `/home/codeit`。目录结构为：

```text
/home/codeit/codeit-deploy_<架构>/<通道>/v1.2.3.zip
/home/codeit/frontend/<通道>/robot-platform/v1.2.3.zip
```

例如：

```text
/home/codeit/codeit-deploy_x86_64/release/v1.2.3.zip
/home/codeit/codeit-deploy_nvidia-orin/test/v1.2.3.zip
/home/codeit/frontend/test/robot-platform/v2.0.0.zip
```

支持：

- 类型：`codeit-deploy`、`frontend`（当前对应 `robot-platform`）
- 架构：`x86_64`、`aarch64`、`nvidia-orin`（仅后端软件包使用）
- 通道：`test`、`release`

如需修改软件包根目录，可设置 `CODEIT_PACKAGE_ROOT`。

版本查询使用内存缓存。服务首次收到查询，或最新两个软件包的文件名、大小、修改时间发生变化时，才会重新计算 SHA-256 并更新 `version.json`；软件包没有变化时直接返回缓存结果。

## 接口

```text
curl http://127.0.0.1:28001/health

curl http://127.0.0.1:28001/api/version/codeit-deploy/x86_64/release
curl "http://127.0.0.1:28001/api/version/codeit-deploy/nvidia-orin/test"
curl "http://127.0.0.1:28001/api/version/frontend/x86_64/test"
curl "http://127.0.0.1:28001/api/version/frontend/x86_64/release"

curl -O http://127.0.0.1:28001/api/packages/codeit-deploy/x86_64/release/v2.0.0.zip
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
