#!/bin/bash
# MySQL backup script for Kama-HTTPServer
# Usage: ./scripts/backup-db.sh [output_dir]

set -e

OUT_DIR="${1:-./backups}"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
BACKUP_FILE="$OUT_DIR/inference_platform-$TIMESTAMP.sql.gz"

mkdir -p "$OUT_DIR"

echo "Backing up MySQL (inference_platform) to $BACKUP_FILE ..."

# Requires docker exec access to kama-mysql container
if docker ps --format '{{.Names}}' | grep -q kama-mysql; then
    docker exec kama-mysql mysqldump \
        -u root -p"${MYSQL_ROOT_PASSWORD:-root}" \
        --single-transaction \
        --routines \
        --triggers \
        inference_platform \
        | gzip > "$BACKUP_FILE"
else
    echo "kama-mysql container not found, trying direct connection..."
    mysqldump \
        -h "${MYSQL_HOST:-127.0.0.1}" \
        -u "${MYSQL_USER:-root}" \
        -p"${MYSQL_PASSWORD:-root}" \
        --single-transaction \
        --routines \
        --triggers \
        inference_platform \
        | gzip > "$BACKUP_FILE"
fi

echo "Backup complete: $BACKUP_FILE ($(du -h "$BACKUP_FILE" | cut -f1))"

# Keep only last 7 days
find "$OUT_DIR" -name '*.sql.gz' -mtime +7 -delete
echo "Cleaned backups older than 7 days."
