FROM docker.io/library/ubuntu:24.04 AS build

ARG TD_VERSION=3.4.3.9
ARG TD_JOBS=4
ARG TD_BUILD_ROCKSDB=OFF

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    autoconf automake build-essential ca-certificates cmake curl git libcurl4-openssl-dev \
    libssl-dev libtext-template-perl libtool libxml2-dev perl pkg-config python3 xz-utils zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /src
RUN /src/packaging/ubuntu-arm64/build.sh /build \
    && install -Dm644 /src/packaging/cfg/taos.cfg /image/etc/taos/taos.cfg \
    && printf '\nudf 0\n' >> /image/etc/taos/taos.cfg \
    && install -Dm644 /src/LICENSE /image/usr/share/doc/tdengine/LICENSE

FROM docker.io/library/ubuntu:24.04 AS runtime

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates libgcc-s1 libstdc++6 tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 taos \
    && useradd --uid 10001 --gid taos --no-create-home --shell /usr/sbin/nologin taos \
    && install -d -o taos -g taos /var/lib/taos /var/log/taos

COPY --from=build /image/ /
COPY --from=build /build/build/bin/taosd /build/build/bin/taos /usr/local/bin/
COPY --from=build /build/build/lib/libtaos.so /build/build/lib/libtaosnative.so /usr/local/lib/
RUN ldconfig \
    && for artifact in /usr/local/bin/taosd /usr/local/bin/taos /usr/local/lib/libtaos.so /usr/local/lib/libtaosnative.so; do \
         deps=$(ldd "$artifact") && ! printf '%s\n' "$deps" | grep -q 'not found' || exit 1; \
       done

USER taos
EXPOSE 6030/tcp
RUN taosd -V && taos -V
ENTRYPOINT ["/usr/local/bin/taosd"]
CMD ["-c", "/etc/taos"]
