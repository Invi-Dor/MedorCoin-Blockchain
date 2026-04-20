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

# 2. Install Node dependencies FIRST (to utilize Docker cache)
COPY package*.json ./
RUN npm install

# 3. Copy the rest of the code
COPY . .

# 4. Explicitly build the C++ addon
RUN npm run build

# 5. Expose Port and Start
EXPOSE 3000
CMD ["node", "node.cjs"]
