stok="$DNA_TMP/DNA_stok"
src="$START_DIR/samples/hal/vivo"
vendor="$stok/vendor"
vendor_prefix="/vendor"
contexts_target="$stok/config/vendor_file_contexts"

echo "=== Vivo OriginOS HALs ==="

#
echo "> Step: Copy files"

find "$src" -mindepth 1 -type d | while read -r d; do
    rel="${d#"$src"/}"
    mkdir -p "$vendor/$rel"
done

find "$src" -type f | while read -r f; do
    rel="${f#"$src"/}"

    [ "$rel" = "etc/vintf/manifest.xml" ] && continue

    mkdir -p "$vendor/$(dirname "$rel")"
    cp -f "$f" "$vendor/$rel"
    echo ">> Copied: $rel"
done

# Merge manifest.xml
src_manifest="$src/etc/vintf/manifest.xml"
target_manifest="$vendor/etc/vintf/manifest.xml"

if [ -f "$src_manifest" ] && [ -f "$target_manifest" ]; then
    # check if Vivo's HAL is already in the manifest
    if grep -q "vendor.vivo.hardware" "$target_manifest" || grep -q "vendor.factory.hardware.vivoem" "$target_manifest"; then
        echo ">> Vivo HALs already present in manifest.xml, skipping"
    else
        echo "> Step: Append manifest.xml"
        
        echo "" >> "$target_manifest"
        cat "$src_manifest" >> "$target_manifest"

        echo ">> Manifest appended to $target_manifest"
    fi
else
    echo ">> Warning: manifest.xml for merging was not found!"
fi

# Permissions: absolutely all folders -> 755, all files -> 644, except bin/hw/* -> 755
echo "> Step: Set permissions"

find "$src" -mindepth 1 -type d | while read -r d; do
    rel="${d#"$src"/}"
    chmod 755 "$vendor/$rel"
done

find "$src" -type f | while read -r f; do
    rel="${f#"$src"/}"
    case "$rel" in
        bin/hw/*)
            chmod 755 "$vendor/$rel"
            ;;
        *)
            chmod 644 "$vendor/$rel"
            ;;
    esac
done

echo ">> Permissions set"

# Automatically create a selinux context for each file in bin/hw
mkdir -p "$(dirname "$contexts_target")"
[ -f "$contexts_target" ] || touch "$contexts_target"

if [ -d "$src/bin/hw" ]; then
    for file in "$src/bin/hw"/*; do
        [ -f "$file" ] || continue
        name=$(basename "$file")
        escaped_name=$(echo "$name" | sed 's/\./\\./g')
        context_line="$vendor_prefix/bin/hw/$escaped_name u:object_r:hal_allocator_default_exec:s0"

        if grep -qF "$context_line" "$contexts_target" 2>/dev/null; then
            echo ">> Context already present for $name, skipping"
        else
            echo "$context_line" >> "$contexts_target"
            echo ">> Context added: $context_line"
        fi
    done
fi

echo "=== Done ==="
