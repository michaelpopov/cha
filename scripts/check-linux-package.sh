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

for required in chaweb start-cha.sh app.toml web/index.html; do
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
expected_entries=$(printf '%s\n' app.toml chaweb start-cha.sh web | LC_ALL=C sort)
if [ "$actual_entries" != "$expected_entries" ]; then
    echo "package check: application directory has unexpected top-level entries" >&2
    printf '%s\n' "$actual_entries" >&2
    exit 1
fi

if ! grep -Eq '^host[[:space:]]*=[[:space:]]*"0\.0\.0\.0"[[:space:]]*$' "$application/app.toml"; then
    echo "package check: app.toml must bind to 0.0.0.0" >&2
    exit 1
fi
config_lines=$(grep -Ev '^[[:space:]]*(#|$)' "$application/app.toml" | wc -l)
if [ "$config_lines" -ne 3 ] \
    || grep -Eiq 'provider|logging|api[_-]?key|secret|token' "$application/app.toml"; then
    echo "package check: app.toml must contain only host, port, and workspace" >&2
    exit 1
fi

for forbidden in workspace.toml .env characters forums personas logs; do
    if find "$application" -name "$forbidden" -print -quit | grep -q .; then
        echo "package check: workspace-owned '$forbidden' leaked into the application" >&2
        exit 1
    fi
done

if find "$application" \( -name '*.sqlite3' -o -name '*.cha-lock' \) -print -quit | grep -q .; then
    echo "package check: stored workspace data leaked into the application" >&2
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
launcher_workspace=$(launcher_setting WORKSPACE)
config_port=$(sed -n 's/^port[[:space:]]*=[[:space:]]*//p' "$application/app.toml")
config_workspace=$(sed -n 's/^workspace[[:space:]]*=[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$application/app.toml")
if [ "$launcher_host" != "$(shell_quote 0.0.0.0)" ] \
    || [ "$launcher_port" != "$(shell_quote "$config_port")" ] \
    || [ "$launcher_workspace" != "$(shell_quote "$config_workspace")" ]; then
    echo "package check: launcher settings do not match app.toml" >&2
    exit 1
fi

if ! find "$application/web/assets" -maxdepth 1 -type f -print -quit 2>/dev/null | grep -q .; then
    echo "package check: web/assets has no production files" >&2
    exit 1
fi

echo "Package integrity check passed: $application"
