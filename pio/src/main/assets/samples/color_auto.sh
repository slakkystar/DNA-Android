stok=$DNA_TMP/DNA_stok
port=$DNA_TMP/DNA_port
product_prop="$port/product/etc/build.prop"
manifest_prop="$port/system/my_manifest/build.prop"
my_product_prop="$port/system/my_product/build.prop"

#
echo "Removing first api from my_manifest"
if [ -f "$manifest_prop" ]; then
    sed -i '/ro.product.first_api_level=/d' "$manifest_prop"
else
    echo ">> Error: File $manifest_prop not found"
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

#
echo "Adding parameters to product build.prop"
if [ -f "$product_prop" ]; then
    lines_to_add=(
        "ro.apex.updatable=true"
        "ro.cp_system_other_odex=0"
        "ro.nnapi.extensions.deny_on_product=true"
        "ro.lmk.kill_heaviest_task=true"
        "ro.lmk.kill_timeout_ms=100"
        "ro.lmk.use_minfree_levels=true"
        "ro.surface_flinger.vsync_event_phase_offset_ns=-1"
        "ro.surface_flinger.vsync_sf_event_phase_offset_ns=-1"
        "qemu.hw.mainkeys=0"
        "ro.opa.eligible_device=true"
        "ro.setupwizard.mode=OPTIONAL"
        "ro.vndk.lite=false"
        "persist.sys.disable_rescue=true"
        "ro.control_privapp_permissions=disable"
    )

    for line in "${lines_to_add[@]}"; do
        param_name=$(echo "$line" | cut -d'=' -f1)
        if grep -q "^$param_name=" "$product_prop"; then
            echo ">> Skipping: $param_name already exists"
        else
            echo "$line" >> "$product_prop"
            echo ">> Added: $line"
        fi
    done
else
    echo ">> Error: File $product_prop not found"
fi

#
echo "Commenting out parameters in my_product build.prop"

if [ -f "$my_product_prop" ]; then
    params_to_comment=(
        "ro.density.screenzoom.fdh"
        "ro.density.screenzoom.qdh"
        "persist.debug.wfd.enable"
        "ro.oplus.display.screen.heteromorphism"
        "ro.display.underscreen.lightsensor.support"
        "ro.display.underscreen.lightsensor.screenshot.period"
        "ro.lcd.display.screen.underlightsensor.region"
        "ro.oplus.lcd.display.screen.underlightsensor.region"
        "ro.oplus.density.fhd_default"
        "ro.oplus.density.qhd_default"
        "persist.oplus.display.defaultresolution_density"
    )

    for param in "${params_to_comment[@]}"; do
        if grep -q "^$param=" "$my_product_prop"; then
            sed -i "s/^$param=/#$param=/g" "$my_product_prop"
            echo ">> Commented out: $param"
        else
            echo ">> Skipping: $param not found or already commented out"
        fi
    done
else
    echo ">> Error: File $my_product_prop not found"
fi

#
echo "Updating Oplusrom version"
manifest_files=(
    "$manifest_prop"
    "$product_prop_file"
)

for prop_file in "${manifest_files[@]}"; do
    if [ -f "$prop_file" ]; then
        if grep -q "ro.build.version.oplusrom.display=" "$prop_file"; then
            if ! grep -q "| AUTOSCRIPT" "$prop_file"; then
                sed -i 's/^ro.build.version.oplusrom.display=.*/& | AUTOSCRIPT/' "$prop_file"
                echo ">> Updated: $prop_file"
            else
                echo ">> Skipping: Label already present in $prop_file"
            fi
        else
            echo ">> Skipping: Line not found in $(basename $prop_file)"
        fi
    else
        echo ">> Error: File $prop_file not found"
    fi
done

#
echo "Adding Bluetooth Fix to my_product"
if [ -f "$my_product_prop" ]; then
    if grep -q "# Bluetooth Fix" "$my_product_prop"; then
        echo ">> Skipping: Bluetooth Fix already present in file"
    else
        cat <<EOF >> "$my_product_prop"

