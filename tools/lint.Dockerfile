# Lint toolchain for tools/lint.sh: every tool is pinned, so CI and developers get identical results.
#
# Build and run it through tools/lint.sh; the build context is this directory (it needs no repository files).
#
# Pins and where they come from:
#   clang-format   PyPI wheel, the same version as the developers' clang-format.
#   hadolint       GitHub release binary, sha256 from the release asset digests.
#   actionlint     GitHub release binary, sha256 from actionlint_<version>_checksums.txt.
#   shellcheck,
#   doxygen        Ubuntu packages (the base image is pinned by tag, so these only move with it).
#
# To bump a tool: change its version and hashes here, run tools/lint.sh, and commit the result.
ARG UBUNTU_TAG=26.04
FROM docker.io/library/ubuntu:${UBUNTU_TAG}

ARG CLANG_FORMAT_VERSION=22.1.8
ARG HADOLINT_VERSION=2.15.1
ARG HADOLINT_SHA256_AMD64=c7187db94eeeeca956519a6af171adc31453941a1e777961f6e680f697c8c507
ARG HADOLINT_SHA256_ARM64=f6198ef8090f404dbb771abfee086eb8c48ac177f30da7fd3510aca35b344b5d
ARG ACTIONLINT_VERSION=1.7.12
ARG ACTIONLINT_SHA256_AMD64=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8
ARG ACTIONLINT_SHA256_ARM64=325e971b6ba9bfa504672e29be93c24981eeb1c07576d730e9f7c8805afff0c6

# hadolint ignore=DL3008
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl doxygen python3-venv shellcheck \
    && rm -rf /var/lib/apt/lists/*

# clang-format lives in a virtual environment (PEP 668 forbids installing into the system Python).
RUN python3 -m venv /opt/clang-format \
    && /opt/clang-format/bin/pip install --no-cache-dir "clang-format==${CLANG_FORMAT_VERSION}" \
    && ln -s /opt/clang-format/bin/clang-format /usr/local/bin/clang-format

# hadolint and actionlint are downloaded as release binaries and refused unless the sha256 matches.
RUN arch="$(dpkg --print-architecture)" \
    && case "${arch}" in \
        amd64) hadolint_arch=x86_64; hadolint_sha="${HADOLINT_SHA256_AMD64}"; actionlint_sha="${ACTIONLINT_SHA256_AMD64}" ;; \
        arm64) hadolint_arch=arm64; hadolint_sha="${HADOLINT_SHA256_ARM64}"; actionlint_sha="${ACTIONLINT_SHA256_ARM64}" ;; \
        *) echo "unsupported architecture ${arch}" >&2; exit 1 ;; \
    esac \
    && curl -fsSL -o /usr/local/bin/hadolint \
        "https://github.com/hadolint/hadolint/releases/download/v${HADOLINT_VERSION}/hadolint-linux-${hadolint_arch}" \
    && echo "${hadolint_sha}  /usr/local/bin/hadolint" > /tmp/hadolint.sha256 \
    && sha256sum -c /tmp/hadolint.sha256 \
    && chmod 0755 /usr/local/bin/hadolint \
    && curl -fsSL -o /tmp/actionlint.tar.gz \
        "https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_${arch}.tar.gz" \
    && echo "${actionlint_sha}  /tmp/actionlint.tar.gz" > /tmp/actionlint.sha256 \
    && sha256sum -c /tmp/actionlint.sha256 \
    && tar -xzf /tmp/actionlint.tar.gz -C /usr/local/bin actionlint \
    && rm /tmp/actionlint.tar.gz /tmp/*.sha256

# The repository is mounted here; tools/lint.sh runs inside it.
WORKDIR /repo
