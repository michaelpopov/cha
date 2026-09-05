#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <application-directory>" >&2
    exit 2
fi

application=$1

if [ ! -d "$application" ]; then
    echo "package check: no application directory at $application" >&2
    exit 1
fi
file_mode() {
    if stat -c '%a' "$1" >/dev/null 2>&1; then
        stat -c '%a' "$1"
    else
        stat -f '%Lp' "$1"
    fi
}

if [ "$(file_mode "$application")" != "755" ]; then
    echo "package check: application directory must have mode 755" >&2
    exit 1
fi

for required in \
    chaweb \
    start-cha.sh \
    cha.toml.example \
    import-seed/.env \
    web/index.html; do
    if [ ! -f "$application/$required" ]; then
        echo "package check: missing $required" >&2
        exit 1
    fi
done

if [ ! -x "$application/chaweb" ] || [ ! -x "$application/start-cha.sh" ]; then
    echo "package check: chaweb and start-cha.sh must be executable" >&2
    exit 1
fi

actual_entries=$(find "$application" -mindepth 1 -maxdepth 1 -exec basename {} \; | LC_ALL=C sort)
expected_entries=$(printf '%s\n' chaweb cha.toml.example import-seed start-cha.sh web | LC_ALL=C sort)
if [ "$actual_entries" != "$expected_entries" ]; then
    echo "package check: application directory has unexpected top-level entries" >&2
    printf '%s\n' "$actual_entries" >&2
    exit 1
fi

config="$application/cha.toml.example"
if ! grep -Eq '^data[[:space:]]*=[[:space:]]*"cha\.sqlite3"[[:space:]]*$' "$config" \
    || ! grep -Eq '^\[web\][[:space:]]*$' "$config" \
    || ! grep -Eq '^host[[:space:]]*=[[:space:]]*"0\.0\.0\.0"[[:space:]]*$' "$config" \
    || ! grep -Eq '^port[[:space:]]*=[[:space:]]*8086[[:space:]]*$' "$config" \
    || ! grep -Eq '^\[logging\][[:space:]]*$' "$config"; then
    echo "package check: cha.toml.example is not the expected unified config" >&2
    exit 1
fi

seed="$application/import-seed"

for required_directory in characters forums system; do
    if [ ! -d "$seed/$required_directory" ]; then
        echo "package check: import seed is missing $required_directory/" >&2
        exit 1
    fi
done

seed_entries=$(find "$seed" -mindepth 1 -maxdepth 1 -exec basename {} \; | LC_ALL=C sort)
expected_seed_entries=$(printf '%s\n' .env characters forums system | LC_ALL=C sort)
if [ "$seed_entries" != "$expected_seed_entries" ]; then
    echo "package check: import seed contains unexpected top-level entries" >&2
    printf '%s\n' "$seed_entries" >&2
    exit 1
fi

unexpected_seed_file=$(find "$seed" -type f \
    ! -name '.env' ! -iname '*.toml' ! -iname '*.md' -print -quit)
if [ -n "$unexpected_seed_file" ]; then
    echo "package check: import seed contains unsupported file $unexpected_seed_file" >&2
    exit 1
fi

if find "$seed" -mindepth 2 -name '.env' -print -quit | grep -q . \
    || find "$seed" ! -type d ! -type f -print -quit | grep -q .; then
    echo "package check: import seed contains a nested .env or non-regular entry" >&2
    exit 1
fi

env_value=$(grep -Ev '^[[:space:]]*(#|$)' "$seed/.env")
if [ "$env_value" != 'OPENAI_API_KEY=replace-with-your-openai-api-key' ]; then
    echo "package check: import seed must contain only the documented key placeholder" >&2
    exit 1
fi
if [ "$(file_mode "$seed/.env")" != "600" ]; then
    echo "package check: import-seed/.env must have mode 600" >&2
    exit 1
fi

chatgpt="$seed/system/providers/chatgpt/config.toml"
terra="$seed/system/providers/terra/config.toml"
if [ ! -f "$chatgpt" ] || [ ! -f "$terra" ]; then
    echo "package check: import seed is missing required providers" >&2
    exit 1
