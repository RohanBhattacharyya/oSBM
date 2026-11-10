#!/usr/bin/env bash
set -euo pipefail
sudo apt-get update
sudo apt-get install -y \
  ninja-build \
  pkg-config \
  build-essential \
  autoconf \
  autoconf-archive \
  automake \
  libtool \
  libltdl-dev \
  libasound2-dev \
  libpulse-dev \
  libx11-dev \
  libxext-dev \
  libxrandr-dev \
  libxcursor-dev \
  libxfixes-dev \
  libxi-dev \
  libxss-dev \
  libxtst-dev \
  libxkbcommon-dev \
  libdrm-dev \
  libgbm-dev \
  libgl1-mesa-dev \
  libglu1-mesa-dev \
  libegl1-mesa-dev \
  libdbus-1-dev \
  libudev-dev \
  libwayland-dev \
  libdecor-0-dev