# Bluetooth Fix
bluetooth.profile.asha.central.enabled=true
bluetooth.profile.a2dp.source.enabled=true
bluetooth.profile.avrcp.target.enabled=true
bluetooth.profile.bap.broadcast.assist.enabled=false
bluetooth.profile.bap.unicast.client.enabled=false
bluetooth.profile.bap.broadcast.source.enabled=false
bluetooth.profile.bas.client.enabled=true
bluetooth.profile.ccp.server.enabled=false
bluetooth.profile.csip.set_coordinator.enabled=false
bluetooth.profile.gatt.enabled=true
bluetooth.profile.hap.client.enabled=false
bluetooth.profile.hfp.ag.enabled=true
bluetooth.profile.hid.host.enabled=true
bluetooth.profile.mcp.server.enabled=false
bluetooth.profile.opp.enabled=true
bluetooth.profile.pan.nap.enabled=true
bluetooth.profile.pan.panu.enabled=true
bluetooth.profile.vcp.controller.enabled=false
bluetooth.profile.a2dp.source.enabled=true
bluetooth.profile.avrcp.target.enabled=true
bluetooth.profile.avrcp.controller.enabled=false
bluetooth.profile.hfp.ag.enabled=true
bluetooth.profile.asha.central.enabled=true
bluetooth.profile.gatt.enabled=true
bluetooth.profile.hid.host.enabled=true
bluetooth.profile.hid.device.enabled=true
bluetooth.profile.map.server.enabled=true
bluetooth.profile.opp.enabled=true
bluetooth.profile.pan.nap.enabled=true
bluetooth.profile.pan.panu.enabled=true
bluetooth.profile.pbap.server.enabled=true
bluetooth.profile.sap.server.enabled=false
EOF
        echo ">> Bluetooth Fix successfully added"
    fi
else
    echo ">> Error: File $my_product_prop not found"
fi

#
echo "Transferring user configuration"
vendor_port_etc="$port/vendor/etc"
vendor_stok_etc="$stok/vendor/etc"

if [ -d "$vendor_stok_etc" ]; then
    user_files=("group" "passwd")

    for file in "${user_files[@]}"; do
        if [ -f "$vendor_port_etc/$file" ]; then
            cp -f "$vendor_port_etc/$file" "$vendor_stok_etc/$file"
            echo ">> File $file successfully transferred to $vendor_stok_etc"
        else
            echo ">> Error: File $file not found in $vendor_port_etc"
        fi
    done
else
    echo ">> Error: Target folder $vendor_stok_etc not found"
fi

#
echo "Synchronizing ODM parameters"
if [ -d "$stok/odm/etc" ]; then
    target_odm_prop="$stok/odm/etc/build.prop"
    echo ">> Path found: $stok/odm"
elif [ -d "$stok/vendor/odm/etc" ]; then
    target_odm_prop="$stok/vendor/odm/etc/build.prop"
    echo ">> Path found: $stok/vendor/odm"
else
    echo ">> Error: ODM partition not found in $stok"
fi

if [ -n "$target_odm_prop" ]; then
    source_odm_prop="$port/odm/build.prop"

    if [ -f "$source_odm_prop" ]; then
        odm_params=(
            "ro.oplus.image.odm.version"
            "ro.vendor.product.oem"
            "ro.vendor.product.device.oem"
            "ro.product.brand"
            "ro.product.manufacturer"
            "ro.build.display.id"
            "ro.build.display.full_id"
            "ro.build.display.full_ims"
            "ro.build.product"
        )

        for param in "${odm_params[@]}"; do
            value=$(grep "^$param=" "$source_odm_prop" | cut -d'=' -f2-)

            if [ -n "$value" ]; then
                if grep -q "^$param=" "$target_odm_prop"; then
                    sed -i "s|^$param=.*|$param=$value|" "$target_odm_prop"
                    echo ">> Updated: $param=$value"
                else
                    echo "$param=$value" >> "$target_odm_prop"
                    echo ">> Added: $param=$value"
                fi
            else
                echo ">> Skipping: Parameter $param not found in port"
            fi
        done
    else
        echo ">> Error: File $source_odm_prop (source) not found"
    fi
fi