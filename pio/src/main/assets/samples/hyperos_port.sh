port="$DNA_TMP/DNA_port"
stok="$DNA_TMP/DNA_stok"

echo "=== HyperOS Auto-Port ==="

#
if [ ! -d "$port" ]; then
    echo ">> Error: donor project (port) not found: $port"
    exit 1
fi
if [ ! -d "$stok" ]; then
    echo ">> Error: stock project (stok) not found: $stok"
    exit 1
fi

#
echo "Device features"
src="$stok/product/etc/device_features"
dst="$port/product/etc/device_features"

if [ -d "$src" ]; then
    cp -rf "$src"/. "$dst"/
    echo ">> Device features copied"
else
    echo ">> Skip: $src not found"
fi

#
echo "Display config"
src="$stok/product/etc/displayconfig"
dst="$port/product/etc/displayconfig"
if [ -d "$src" ]; then
    cp -rf "$src"/. "$dst"/
    echo ">> Display config copied"
else
    echo ">> Skip: $src not found"
fi

#
echo "Merge mi_ext build.prop"
mi_ext_prop="$port/mi_ext/etc/build.prop"
product_prop="$port/product/etc/build.prop"
if [ -f "$mi_ext_prop" ]; then
    echo "" >> "$product_prop"
    echo "# --- merged from mi_ext/etc/build.prop ---" >> "$product_prop"
    cat "$mi_ext_prop" >> "$product_prop"
    echo ">> build.prop merged into product/etc/build.prop"
else
    echo ">> Skip: $mi_ext_prop not found"
fi

#
echo "Merge mi_ext/product into product"
src="$port/mi_ext/product"
dst="$port/product"
if [ -d "$src" ]; then
    cp -rf "$src"/. "$dst"/
    echo ">> mi_ext/product copied into product"
else
    echo ">> Skip: $src not found"
fi

#
echo "Merge pangu system into system"
src="$port/product/pangu/system"
dst="$port/system/system"
if [ -d "$src" ]; then
    cp -rf "$src"/. "$dst"/
    echo ">> product/pangu/system copied into system/system"
else
    echo ">> Skip: $src not found"
fi

#
echo "Transferring VNDK APEX from stok to port/system_ext/apex/"
source_apex="$stok/system_ext/apex"
dest_apex="$port/system_ext/apex"

if [ -d "$source_apex" ]; then
    for apex_file in "$source_apex"/com.android.vndk.v*.apex; do
        if [ -f "$apex_file" ]; then
            apex_basename=$(basename "$apex_file")
            
            if [ ! -f "$dest_apex/$apex_basename" ]; then
                cp "$apex_file" "$dest_apex/"
                echo ">> Copied: $apex_basename from stok to port"
            else
                echo ">> Skipping: $apex_basename already exists in port"
            fi
        fi
    done
else
    echo ">> Error: Source folder $source_apex not found in stok"
fi

echo "=== HyperOS Auto-Port Done ==="