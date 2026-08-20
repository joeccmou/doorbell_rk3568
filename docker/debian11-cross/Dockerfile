FROM debian:11@sha256:99cdf7792e25416bd801861ccd8e2fb27fb527b25e8d9a8704ebc3ead2015675

ARG DEBIAN_FRONTEND=noninteractive

ARG BUILD_UID=1000
ARG BUILD_GID=1000
ARG BUILD_USER=joeccmou

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        make \
        pkg-config \
        passwd \
        file \
        gcc-aarch64-linux-gnu \
        g++-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu \
        python3 \
        python3-pip \
        python3-venv \
    && python3 -m pip install \
        --no-cache-dir \
        --disable-pip-version-check \
        pcpp==1.30 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd \
        --gid "${BUILD_GID}" \
        "${BUILD_USER}" \
    && useradd \
        --uid "${BUILD_UID}" \
        --gid "${BUILD_GID}" \
        --create-home \
        --shell /bin/bash \
        "${BUILD_USER}"

RUN aarch64-linux-gnu-gcc --version \
    && aarch64-linux-gnu-g++ --version \
    && cmake --version \
    && python3 --version \
    && command -v pcpp

USER ${BUILD_USER}

WORKDIR /home/${BUILD_USER}

CMD ["/bin/bash"]