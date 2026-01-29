# 阶段1：准备 WSL2 6.6.87.2 内核（含 Module.symvers）
FROM ubuntu:24.04 AS kernel-src

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      git build-essential bc flex bison libssl-dev libelf-dev ca-certificates \
      python3 cpio && \
    rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/microsoft/WSL2-Linux-Kernel.git /wsl-kernel && \
    cd /wsl-kernel && \
    git checkout linux-msft-wsl-6.6.87.2 && \
    cp Microsoft/config-wsl .config && \
    make olddefconfig && \
    make -j"$(nproc)" modules_prepare modules

# 阶段2：开发环境
FROM ubuntu:24.04 AS dev
WORKDIR /workspace

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      build-essential git kmod \
      flex bison bc libssl-dev libelf-dev \
      apt-transport-https ca-certificates \
      gdb gdbserver && \
    rm -rf /var/lib/apt/lists/*

# 携带已准备好的内核树（含 Module.symvers）
COPY --from=kernel-src /wsl-kernel /opt/wsl-kernel
ENV KDIR=/opt/wsl-kernel

RUN printf '#!/bin/sh\nexec /bin/bash "$@"\n' > /docker-entrypoint.sh && chmod +x /docker-entrypoint.sh
ENTRYPOINT ["/docker-entrypoint.sh"]