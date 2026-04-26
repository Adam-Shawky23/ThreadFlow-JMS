#!/bin/bash
#
# jms_script.sh
# Statistics and management script for jms output directories.
#
# Usage: ./jms_script.sh -l <path> -c <command>
#   command can be:
#     list        — list all job output directories
#     size [n]    — list directories sorted by size (largest first if n given)
#     purge       — delete all job output directories

# ── Parse arguments (order not fixed) ──────────────────
path=""
command=""

while getopts "l:c:" opt; do
    case $opt in
        l) path="$OPTARG" ;;
        c) command="$OPTARG" ;;
        *)
            echo "Usage: ./jms_script.sh -l <path> -c <command>"
            exit 1
            ;;
    esac
done

# ── Validate ────────────────────────────────────────────
if [ -z "$path" ] || [ -z "$command" ]; then
    echo "Usage: ./jms_script.sh -l <path> -c <command>"
    exit 1
fi

if [ ! -d "$path" ]; then
    echo "Error: directory '$path' does not exist."
    exit 1
fi

# ── Dispatch ────────────────────────────────────────────

# Extract base command word (handles "size 3" as a single -c argument)
base_cmd=$(echo "$command" | awk '{print $1}')
n_arg=$(echo "$command"    | awk '{print $2}')  # may be empty

case "$base_cmd" in

    list)
        # List all output directories created by jobs
        dirs=$(ls -d "$path"/outputs_* 2>/dev/null)
        if [ -z "$dirs" ]; then
            echo "No job output directories found in '$path'."
        else
            echo "$dirs"
        fi
        ;;

    size)
        # Sort directories by total file size (ascending), show n largest if given
        dirs=$(ls -d "$path"/outputs_* 2>/dev/null)
        if [ -z "$dirs" ]; then
            echo "No job output directories found in '$path'."
            exit 0
        fi

        # du -s: gives a single summary line per directory (kilobytes, dir name)
        sorted=$(du -s "$path"/outputs_* 2>/dev/null | sort -n)

        if [ -n "$n_arg" ] && [ "$n_arg" -eq "$n_arg" ] 2>/dev/null; then
            # Show only the n largest: tail grabs from the bottom of the sorted list
            echo "$sorted" | tail -n "$n_arg"
        else
            echo "$sorted"
        fi
        ;;

    purge)
        # Delete all job output directories
        count=$(ls -d "$path"/outputs_* 2>/dev/null | wc -l)
        if [ "$count" -eq 0 ]; then
            echo "Nothing to purge."
        else
            rm -rf "$path"/outputs_*
            echo "Purged $count job output director(ies)."
        fi
        ;;

    *)
        echo "Unknown command: '$command'"
        echo "Valid commands: list, size [n], purge"
        exit 1
        ;;
esac