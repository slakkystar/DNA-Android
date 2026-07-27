# some variables 
port="$DNA_TMP/DNA_port"
stok="$DNA_TMP/DNA_stok"
build_prop="$port/system/system/build.prop"
vgc_prop="$port/vgc/vgc.prop"
oem_prop="$port/oem/oem.prop"
oem_vendor_prop="$port/oem/vendor.prop"
dyn_prop="$port/dyn/dyn.prop"
phh_src="$START_DIR/samples/phh_init/A$android_version"

echo "=== OriginOS Auto-Port ==="

if [ ! -f "$build_prop" ]; then
    echo ">> Error: File $build_prop not found"
    exit 1
fi

#
echo "Adding persist.sys.disable_rescue"
if grep -q "^persist.sys.disable_rescue=" "$build_prop"; then
    sed -i 's/^persist.sys.disable_rescue=.*/persist.sys.disable_rescue=true/' "$build_prop"
    echo ">> Updated: persist.sys.disable_rescue=true"
else
    echo "persist.sys.disable_rescue=true" >> "$build_prop"
    echo ">> Added: persist.sys.disable_rescue=true"
fi

# vgc to system merge 
echo "Merging vgc.prop into build.prop"
if [ -f "$vgc_prop" ]; then
    echo "" >> "$build_prop"
    echo "# Copyed from vgc.img" >> "$build_prop"
    cat "$vgc_prop" >> "$build_prop"
    echo ">> vgc.prop merged into build.prop"
else
    echo ">> Skip: $vgc_prop not found"
fi

# oem to system merge
echo "Merging oem.prop and vendor.prop into build.prop"
if [ -f "$oem_prop" ] || [ -f "$oem_vendor_prop" ]; then
    echo "" >> "$build_prop"
    echo "# Copyed from oem.img" >> "$build_prop"
    if [ -f "$oem_prop" ]; then
        cat "$oem_prop" >> "$build_prop"
        echo ">> oem.prop merged into build.prop"
    else
        echo ">> Skip: $oem_prop not found"
    fi
    if [ -f "$oem_vendor_prop" ]; then
        cat "$oem_vendor_prop" >> "$build_prop"
        echo ">> vendor.prop merged into build.prop"
    else
        echo ">> Skip: $oem_vendor_prop not found"
    fi
else
    echo ">> Skip: no oem.prop or vendor.prop found in $port/oem"
fi

# dyn to system merge
echo "Merging dyn.prop into build.prop"
if [ -f "$dyn_prop" ]; then
    echo "" >> "$build_prop"
    echo "# Copyed from dyn.img" >> "$build_prop"
    cat "$dyn_prop" >> "$build_prop"
    echo ">> dyn.prop merged into build.prop"
else
    echo ">> Skip: $dyn_prop not found"
fi

# Android version detect for phh init
echo "Detecting Android version"
android_version=$(grep -m1 "^ro.system.build.version.release=" "$build_prop" | cut -d'=' -f2 | tr -d '[:space:]')
if [ -z "$android_version" ]; then
    echo ">> Error: ro.system.build.version.release not found in $build_prop"
    exit 1
fi
echo ">> Detected Android version: $android_version"

# phh init
echo "Adding phh_init"
if [ -d "$phh_src" ]; then
    cp -rf "$phh_src"/. "$port/system/system/"
    echo ">> phh_init (A$android_version) copied into $port/system/system"
else
    echo ">> Error: $phh_src not found"
fi

# apex
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

echo "=== OriginOS Auto-Port Done ==="
