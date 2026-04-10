#!/bin/bash
# 1. Kill any existing Redis processes
sudo pkill -9 redis-server || true
rm -f nodes-6379.conf nodes-6380.conf nodes-6381.conf

# 2. Start all three nodes in cluster mode
redis-server --port 6379 --cluster-enabled yes --daemonize yes
redis-server --port 6380 --cluster-enabled yes --daemonize yes
redis-server --port 6381 --cluster-enabled yes --daemonize yes

# 3. Create the cluster and auto-accept
sleep 3
redis-cli --cluster create 127.0.0.1:6379 127.0.0.1:6380 127.0.0.1:6381 --cluster-yes

echo "MedorCoin Redis Cluster is READY."
