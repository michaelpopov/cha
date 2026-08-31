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
if [ "$(stat -c '%a' "$application")" != "755" ]; then
    echo "package check: application directory must have mode 755" >&2
    exit 1
fi

for required in \
    chaweb \
    start-cha.sh \
    import-seed/.env \
    import-seed/app.toml \
    import-seed/workspace.toml \
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

actual_entries=$(find "$application" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort)
expected_entries=$(printf '%s\n' chaweb import-seed start-cha.sh web | LC_ALL=C sort)
if [ "$actual_entries" != "$expected_entries" ]; then
    echo "package check: application directory has unexpected top-level entries" >&2
    printf '%s\n' "$actual_entries" >&2
    exit 1
fi

seed="$application/import-seed"
if ! grep -Eq '^host[[:space:]]*=[[:space:]]*"0\.0\.0\.0"[[:space:]]*$' "$seed/app.toml"; then
    echo "package check: import-seed/app.toml must bind to 0.0.0.0" >&2
    exit 1
fi
config_lines=$(grep -Ev '^[[:space:]]*(#|$)' "$seed/app.toml" | wc -l)
if [ "$config_lines" -ne 2 ] \
    || grep -Eiq 'workspace|provider|logging|api[_-]?key|secret|token' "$seed/app.toml"; then
    echo "package check: import-seed/app.toml must contain only host and port" >&2
    exit 1
fi

for required_directory in characters forums system; do
    if [ ! -d "$seed/$required_directory" ]; then
        echo "package check: import seed is missing $required_directory/" >&2
        exit 1
    fi
done

seed_entries=$(find "$seed" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort)
expected_seed_entries=$(printf '%s\n' .env app.toml characters forums system workspace.toml | LC_ALL=C sort)
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
if [ "$(stat -c '%a' "$seed/.env")" != "600" ]; then
    echo "package check: import-seed/.env must have mode 600" >&2
    exit 1
fi

if find "$application" -type f \( \
    -name '*.sqlite3' -o -name '*.sqlite' -o -name '*.db' \
    -o -name '*-wal' -o -name '*-shm' -o -name '*-journal' \
    -o -name '*.cha-lock' \) -print -quit | grep -q .; then
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
launcher_host=$(launcher_setting HOST)
launcher_port=$(launcher_setting PORT)
launcher_database=$(launcher_setting DATABASE)
launcher_import_seed=$(launcher_setting IMPORT_SEED)
config_port=$(sed -n 's/^port[[:space:]]*=[[:space:]]*//p' "$seed/app.toml")
if [ "$launcher_host" != "$(shell_quote 0.0.0.0)" ] \
    || [ "$launcher_port" != "$(shell_quote "$config_port")" ] \
    || [ "$launcher_database" != "$(shell_quote ../cha.sqlite3)" ] \
    || [ "$launcher_import_seed" != "$(shell_quote import-seed)" ]; then
    echo "package check: launcher settings do not match the package layout" >&2
    exit 1
fi
if grep -q -- '--workspace' "$application/start-cha.sh" \
    || ! grep -Fq -- '--data "$database"' "$application/start-cha.sh" \
    || ! grep -Fq -- \
        'echo "  \"$here/chaweb\" --data \"$database\" --import \"$import_seed\""' \
        "$application/start-cha.sh"; then
    echo "package check: launcher must use explicit database and import arguments" >&2
    exit 1
fi

if ! find "$application/web/assets" -maxdepth 1 -type f -print -quit 2>/dev/null | grep -q .; then
    echo "package check: web/assets has no production files" >&2
    exit 1
fi

echo "Package integrity check passed: $application"
