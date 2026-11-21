#!/system/bin/sh

project=$(cat "$TMPDIR/DNA.ini")
DNA_PRO="$DNA_DIR/$project"
OUT_DIR="$DNA_PRO/out"
BB="$START_DIR/tools/busybox"
[ -x "$BB" ] || BB=busybox
mkdir -p "$OUT_DIR" || exit 1

case "$silence" in
    1|true|TRUE|True|yes|YES|Yes|on|ON|On) silence=1 ;;
    *) silence=0 ;;
esac

_tmpdir=
_moved=0

cleanup_tmp() {
    if [ -n "$_tmpdir" ]; then "$BB" rm -rf "$_tmpdir"; fi
    _tmpdir=
}

remove_moved_parts() {
    _k=0
    while [ "$_k" -lt "$_moved" ]; do
        "$BB" rm -f "$OUT_DIR/$_prefix.$_k"
        _k=$((_k + 1))
    done
    _moved=0
}

prefix_list="$TMPDIR/dna_split_prefixes.$$"
cleanup_all() {
    cleanup_tmp
    "$BB" rm -f "$prefix_list"
}
trap cleanup_all EXIT

split_one() {
    _prefix=$1
    _tmpdir=
    _moved=0

    case "$_prefix" in
        */*) echo "Error: Invalid file name: $_prefix"; return 1 ;;
    esac

    _input="$DNA_PRO/$_prefix"

    if [ ! -f "$_input" ]; then
        echo "Error: File not found: $_input"
        return 1
    fi

    case "$parts" in
        ''|*[!0-9]*) echo "Error: Invalid part count ($parts). Must be between 4 and 25."; return 1 ;;
    esac
    _parts=$parts
    if [ "$_parts" -lt 4 ] || [ "$_parts" -gt 25 ]; then
        echo "Error: Invalid part count ($_parts). Must be between 4 and 25."
        return 1
    fi

    _size=$("$BB" stat -c %s "$_input") || {
        echo "Error: Cannot read size of $_input"
        return 1
    }
    case "$_size" in
        ''|*[!0-9]*) echo "Error: Cannot read size of $_input"; return 1 ;;
    esac

    _base=$((_size / _parts))
    _rem=$((_size % _parts))
    if [ "$_base" -eq 0 ]; then
        echo "Error: $_prefix is too small for $_parts parts"
        return 1
    fi

    echo "> Detected size: ${_size} bytes ($((_size / 1048576)) MiB)"
    echo "> Splitting $_prefix into up to $_parts parts..."

    _tmpdir=$("$BB" mktemp -d "$OUT_DIR/.dna-split.XXXXXX") || {
        echo "Error: Cannot create temporary directory"
        return 1
    }

    _offset=0
    _i=0
    while [ "$_i" -lt "$_parts" ]; do
        _this=$_base
        if [ "$_i" -lt "$_rem" ]; then _this=$((_this + 1)); fi
        if [ "$_this" -le 0 ]; then break; fi

        _dst="$_tmpdir/$_prefix.$_i"
        if ! "$BB" dd if="$_input" of="$_dst" bs=1M skip="$_offset" count="$_this" iflag=skip_bytes,count_bytes status=none; then
            echo "Error: Split failed at part $_i for $_prefix"
            cleanup_tmp
            return 1
        fi

        _got=$("$BB" stat -c %s "$_dst") || _got=
        if [ "$_got" != "$_this" ]; then
            echo "Error: Short read at part $_i for $_prefix"
            cleanup_tmp
            return 1
        fi

        _offset=$((_offset + _this))
        _i=$((_i + 1))
    done

    if [ "$_offset" -ne "$_size" ] || [ "$_i" -eq 0 ]; then
        echo "Error: Split failed for $_prefix"
        cleanup_tmp
        return 1
    fi

    for _old in "$OUT_DIR/$_prefix".[0-9]*; do
        if [ -f "$_old" ]; then "$BB" rm -f "$_old"; fi
    done
    "$BB" rm -f "$OUT_DIR/.$_prefix.splitmeta"

    _j=0
    while [ "$_j" -lt "$_i" ]; do
        if ! "$BB" mv -f "$_tmpdir/$_prefix.$_j" "$OUT_DIR/$_prefix.$_j"; then
            echo "Error: Cannot write part $_j for $_prefix"
            remove_moved_parts
            cleanup_tmp
            return 1
        fi
        echo "> Created: $_prefix.$_j ($("$BB" stat -c %s "$OUT_DIR/$_prefix.$_j") bytes)"
        _moved=$((_moved + 1))
        _j=$((_j + 1))
    done

    _meta_tmp=$("$BB" mktemp "$OUT_DIR/.dna-meta.XXXXXX") || {
        echo "Error: Cannot create split metadata for $_prefix"
        remove_moved_parts
        cleanup_tmp
        return 1
    }
    printf 'raw %s %s\n' "$_size" "$_i" > "$_meta_tmp"
    if ! "$BB" mv -f "$_meta_tmp" "$OUT_DIR/.$_prefix.splitmeta"; then
        "$BB" rm -f "$_meta_tmp"
        echo "Error: Cannot write split metadata for $_prefix"
        remove_moved_parts
        cleanup_tmp
        return 1
    fi

    cleanup_tmp
    echo "> Total parts created: $_i"

    if [ "$silence" = 1 ]; then
        "$BB" rm -f "$_input"
    fi

    echo
    return 0
}

printf '%s\n' "$IMG" | "$BB" tr -d '\r' > "$prefix_list" || exit 1

rc=0
while IFS= read -r prefix; do
    if [ -z "$prefix" ]; then continue; fi
    split_one "$prefix" || rc=1
done < "$prefix_list"

exit "$rc"
