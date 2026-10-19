FROM node:22-alpine

WORKDIR /app

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

EXPOSE 6800/tcp
EXPOSE 6801/udp

CMD ["node", "src/index.js"]
