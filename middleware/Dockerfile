FROM rust:1-alpine AS librespot-builder

RUN apk add --no-cache build-base git

WORKDIR /build/librespot
# Build librespot dev + PR #1870 (fixes Spotify's spotify:webapi / transfer 400 Bad Request)
RUN git init && \
    git remote add origin https://github.com/librespot-org/librespot.git && \
    git fetch --depth 20 origin 15e02e86c72d96dd39d548e3749f00abe39ffdde && \
    git fetch --depth 20 origin refs/pull/1870/head && \
    git checkout afb2e6aa6ead6206089141b46e7b88134657e87d && \
    cargo build --release --locked --no-default-features --features "rustls-tls-webpki-roots,with-libmdns"


FROM node:22-alpine AS flimmaker-builder

RUN apk add --no-cache g++ make ffmpeg-dev

WORKDIR /build/flimmaker
COPY native/flimmaker ./
RUN make -j"$(nproc)" CXX=g++


FROM node:22-alpine

WORKDIR /app

# Install FFmpeg, espeak-ng, and sox for native 68k 8-bit PCM audio & TTS streaming,
# plus install the custom GLaDOS formant voice variant into eSpeak NG's !v/ folder
RUN apk add --no-cache ffmpeg espeak-ng sox ca-certificates && \
    mkdir -p /usr/share/espeak-ng-data/voices/\!v && \
    printf "name glados\nlanguage variant\ngender female 30\npitch 178 215\nformant 0 100 100 100\nformant 1 100 100 100\nformant 2 106 100 90\nformant 3 110 110 75\nformant 4 106 120 65\nformant 5 108 120 65\nformant 6 110 100 80\nformant 7 110 100 80\nformant 8 110 100 80\necho 14 22\nflutter 4\nroughness 0\nvoicing 90\nconsonants 100 115\nwords 1 0\n" > /usr/share/espeak-ng-data/voices/\!v/glados

# Install patched librespot binary and MacFlim encoder binary built in multi-stage builders
COPY --from=librespot-builder /build/librespot/target/release/librespot /usr/local/bin/librespot
COPY --from=flimmaker-builder /build/flimmaker/flimmaker /usr/local/bin/flimmaker

# Install dependencies first for layer caching
COPY package*.json ./
RUN npm install --omit=dev

# Copy application source
COPY src ./src

# Create persistent volume mount point
RUN mkdir -p /app/data
ENV DATA_DIR=/app/data
ENV PORT=6800
ENV UDP_DISCOVERY_PORT=6801
ENV AUDIO_TCP_PORT=6802
ENV FLIMMAKER_BIN=/usr/local/bin/flimmaker

EXPOSE 6800/tcp
EXPOSE 6801/udp
EXPOSE 6802/tcp
EXPOSE 6803/tcp
EXPOSE 6804/udp
EXPOSE 5050/udp

CMD ["node", "src/index.js"]
