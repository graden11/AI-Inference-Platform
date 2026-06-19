#!/bin/sh
# Substitute environment variables into config.json at container startup.
# Copy to /tmp first — the original may be a bind-mounted file and
# sed -i on bind mounts can fail with "Device or resource busy".
CONFIG_SRC="/app/config.json"
CONFIG_TMP="/tmp/config.json"

cp "$CONFIG_SRC" "$CONFIG_TMP"

if [ -n "$MYSQL_USER" ]; then
    sed -i "s|\"user\": \"[^\"]*\"|\"user\": \"$MYSQL_USER\"|g" "$CONFIG_TMP"
fi
if [ -n "$MYSQL_PASSWORD" ]; then
    sed -i "s|\"password\": \"[^\"]*\"|\"password\": \"$MYSQL_PASSWORD\"|g" "$CONFIG_TMP"
fi
if [ -n "$REDIS_HOST" ]; then
    sed -i "/\"redis\": {/,/}/{s|\"host\": \"[^\"]*\"|\"host\": \"$REDIS_HOST\"|}" "$CONFIG_TMP"
fi
if [ -n "$REDIS_PORT" ]; then
    sed -i "/\"redis\": {/,/}/{s|\"port\": [0-9]*|\"port\": $REDIS_PORT|}" "$CONFIG_TMP"
fi

exec ./simple_server -c "$CONFIG_TMP" "$@"
