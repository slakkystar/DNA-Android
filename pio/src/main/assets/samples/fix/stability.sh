stok="$DNA_DRO"
src="$START_DIR/samples/hal/stability"

echo "=== PowerButton Delay Fix Started ==="

if [ ! -d "$src" ]; then
    echo ">> Error: source folder not found: $src"
    exit 1
fi

if [ -d "$stok/odm" ]; then
    odm="$stok/odm"
    contexts_src="$src/odm"
    contexts_target="$stok/config/odm_file_contexts"
    echo ">> ODM found: $odm"
elif [ -d "$stok/vendor/odm" ]; then
    odm="$stok/vendor/odm"
    contexts_src="$src/vendor"
    contexts_target="$stok/config/vendor_file_contexts"
    echo ">> ODM found: $odm"
else
    echo ">> Error: ODM partition not found (neither $stok/odm nor $stok/vendor/odm exist)"
    exit 1
fi

map="etc/init:644 etc/permissions:644 etc/vintf/manifest:644 framework:644 lib:644 lib64:644 bin/hw:755"

for entry in $map; do
    rel="${entry%%:*}"
    perm="${entry##*:}"

    if [ ! -d "$src/$rel" ]; then
        continue
    fi

    mkdir -p "$odm/$rel"

    for file in "$src/$rel"/*; do
        [ -f "$file" ] || continue
        name=$(basename "$file")
        cp -f "$file" "$odm/$rel/$name"
        chmod "$perm" "$odm/$rel/$name"
        echo ">> Copied: $rel/$name (chmod $perm)"
    done
done

if [ ! -f "$contexts_src" ]; then
    echo ">> Error: contexts file not found: $contexts_src"
    exit 1
fi

context_line=$(cat "$contexts_src")

if grep -qF "$context_line" "$contexts_target" 2>/dev/null; then
    echo ">> Context already present in $(basename "$contexts_target"), skipping"
else
    echo "$context_line" >> "$contexts_target"
    echo ">> Context added to $(basename "$contexts_target")"
fi

echo "=== Done ==="
