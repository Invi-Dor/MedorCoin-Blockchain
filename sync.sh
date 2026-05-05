#!/bin/bash
# PASTE YOUR CLOUDFLARE DATA HERE
TOKEN="dc44f27f48ab405392a5f69fe822bd01"
ZONE_ID="ff271ad4d84357b78d1e622894bd74b4"
DOMAIN="medorcoin.org"

IP=$(curl -s https://ipify.org)
REC_ID=$(curl -s -X GET "https://cloudflare.com" -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" | python3 -c "import sys, json; print(json.load(sys.stdin)['result'][0]['id'])")
curl -s -X PUT "https://cloudflare.com" -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" --data "{\"type\":\"A\",\"name\":\"$DOMAIN\",\"content\":\"$IP\",\"proxied\":true}"
echo "SUCCESS: Medorcoin.org linked to $IP"
