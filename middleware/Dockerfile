FROM node:22-alpine

WORKDIR /app

# Install FFmpeg, espeak-ng, sox, and librespot for native 68k 8-bit PCM audio & TTS streaming,
# plus install the custom GLaDOS formant voice variant into eSpeak NG's !v/ folder
RUN apk add --no-cache ffmpeg espeak-ng sox && \
    apk add --no-cache librespot --repository=https://dl-cdn.alpinelinux.org/alpine/edge/testing && \
    mkdir -p /usr/share/espeak-ng-data/voices/\!v && \
    printf "name glados\nlanguage variant\ngender female 30\npitch 178 215\nformant 0 100 100 100\nformant 1 100 100 100\nformant 2 106 100 90\nformant 3 110 110 75\nformant 4 106 120 65\nformant 5 108 120 65\nformant 6 110 100 80\nformant 7 110 100 80\nformant 8 110 100 80\necho 14 22\nflutter 4\nroughness 0\nvoicing 90\nconsonants 100 115\nwords 1 0\n" > /usr/share/espeak-ng-data/voices/\!v/glados

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

EXPOSE 6800/tcp
EXPOSE 6801/udp
EXPOSE 6802/tcp
EXPOSE 6803/tcp

CMD ["node", "src/index.js"]
