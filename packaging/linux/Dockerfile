FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      curl \
      dpkg-dev \
      file \
      git \
      libobs-dev \
      libsimde-dev \
      ninja-build \
      obs-studio \
      pkg-config \
      qt6-base-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

CMD ["./scripts/package-linux.sh"]