fi
if ! grep -Eq '^auth[[:space:]]*=[[:space:]]*"openai_subscription"[[:space:]]*$' "$chatgpt" \
    || ! grep -Eq '^host[[:space:]]*=[[:space:]]*"chatgpt.com"[[:space:]]*$' "$chatgpt" \
    || ! grep -Eq '^port[[:space:]]*=[[:space:]]*443[[:space:]]*$' "$chatgpt" \
    || ! grep -Eq '^https[[:space:]]*=[[:space:]]*true[[:space:]]*$' "$chatgpt" \
    || ! grep -Eq '^base_path[[:space:]]*=[[:space:]]*"/backend-api/codex"[[:space:]]*$' "$chatgpt" \
    || ! grep -Eq '^mode[[:space:]]*=[[:space:]]*"net"[[:space:]]*$' "$chatgpt" \
    || ! grep -Eq '^api[[:space:]]*=[[:space:]]*"responses"[[:space:]]*$' "$chatgpt" \
    || ! grep -Eq '^model[[:space:]]*=[[:space:]]*"gpt-5.6-terra"[[:space:]]*$' "$chatgpt" \
    || ! grep -Eq '^stream[[:space:]]*=[[:space:]]*true[[:space:]]*$' "$chatgpt" \
    || ! grep -Eq '^web_search[[:space:]]*=[[:space:]]*"off"[[:space:]]*$' "$chatgpt" \
    || ! grep -Eq '^cache_retention[[:space:]]*=[[:space:]]*"off"[[:space:]]*$' "$chatgpt" \
    || grep -Eq '^api_key_env[[:space:]]*=' "$chatgpt" \
    || grep -Eq '^temperature[[:space:]]*=' "$chatgpt" \
    || grep -Eq '^max_tokens[[:space:]]*=' "$chatgpt"; then
    echo "package check: chatgpt provider is not the subscription seed" >&2
    exit 1
fi
if ! grep -Eq '^api_key_env[[:space:]]*=[[:space:]]*"OPENAI_API_KEY"[[:space:]]*$' "$terra" \
    || grep -Eq '^auth[[:space:]]*=[[:space:]]*"openai_subscription"' "$terra"; then
    echo "package check: terra API-key provider is missing" >&2
    exit 1
fi
if ! grep -Eq '^provider[[:space:]]*=[[:space:]]*"chatgpt"[[:space:]]*$' \
    "$seed/system/assistant/character.toml"; then
    echo "package check: assistant does not select the subscription provider" >&2
    exit 1
fi
for character in epictetus markus_aurelius seneca; do
    if ! grep -Eq '^provider[[:space:]]*=[[:space:]]*"chatgpt"[[:space:]]*$' \
        "$seed/characters/$character/character.toml"; then
        echo "package check: $character does not select the subscription provider" >&2
        exit 1
    fi
done

if find "$application" -type f \( \
    -name '*.sqlite3' -o -name '*.sqlite' -o -name '*.db' \
    -o -name '*-wal' -o -name '*-shm' -o -name '*-journal' \
    -o -name '*.cha-lock' -o -name '*.openai-auth.json' \) -print -quit | grep -q .; then
    echo "package check: a database, sidecar, journal, or lock leaked into the application" >&2
    exit 1
fi

# Matched on the assignment rather than the line, so moving the header does not
# turn this check into a silent pass or a confusing failure.
launcher_setting() {
    sed -n "s/^$1=//p" "$application/start-cha.sh" | head -1
}
shell_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}
launcher_config=$(launcher_setting CONFIG)
launcher_import_seed=$(launcher_setting IMPORT_SEED)
if [ "$launcher_config" != "$(shell_quote ../cha.toml)" ] \
    || [ "$launcher_import_seed" != "$(shell_quote import-seed)" ]; then
    echo "package check: launcher settings do not match the package layout" >&2
    exit 1
fi
if grep -q -- '--workspace' "$application/start-cha.sh" \
    || grep -q -- '--data' "$application/start-cha.sh" \
    || ! grep -Fq -- 'config_setting=${CHA_CONFIG:-"$CONFIG"}' \
        "$application/start-cha.sh" \
    || ! grep -Fq -- '--config="$config"' "$application/start-cha.sh" \
    || ! grep -Fq -- \
        'echo "  \"$here/chaweb\" --config=\"$config\" --import \"$import_seed\""' \
        "$application/start-cha.sh"; then
    echo "package check: launcher must use the external unified config" >&2
    exit 1
fi

if ! find "$application/web/assets" -maxdepth 1 -type f -print -quit 2>/dev/null | grep -q .; then
    echo "package check: web/assets has no production files" >&2
    exit 1
fi

echo "Package integrity check passed: $application"
