#!/usr/bin/env bash

set -euo pipefail

usage() {
    echo "Usage: $0 N" >&2
    exit 2
}

run_grok() {
    local prompt=$1

    grok --always-approve --output-format streaming-json --single "$prompt" |
        jq --unbuffered -j '
            if .type == "thought" or .type == "text" then
                .data
            elif .type == "tool_call" then
                "\n[Grok tool] " + (.title // .toolName // .kind // "unknown") + "\n"
            elif .type == "error" then
                "\n[Grok error] " + (.message // "unknown error") + "\n"
            elif .type == "end" then
                "\n"
            else
                empty
            end
        '
}

if [[ $# -ne 1 || ! $1 =~ ^[0-9]+$ ]]; then
    usage
fi

block_number=$1
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(git -C "$script_dir" rev-parse --show-toplevel)
block_file="docs/block${block_number}.md"

cd "$repo_root"

if [[ ! -f $block_file ]]; then
    echo "Missing block file: $block_file" >&2
    exit 1
fi

for command in grok pi git jq; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

if [[ -n $(git status --short) ]]; then
    echo "Warning: existing changes will also be reviewed and committed." >&2
fi

echo "Implementing block $block_number with Grok..."
run_grok "Implement steps described in $block_file"

review_dir=$(mktemp -d "${TMPDIR:-/tmp}/doit.XXXXXX")
trap 'rm -rf -- "$review_dir"' EXIT
review_events="$review_dir/pi-review.jsonl"

echo "Reviewing uncommitted changes with Pi..."
review_prompt="Uncommitted code changes implement steps described in $block_file. Review the uncommitted code. Use git status and git diff to identify modified, staged, and untracked files, then inspect relevant files as needed. Report only actionable correctness, security, regression, or necessary-test findings. For each finding, give the file, location, problem, and required fix. Do not modify files. Do not include praise, summaries, or optional suggestions. If there are no findings, output exactly NO_FINDINGS."
pi --mode json --no-session --tools read,bash,grep,find,ls \
    "$review_prompt" |
    tee "$review_events" |
    jq --unbuffered -j '
        if .type == "message_update"
            and (.assistantMessageEvent.type == "thinking_delta"
                or .assistantMessageEvent.type == "text_delta") then
            .assistantMessageEvent.delta
        elif .type == "tool_execution_start" then
            "\n[Pi tool] " + .toolName + "\n"
        elif .type == "tool_execution_end" then
            "\n[Pi tool " + (if .isError then "failed" else "done" end) + "] " + .toolName + "\n"
        elif .type == "agent_end" then
            "\n"
        else
            empty
        end
    '

review_output=$(jq -sr '
    [
        .[]
        | select(.type == "message_end" and .message.role == "assistant")
        | [.message.content[]? | select(.type == "text") | .text]
        | join("")
    ]
    | last // ""
' "$review_events")
if [[ -z $review_output ]]; then
    echo "Pi did not produce a review." >&2
    exit 1
fi

printf -v fix_prompt \
    'Address the code review comments below for the uncommitted changes implementing %s. Make only the necessary fixes, run relevant tests, and leave the changes uncommitted. If the review says NO_FINDINGS, verify the current changes and do not invent work.\n\nCode review comments:\n%s' \
    "$block_file" "$review_output"

echo "Applying review feedback with Grok..."
run_grok "$fix_prompt"

echo "Committing block $block_number..."
git add -A
if git diff --cached --quiet; then
    echo "There are no changes to commit." >&2
    exit 1
fi
git commit -m "Block $block_number"
