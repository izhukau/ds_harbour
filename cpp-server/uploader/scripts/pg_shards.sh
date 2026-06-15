#!/usr/bin/env bash
set -euo pipefail

PGBIN="$(brew --prefix postgresql@17)/bin"
ROOT="${PGSHARD_ROOT:-/tmp/ds_harbour_shards}"
DB="${PGSHARD_DB:-payments}"
BASE_PORT="${PGSHARD_BASE_PORT:-5441}"

cmd="${1:-help}"
N="${2:-2}"

start_shards() {
    mkdir -p "$ROOT"
    for i in $(seq 0 $((N - 1))); do
        port=$((BASE_PORT + i))
        dir="$ROOT/shard$i"
        if [ ! -d "$dir" ]; then
            "$PGBIN/initdb" -A trust -U postgres -D "$dir" >/dev/null
        fi
        if ! "$PGBIN/pg_ctl" -D "$dir" status >/dev/null 2>&1; then
            "$PGBIN/pg_ctl" -D "$dir" -o "-p $port" -l "$dir/server.log" -w start >/dev/null
        fi
        "$PGBIN/createdb" -h localhost -p "$port" -U postgres "$DB" 2>/dev/null || true
    done
    echo "shards up. connection strings:"
    conn_strings
}

conn_strings() {
    for i in $(seq 0 $((N - 1))); do
        port=$((BASE_PORT + i))
        echo "  \"host=localhost port=$port dbname=$DB user=postgres\""
    done
}

stop_shards() {
    for i in $(seq 0 $((N - 1))); do
        dir="$ROOT/shard$i"
        if [ -d "$dir" ]; then
            "$PGBIN/pg_ctl" -D "$dir" -m fast stop >/dev/null 2>&1 || true
        fi
    done
    echo "shards stopped"
}

destroy_shards() {
    stop_shards
    rm -rf "$ROOT"
    echo "shard data removed from $ROOT"
}

case "$cmd" in
up) start_shards ;;
down) stop_shards ;;
destroy) destroy_shards ;;
conn) conn_strings ;;
*)
    echo "usage: $0 {up|down|destroy|conn} [num_shards]"
    echo "  up N       initialize and start N postgres shard instances (default 2)"
    echo "  down N     stop N shard instances"
    echo "  destroy N  stop and delete all shard data"
    echo "  conn N     print N connection strings"
    ;;
esac
