FROM node:18-bullseye

RUN apt-get update && apt-get install -y \
    python3 \
    make \
    g++ \
    gcc \
    librocksdb-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY package*.json ./
RUN npm install

COPY . .

EXPOSE 5000
CMD ["node", "server.cjs"]
