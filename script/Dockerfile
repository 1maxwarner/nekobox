# Void Linux (glibc)
FROM ghcr.io/void-linux/void-glibc:latest

ARG TARGETPLATFORM
ARG BUILDPLATFORM

RUN echo "FINE"
RUN echo "I am running on $BUILDPLATFORM, building for $TARGETPLATFORM"

# Update and install in one retried transaction. Void mirrors can briefly return
# 404 while per-architecture repository indexes are being synchronized.
RUN set -eux; \
    installed=0; \
    for mirror in \
      https://repo-fastly.voidlinux.org \
      https://repo-default.voidlinux.org; do \
      sed -i "s|https://repo-[^/]\\+\\.voidlinux\\.org|$mirror|g" /usr/share/xbps.d/*-repository-*.conf; \
      if xbps-install -Syu -y \
        bash cmake git ninja jq curl ccache wget sed util-linux coreutils shadow squashfs-tools \
        qt6-base-devel thrift-devel openssl-devel boost-devel lmdb-devel acl-devel libunistring-devel \
        libcurl-devel libzstd-devel patchelf pkg-config qt6-base qt6-tools go thrift base-devel \
        tar xz which file yaml-cpp-devel leveldb-devel; then \
        installed=1; \
        break; \
      fi; \
    done; \
    test "$installed" = 1

WORKDIR /app

CMD ["bash", "-x", "/app/script/build_docker.sh"]
