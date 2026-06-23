#!/bin/bash
# Switch documentation language (en/es)
# Usage: ./docs/switch.sh       → toggle language
#        ./docs/switch.sh en    → switch to English
#        ./docs/switch.sh es    → switch to Spanish
set -euo pipefail
DOCS="$(cd "$(dirname "$0")" && pwd)"
LANG_FILE="${DOCS}/.lang"

# Read current language
if [ -f "$LANG_FILE" ]; then
    CURRENT="$(cat "$LANG_FILE")"
else
    CURRENT="en"
fi

# Determine target language
if [ $# -ge 1 ]; then
    TARGET="$1"
else
    # Toggle
    case "$CURRENT" in
        en) TARGET="es" ;;
        es) TARGET="en" ;;
        *)  TARGET="en" ;;
    esac
fi

case "$TARGET" in
    en|es) ;;
    *) echo "Usage: $0 [en|es]"; exit 1 ;;
esac

# Update symlinks for each doc
COUNT=0
for EN in "${DOCS}"/*.en.md; do
    BASE="$(basename "$EN" .en.md)"
    LINK="${DOCS}/${BASE}.md"

    # Check if symlink already points to target
    if [ -L "$LINK" ] && [ "$(readlink "$LINK")" = "${BASE}.${TARGET}.md" ]; then
        continue
    fi

    # Remove existing symlink or file (but NOT the .en/.es source files)
    rm -f "$LINK"

    # Create symlink to target language
    ln -s "${BASE}.${TARGET}.md" "$LINK"
    COUNT=$((COUNT + 1))
done

echo "$TARGET" > "$LANG_FILE"
echo "[docs] Switched to $TARGET ($COUNT docs updated)"
