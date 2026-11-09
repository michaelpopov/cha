#!/usr/bin/env bash

set -euo pipefail

started_at=$SECONDS

usage() {
    echo "Usage: $0 N" >&2
    exit 2
}

run_pi() {
    pi --mode json --no-session --model openrouter/meta/muse-spark-1.3-contributor "$@" |
        tee "$pi_events" |
        show_pi_events

    # Pi continues after some errors. Only the last assistant message gives the result.
    local pi_error
    pi_error=$(jq -nr '
        reduce inputs as $event ({};
            .last = $event.type
            | if $event.type == "message_end" and $event.message.role == "assistant"
              then .assistant = $event.message else . end)
        | if .last != "agent_end" or .assistant == null then
            "Pi did not finish the session."
          elif .assistant.stopReason == "error" or .assistant.stopReason == "aborted" then
            .assistant.errorMessage // .assistant.stopReason
          elif .assistant.stopReason != "stop" then
            "Pi did not return a complete response."
          else empty end
    ' "$pi_events")
    if [[ -n $pi_error ]]; then
        echo "Pi session failed: $pi_error" >&2
        exit 1
    fi
}

show_pi_events() {
    jq --unbuffered -j '
        if .type == "message_update"
            and (.assistantMessageEvent.type == "thinking_delta"
                or .assistantMessageEvent.type == "text_delta") then
            .assistantMessageEvent.delta
        elif .type == "tool_execution_start" then
            "\n[Pi tool] " + .toolName + "\n"
        elif .type == "tool_execution_end" then
            "\n[Pi tool " + (if .isError then "failed" else "done" end) + "] " + .toolName + "\n"
        elif .type == "message_end" and .message.role == "assistant"
            and (.message.stopReason == "error" or .message.stopReason == "aborted") then
            "\n[Pi error] " + (.message.errorMessage // .message.stopReason) + "\n"
        elif .type == "agent_end" then
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

if (( 10#$block_number > 1 )); then
    previous_block_file="docs/block$((10#$block_number - 1)).md"
    if [[ ! -f $previous_block_file ]]; then
        echo "Missing previous block file: $previous_block_file" >&2
        exit 1
    fi
    if ! grep -qw -- COMPLETED "$previous_block_file"; then
        echo "Previous block is not completed: $previous_block_file" >&2
        exit 1
    fi
fi

for command in pi git jq; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

if [[ -n $(git status --short) ]]; then
    echo "Warning: existing changes will also be reviewed and committed." >&2
fi

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/doit.XXXXXX")
trap 'rm -rf -- "$tmp_dir"' EXIT
pi_events="$tmp_dir/pi-events.jsonl"

complete_line='- Status: Complete.'

echo "Implementing block $block_number with Pi..."
run_pi "Implement steps described in $block_file. You may update the Execution record section of $block_file with results and instructions for the next session. Leave other docs/ files unchanged. When all assigned work and validation are complete, change the Status line of the Execution record to exactly '$complete_line'. Do not write that line if work remains or a blocker is unresolved. Leave the changes uncommitted."

if ! awk -v complete="$complete_line" '
    /^### Execution record([[:space:]]|$)/ { in_record = 1; next }
    /^#/ { in_record = 0 }
    in_record && $0 == complete { complete_found = 1 }
    END { exit !complete_found }
' "$block_file"; then
    echo "Block $block_number is not complete: $block_file has no '$complete_line' line." >&2
    exit 1
fi

review_prompt="Uncommitted code changes implement steps described in $block_file. Review the uncommitted code. Use git status and git diff to identify modified, staged, and untracked files, then inspect relevant files as needed. Report only actionable correctness, security, regression, or necessary-test findings. For each finding, give the file, location, problem, and required fix. Do not modify files. Do not include praise, summaries, or optional suggestions. If there are no findings, output exactly NO_FINDINGS."

echo "Reviewing uncommitted changes with Pi..."
run_pi --tools read,bash,grep,find,ls "$review_prompt"

review_output=$(jq -sr '
    [
        .[]
        | select(.type == "message_end" and .message.role == "assistant")
        | [.message.content[]? | select(.type == "text") | .text]
        | join("")
    ]
    | last // ""
' "$pi_events")
if [[ -z $review_output ]]; then
    echo "Pi did not produce a review." >&2
    exit 1
fi

if [[ $review_output == NO_FINDINGS ]]; then
    echo "The review found no issues."
else
    printf -v fix_prompt \
        'Evaluate the code review findings below for the uncommitted changes implementing %s. Fix only findings that are valid and consistent with project requirements and prior decisions. Run relevant tests and leave the changes uncommitted.\n\nCode review findings:\n%s' \
        "$block_file" "$review_output"

    echo "Applying review feedback with Pi..."
    run_pi "$fix_prompt"
fi

printf '\nCOMPLETED\n' >> "$block_file"

echo "Committing block $block_number..."
git add -A
if git diff --cached --quiet; then
    echo "There are no changes to commit." >&2
    exit 1
fi
git commit -m "Block $block_number"

elapsed=$((SECONDS - started_at))
printf 'Completed in %02d:%02d:%02d\n' \
    "$((elapsed / 3600))" "$(((elapsed % 3600) / 60))" "$((elapsed % 60))"
