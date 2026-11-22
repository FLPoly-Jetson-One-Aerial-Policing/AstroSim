# AstroSim orchestrator: test, cross-compile and export stages.
#
# Build context is the repository root (the wire tests read
# shared/proto/testdata), e.g. from the repo root:
#
#   podman build -f orchestrator/Dockerfile --target test .
#   podman build -f orchestrator/Dockerfile --target dist -o build/orchestrator .

ARG GO_IMAGE=docker.io/library/golang:1.26
FROM ${GO_IMAGE} AS base
WORKDIR /src
# Golden vectors live outside the module; tests find them at
# ../../../shared/proto/testdata relative to internal/wire.
COPY shared/proto/testdata /src/shared/proto/testdata
COPY orchestrator/ /src/orchestrator/
WORKDIR /src/orchestrator

# Fails the build on formatting drift, vet findings or test failures.
FROM base AS test
RUN test -z "$(gofmt -l .)" || { echo "gofmt needed on:"; gofmt -l .; exit 1; }
RUN go vet ./...
RUN go test -race -count=1 ./...

# Cross-compiles every supported target. CGO is disabled so the binaries are
# static and so any cgo or OS-specific call outside a build-tagged file is
# caught here rather than on a user's machine.
FROM base AS build
ARG VERSION=dev
# TODO: until ./cmd/astrosim exists this only compile-checks ./... per target.
# When it lands, replace the `go build ./...` below with
#   go build -trimpath -ldflags "..." -o /out/astrosim-${os}-${arch}${ext} ./cmd/astrosim
RUN set -eu; \
    mkdir -p /out; \
    for target in linux/amd64 linux/arm64 darwin/amd64 darwin/arm64 windows/amd64; do \
        os="${target%/*}"; arch="${target#*/}"; \
        echo "building ${os}/${arch}"; \
        CGO_ENABLED=0 GOOS="$os" GOARCH="$arch" \
            go build -trimpath -ldflags "-s -w -X main.version=${VERSION}" ./...; \
    done

# Scratch stage holding only the artifacts, for `--target dist -o <dir>`.
FROM scratch AS dist
COPY --from=build /out/ /
