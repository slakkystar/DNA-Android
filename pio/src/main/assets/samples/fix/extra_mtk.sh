stok="$DNA_TMP/DNA_stok"
src="$START_DIR/samples/hal/mtk"

echo "=== Extra MTK HAL ==="

# odm
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

#
echo "> Step: Copy files"

find "$src" -mindepth 1 -type d | while read -r d; do
    rel="${d#"$src"/}"
    mkdir -p "$odm/$rel"
done

find "$src" -type f | while read -r f; do
    rel="${f#"$src"/}"
    name=$(basename "$f")

    # build.prop is processed separately (merged, not overwritten)
    [ "$name" = "build.prop" ] && continue

    mkdir -p "$odm/$(dirname "$rel")"
    cp -f "$f" "$odm/$rel"
    echo ">> Copied: $rel"
done

# Permissions: absolutely all folders -> 755, all files -> 644, except bin/hw/* -> 755
echo "> Step: Set permissions"

find "$src" -mindepth 1 -type d | while read -r d; do
    rel="${d#"$src"/}"
    chmod 755 "$odm/$rel"
done

find "$src" -type f | while read -r f; do
    rel="${f#"$src"/}"
    name=$(basename "$f")

    [ "$name" = "build.prop" ] && continue

    case "$rel" in
        bin/hw/*)
            chmod 755 "$odm/$rel"
            ;;
        *)
            chmod 644 "$odm/$rel"
            ;;
    esac
done

echo ">> Permissions set"

# Merge build.prop: mtk/build.prop -> to the end of odm/build.prop
if [ -f "$src/build.prop" ]; then
    mkdir -p "$odm"
    [ -f "$odm/build.prop" ] || touch "$odm/build.prop"
    marker="# --- merged from samples/hal/mtk/build.prop ---"
    if grep -qF "$marker" "$odm/build.prop" 2>/dev/null; then
        echo ">> build.prop already merged into odm/build.prop, skipping"
    else
        echo "" >> "$odm/build.prop"
        echo "$marker" >> "$odm/build.prop"
        cat "$src/build.prop" >> "$odm/build.prop"
        chmod 644 "$odm/build.prop"
        echo ">> build.prop merged into odm/build.prop"
    fi
fi

# Merge build.prop: mtk/etc/build.prop -> to the end of odm/etc/build.prop
if [ -f "$src/etc/build.prop" ]; then
    mkdir -p "$odm/etc"
    [ -f "$odm/etc/build.prop" ] || touch "$odm/etc/build.prop"
    marker="# --- merged from samples/hal/mtk/etc/build.prop ---"
    if grep -qF "$marker" "$odm/etc/build.prop" 2>/dev/null; then
        echo ">> build.prop already merged into odm/etc/build.prop, skipping"
    else
        echo "" >> "$odm/etc/build.prop"
        echo "$marker" >> "$odm/etc/build.prop"
        cat "$src/etc/build.prop" >> "$odm/etc/build.prop"
        chmod 644 "$odm/etc/build.prop"
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
