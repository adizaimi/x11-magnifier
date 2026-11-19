# Build the Debian package in a clean Debian container and smoke-test it by
# installing the resulting .deb into a fresh image.
#
#   docker build --platform linux/amd64 -f packaging/deb.Dockerfile --target test -t x11-loupe-deb .
#
# To copy the .deb out of the build stage:
#   docker build --platform linux/amd64 -f packaging/deb.Dockerfile --target export -o type=local,dest=./dist .

# ---- build the .deb -------------------------------------------------------
FROM debian:bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential debhelper dpkg-dev libx11-dev libxrender-dev libxcomposite-dev libxdamage-dev lintian \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN dpkg-buildpackage -us -uc -b
RUN mkdir -p /out && cp ../x11-loupe_*_*.deb /out/ && lintian /out/*.deb || true

# ---- smoke-test: install into a clean Debian and check it -----------------
FROM debian:bookworm-slim AS test
COPY --from=build /out/*.deb /tmp/
RUN apt-get update \
    && apt-get install -y --no-install-recommends /tmp/*.deb \
    && rm -rf /var/lib/apt/lists/* \
    && dpkg-deb -I /tmp/*.deb \
    && (x11-loupe -h 2>&1 | head -3; true) \
    && dpkg-deb -c /tmp/*.deb | grep -q 'usr/share/man/man1/x11-loupe.1.gz' \
    && echo "OK: installs cleanly, man page packaged"

# ---- optional: export just the .deb --------------------------------------
FROM scratch AS export
COPY --from=build /out/*.deb /
