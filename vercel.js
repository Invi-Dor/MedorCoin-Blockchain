{
  "version": 2,
  "public": true,
  "rewrites": [
    { "source": "/api/(.*)", "destination": "https://medorcoin.org/api/$1" },
    { "source": "/(.*)", "destination": "/$1" }
  ]
}
