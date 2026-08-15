# hako studio — the agent, served as static files.
#
# The agent runs in the visitor's browser as WebAssembly, so this image serves a
# page; it never sees a conversation, an API key, or a file. No database, no
# environment to configure, nothing to back up. Host it anywhere, or run it on
# your own machine and point your phone at it.
#
#   docker build -t hako-studio .
#   docker run --rm -p 8080:80 hako-studio
#
# The build compiles hako.c to wasm itself — nothing installed but Docker.

# ---- stage 1: build ---------------------------------------------------------
FROM debian:stable-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        curl ca-certificates xz-utils tar make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN make site \
      WASI_DIR=/opt/wasi-sdk \
      WASI_URL=https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-33/wasi-sdk-33.0-x86_64-linux.tar.gz

# ---- stage 2: serve ---------------------------------------------------------
FROM nginx:alpine
COPY --from=build /src/dist /usr/share/nginx/html

# wasm needs its MIME type; page and wasm move together, so neither is cached.
RUN printf 'types { application/wasm wasm; }\n' > /etc/nginx/conf.d/wasm.conf \
    && sed -i 's|location / {|location / {\n        add_header Cache-Control "no-cache";|' \
       /etc/nginx/conf.d/default.conf

EXPOSE 80
