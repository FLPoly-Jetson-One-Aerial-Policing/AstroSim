# Build and test image for the AstroSim orchestrator. This file is for
# building and testing only: the orchestrator itself runs natively on the
# host, never in a container.
#
# Build context is the repository root (the wire tests read
# shared/proto/testdata). From the repo root:
#
#   podman build -f orchestrator/build.Dockerfile --target test .
#   podman build -f orchestrator/build.Dockerfile --target dist -o build/orchestrator .
#
# orchestrator/build.sh wraps both.

ARG GO_IMAGE=docker.io/library/golang:1.26
FROM ${GO_IMAGE} AS base
WORKDIR /src
# Golden vectors live outside the module; tests find them at
# ../../../shared/proto/testdata relative to internal/wire.
COPY shared/proto/testdata /src/shared/proto/testdata
COPY orchestrator/ /src/orchestrator/
WORKDIR /src/orchestrator

# Fails the build on formatting drift, vet or staticcheck findings, or test
# failures. staticcheck is pinned so a new release cannot break the build.
FROM base AS test
ARG STATICCHECK_VERSION=2026.1
RUN go install honnef.co/go/tools/cmd/staticcheck@${STATICCHECK_VERSION}
RUN test -z "$(gofmt -l .)" || { echo "gofmt needed on:"; gofmt -l .; exit 1; }
RUN go vet ./...
RUN /go/bin/staticcheck ./...
RUN go test -race -count=1 ./...

# Cross-compiles every supported target. CGO is disabled so the binaries are
# static and so any cgo or OS-specific call outside a build-tagged file is
# caught here rather than on a user's machine.
FROM base AS build
ARG VERSION=dev
RUN set -eu; \
    mkdir -p /out; \
    for target in linux/amd64 linux/arm64 darwin/amd64 darwin/arm64 windows/amd64; do \
        os="${target%/*}"; arch="${target#*/}"; ext=""; \
        if [ "$os" = windows ]; then ext=".exe"; fi; \
        echo "building ${os}/${arch}"; \
        CGO_ENABLED=0 GOOS="$os" GOARCH="$arch" \
            go build -trimpath -ldflags "-s -w -X main.version=${VERSION}" \
                -o "/out/astrosim-${os}-${arch}${ext}" ./cmd/astrosim; \
    done

# Scratch stage holding only the artifacts, for `--target dist -o <dir>`.
FROM scratch AS dist
COPY --from=build /out/ /
