FROM node:22-alpine

WORKDIR /app

# Install FFmpeg, espeak-ng, and librespot for native 68k 8-bit PCM audio & TTS streaming
RUN apk add --no-cache ffmpeg espeak-ng && \
    apk add --no-cache librespot --repository=https://dl-cdn.alpinelinux.org/alpine/edge/testing

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

CMD ["node", "src/index.js"]
