#!/bin/sh
# Substitute environment variables into config.json at container startup
if [ -n "$MYSQL_USER" ]; then
    sed -i "s|\"user\": \"\"|\"user\": \"$MYSQL_USER\"|g" /app/config.json
fi
if [ -n "$MYSQL_PASSWORD" ]; then
    sed -i "s|\"password\": \"\"|\"password\": \"$MYSQL_PASSWORD\"|g" /app/config.json
fi

exec ./simple_server -c config.json "$@"
