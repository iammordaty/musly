FROM debian:trixie-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        pkg-config \
        libeigen3-dev \
        libavcodec-dev \
        libavformat-dev \
        libavutil-dev \
        ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# BUILD_STATIC builds a static libmusly; FFmpeg remains shared (Debian packages).
# If static linking of the client fails against shared FFmpeg, drop -DBUILD_STATIC=1.
RUN mkdir -p build && cd build && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_STATIC=1 \
        -DUSE_OPENMP=1 \
        -DBUILD_TEST=ON \
        .. && \
    make -j"$(nproc)" && \
    ctest --output-on-failure && \
    make install && \
    chmod +x /src/test/decoder_smoke.sh && \
    /src/test/decoder_smoke.sh && \
    make install DESTDIR=/staging && \
    ldconfig

FROM debian:trixie-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV TERM=xterm

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        libavcodec61 \
        libavformat61 \
        libavutil59 \
        libgomp1 \
        openssh-server \
        ffmpeg \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /staging/ /

RUN echo "root:root" | chpasswd && \
    mkdir -p /var/run/sshd && \
    ldconfig

VOLUME /collection
VOLUME /metadata

WORKDIR /metadata

EXPOSE 22

CMD [ "/usr/sbin/sshd", "-D", "-e", "-o", "LogLevel=info", "-o", "PermitRootLogin=yes" ]
