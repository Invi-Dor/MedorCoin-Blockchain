FROM node:18-bullseye

# 1. Install System Dependencies for C++ and RocksDB
RUN apt-get update && apt-get install -y \
    python3 \
    make \
    g++ \
    gcc \
    librocksdb-dev \
    libsnappy-dev \
    libz-dev \
    libbz2-dev \
    liblz4-dev \
    libzstd-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 2. Copy ALL files first (so binding.gyp and src/ are present)
COPY . .

# 3. Install dependencies and compile the C++ addon
RUN npm install

# 4. Expose Port and Start
EXPOSE 3000
CMD ["node", "node.cjs"]
