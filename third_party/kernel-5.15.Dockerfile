FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    bc \
    bison \
    build-essential \
    cpio \
    debhelper \
    dpkg-dev \
    dwarves \
    fakeroot \
    flex \
    kmod \
    libelf-dev \
    libncurses-dev \
    libssl-dev \
    openssl \
    python3 \
    rsync \
    xz-utils \
    zstd \
    && rm -rf /var/lib/apt/lists/*

ENV KDEB_COMPRESS=gzip

WORKDIR /build

# Mount a Linux 5.15 source tree at /source:ro and an output directory at /output.
# A private copy keeps generated files out of the mounted source tree.
CMD ["bash", "-euc", "mkdir -p /build/linux /output; rsync -a --exclude=.git /source/ /build/linux/; cd /build/linux; make clean; scripts/config --disable DEBUG_INFO; make olddefconfig; make bindeb-pkg -j\"$(nproc)\"; cp -v ../*.deb /output/"]
