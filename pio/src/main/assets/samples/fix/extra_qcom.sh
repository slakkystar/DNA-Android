stok="$DNA_DRO"
src="$START_DIR/samples/hal/qcom"

echo "=== Extra QCOM HAL ==="

if [ -d "$stok/odm" ]; then
    odm="$stok/odm"
    odm_prefix="/odm"
    contexts_target="$stok/config/odm_file_contexts"
    echo ">> ODM found: $odm"
elif [ -d "$stok/vendor/odm" ]; then
    odm="$stok/vendor/odm"
    odm_prefix="/vendor/odm"
    contexts_target="$stok/config/vendor_file_contexts"
    echo ">> ODM found: $odm"
else
    echo ">> Error: ODM partition not found (neither $stok/odm nor $stok/vendor/odm exist)"
    exit 1
fi

# Copy the files and set the permissions (bin/hw -> 755, everything else -> 644)
map="bin/hw:755 lib:644 etc:644 lib64:644 firmware:644"

for entry in $map; do
    rel="${entry%%:*}"
    perm="${entry##*:}"

    [ -d "$src/$rel" ] || continue

    mkdir -p "$odm/$rel"

    for file in "$src/$rel"/*; do
        [ -f "$file" ] || continue
        name=$(basename "$file")
        # build.prop is processed separately (merged, not overwritten)
        [ "$name" = "build.prop" ] && continue
        cp -f "$file" "$odm/$rel/$name"
        chmod "$perm" "$odm/$rel/$name"
        echo ">> Copied: $rel/$name (chmod $perm)"
    done
done

# Merge build.prop: qcom/build.prop -> to the end of odm/build.prop
if [ -f "$src/build.prop" ]; then
    mkdir -p "$odm"
    [ -f "$odm/build.prop" ] || touch "$odm/build.prop"
    marker="# --- merged from samples/hal/qcom/build.prop ---"
    if grep -qF "$marker" "$odm/build.prop" 2>/dev/null; then
        echo ">> build.prop already merged into odm/build.prop, skipping"
    else
        echo "" >> "$odm/build.prop"
        echo "$marker" >> "$odm/build.prop"
        cat "$src/build.prop" >> "$odm/build.prop"
        echo ">> build.prop merged into odm/build.prop"
    fi
fi

# Merge build.prop: qcom/etc/build.prop -> to the end of odm/etc/build.prop
if [ -f "$src/etc/build.prop" ]; then
    mkdir -p "$odm/etc"
    [ -f "$odm/etc/build.prop" ] || touch "$odm/etc/build.prop"
    marker="# --- merged from samples/hal/qcom/etc/build.prop ---"
    if grep -qF "$marker" "$odm/etc/build.prop" 2>/dev/null; then
        echo ">> build.prop already merged into odm/etc/build.prop, skipping"
    else
        echo "" >> "$odm/etc/build.prop"
        echo "$marker" >> "$odm/etc/build.prop"
        cat "$src/etc/build.prop" >> "$odm/etc/build.prop"
        echo ">> build.prop merged into odm/etc/build.prop"
    fi
fi

# Automatically create a selinux context for each file in bin/hw
mkdir -p "$(dirname "$contexts_target")"
[ -f "$contexts_target" ] || touch "$contexts_target"

if [ -d "$src/bin/hw" ]; then
    for file in "$src/bin/hw"/*; do
        [ -f "$file" ] || continue
        name=$(basename "$file")
        escaped_name=$(echo "$name" | sed 's/\./\\./g')
        context_line="$odm_prefix/bin/hw/$escaped_name u:object_r:hal_allocator_default_exec:s0"

        if grep -qF "$context_line" "$contexts_target" 2>/dev/null; then
            echo ">> Context already present for $name, skipping"
        else
            echo "$context_line" >> "$contexts_target"
            echo ">> Context added: $context_line"
        fi
    done
fi

echo "=== Done ==="
