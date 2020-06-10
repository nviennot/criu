#!/bin/bash
set -eux

# Install Rust toolchain
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y

# Clone criu-image-streamer in a sibling directory of the criu project directory
cd $(dirname "$0")/../../../
# TODO change dev branch to master
git clone https://github.com/checkpoint-restore/criu-image-streamer.git -b dev

# Compile
cd criu-image-streamer
make BUILD=debug # debug build compiles faster than release mode (130s vs 230s of CPU time)
