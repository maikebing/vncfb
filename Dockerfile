# 阶段1：拉取并准备 WSL2 6.6.87.2 内核（仅做 modules_prepare）
FROM ubuntu:24.04 AS kernel-src

# 替换为清华 HTTP 源
RUN cat > /etc/apt/sources.list <<'EOF'
deb http://mirrors.tuna.tsinghua.edu.cn/ubuntu/ noble main restricted universe multiverse
deb http://mirrors.tuna.tsinghua.edu.cn/ubuntu/ noble-updates main restricted universe multiverse
deb http://mirrors.tuna.tsinghua.edu.cn/ubuntu/ noble-backports main restricted universe multiverse
deb http://mirrors.tuna.tsinghua.edu.cn/ubuntu/ noble-security main restricted universe multiverse
EOF

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      git build-essential bc flex bison libssl-dev libelf-dev ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# 获取并准备 WSL2 内核
RUN git clone https://github.com/microsoft/WSL2-Linux-Kernel.git /wsl-kernel && \
    cd /wsl-kernel && \
    git checkout linux-msft-wsl-6.6.87.2 && \
    cp Microsoft/config-wsl .config && \
    make olddefconfig && \
    make prepare modules_prepare

# 阶段2：开发环境，包含编译工具与已准备好的内核树
FROM ubuntu:24.04 AS dev
WORKDIR /workspace

# 替换为清华 HTTP 源
RUN cat > /etc/apt/sources.list <<'EOF'
deb http://mirrors.tuna.tsinghua.edu.cn/ubuntu/ noble main restricted universe multiverse
deb http://mirrors.tuna.tsinghua.edu.cn/ubuntu/ noble-updates main restricted universe multiverse
deb http://mirrors.tuna.tsinghua.edu.cn/ubuntu/ noble-backports main restricted universe multiverse
deb http://mirrors.tuna.tsinghua.edu.cn/ubuntu/ noble-security main restricted universe multiverse
EOF

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      build-essential git kmod \
      flex bison bc libssl-dev libelf-dev \
      apt-transport-https ca-certificates \
      gdb gdbserver && \
    rm -rf /var/lib/apt/lists/*

# 复制已 modules_prepare 的内核树，供 KDIR 使用
COPY --from=kernel-src /wsl-kernel /opt/wsl-kernel
ENV KDIR=/opt/wsl-kernel

# 简单入口
RUN printf '#!/bin/sh\nexec /bin/bash "$@"\n' > /docker-entrypoint.sh && chmod +x /docker-entrypoint.sh
ENTRYPOINT ["/docker-entrypoint.sh"]