module.exports = {
  apps: [
    { name: "redis-6379", script: "redis-server", args: "--port 6379 --cluster-enabled yes --daemonize no" },
    { name: "redis-6380", script: "redis-server", args: "--port 6380 --cluster-enabled yes --daemonize no" },
    { name: "redis-6381", script: "redis-server", args: "--port 6381 --cluster-enabled yes --daemonize no" },
    { name: "medor-engine", script: "npm start", delay: 5001 }
  ]
};
