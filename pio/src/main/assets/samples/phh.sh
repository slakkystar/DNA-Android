src="$START_DIR/samples/phh_init/A$A"
project=$(cat "$TMPDIR/DNA.ini" 2>/dev/null)

echo "=== phh_init Injector ==="

#
if [ -z "$A" ]; then
    echo ">> Error: variable A is empty (expected Android version, e.g. 10-16)"
    exit 1
fi

if [ ! -d "$DNA_DRO/system/system" ]; then
    echo ">> Error: $project/system/system/ not found"
    exit 1
fi

if [ ! -d "$src" ]; then
    echo ">> Error: phh_init for A$A not found: $src"
    exit 1
fi

#
echo "Copying phh_init (A$A) into $project/system/system/ not found"
cp -rf "$src"/. "$DNA_DRO/system/system/"
echo ">> phh_init (A$A) copied"