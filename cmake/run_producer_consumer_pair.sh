#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  run_producer_consumer_pair.sh --producer PATH --consumer PATH [--mode background|fifo]
                                [--ready-pattern TEXT] [--ready-timeout SEC]

The script starts a producer, waits for a readiness marker, runs the consumer,
then waits for the producer to finish. In fifo mode it keeps stdin open for
interactive producers and sends a newline after the consumer completes.
EOF
}

producer=""
consumer=""
mode="background"
ready_pattern=""
ready_timeout="20"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --producer)
            producer="$2"
            shift 2
            ;;
        --consumer)
            consumer="$2"
            shift 2
            ;;
        --mode)
            mode="$2"
            shift 2
            ;;
        --ready-pattern)
            ready_pattern="$2"
            shift 2
            ;;
        --ready-timeout)
            ready_timeout="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$producer" || -z "$consumer" ]]; then
    usage >&2
    exit 2
fi

if [[ "$mode" != "background" && "$mode" != "fifo" ]]; then
    echo "Unsupported mode: $mode" >&2
    exit 2
fi

tmp_dir="$(mktemp -d)"
producer_log="$tmp_dir/producer.log"
consumer_log="$tmp_dir/consumer.log"
producer_pid=""
fifo_path=""
fifo_fd_open=0

cleanup() {
    if [[ -n "$producer_pid" ]] && kill -0 "$producer_pid" 2>/dev/null; then
        if [[ "$mode" == "fifo" && "$fifo_fd_open" -eq 1 ]]; then
            printf '\n' >&3 || true
            exec 3>&- 3<&- || true
            fifo_fd_open=0
        fi
        kill "$producer_pid" 2>/dev/null || true
        wait "$producer_pid" 2>/dev/null || true
    fi

    rm -rf "$tmp_dir"
}

fail_with_logs() {
    local message="$1"
    echo "$message" >&2
    if [[ -f "$producer_log" ]]; then
        echo "--- producer log ---" >&2
        cat "$producer_log" >&2 || true
    fi
    if [[ -f "$consumer_log" ]]; then
        echo "--- consumer log ---" >&2
        cat "$consumer_log" >&2 || true
    fi
    cleanup
    exit 1
}

trap cleanup EXIT INT TERM

if [[ "$mode" == "fifo" ]]; then
    fifo_path="$tmp_dir/producer.stdin"
    mkfifo "$fifo_path"
    exec 3<>"$fifo_path"
    fifo_fd_open=1
    "$producer" >"$producer_log" 2>&1 <"$fifo_path" &
else
    "$producer" >"$producer_log" 2>&1 &
fi

producer_pid=$!

if [[ -n "$ready_pattern" ]]; then
    deadline=$((SECONDS + ready_timeout))
    while ! grep -F -q -- "$ready_pattern" "$producer_log" 2>/dev/null; do
        if ! kill -0 "$producer_pid" 2>/dev/null; then
            fail_with_logs "Producer exited before readiness pattern appeared: $ready_pattern"
        fi
        if (( SECONDS >= deadline )); then
            fail_with_logs "Timed out waiting for readiness pattern: $ready_pattern"
        fi
        sleep 0.2
    done
else
    sleep 1
fi

if ! "$consumer" >"$consumer_log" 2>&1; then
    fail_with_logs "Consumer exited with failure"
fi

if [[ "$mode" == "fifo" ]]; then
    printf '\n' >&3
    exec 3>&- 3<&-
    fifo_fd_open=0
fi

if ! wait "$producer_pid"; then
    fail_with_logs "Producer exited with failure"
fi

echo "producer/consumer pair passed"
