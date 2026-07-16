if [ -d "$DNA_DRO/vendor_boot" ]; then
    HEADER_FILE="$DNA_DRO/vendor_boot/header"
elif [ -d "$DNA_DRO/boot" ]; then
    HEADER_FILE="$DNA_DRO/boot/header"
else
    echo "Error: Neither boot nor vendor_boot found in project $DNA_DRO."
    exit 1
fi

if [ ! -f "$HEADER_FILE" ]; then
    echo "Error: File $HEADER_FILE not found."
    exit 1
fi

if grep -q "androidboot.selinux=permissive" "$HEADER_FILE"; then
    echo "Flag is already set in $HEADER_FILE."
else
    sed -i '/^cmdline=/ s/$/ androidboot.selinux=permissive/' "$HEADER_FILE"
    
    if [ $? -eq 0 ]; then
        echo "Success: androidboot.selinux=permissive added to $HEADER_FILE"
    else
        echo "Error editing the file."
    fi
fi