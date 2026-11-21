#!/system/bin/sh

project=$(cat "$TMPDIR/DNA.ini")
DNA_PRO="$DNA_DIR/$project"
OUT_DIR="$DNA_PRO/out"
BB="$START_DIR/tools/busybox"
SIMG2IMG="$START_DIR/tools/simg2img"
[ -x "$BB" ] || BB=busybox
[ -x "$SIMG2IMG" ] || SIMG2IMG=simg2img
mkdir -p "$OUT_DIR" || exit 1

case "$silence" in
    1|true|TRUE|True|yes|YES|Yes|on|ON|On) silence=1 ;;
    *) silence=0 ;;
esac

_tmp_list=
_tmp_sorted=
_tmp_out=

cleanup_tmp() {
    if [ -n "$_tmp_list" ]; then "$BB" rm -f "$_tmp_list"; fi
    if [ -n "$_tmp_sorted" ]; then "$BB" rm -f "$_tmp_sorted"; fi
    if [ -n "$_tmp_out" ]; then "$BB" rm -f "$_tmp_out"; fi
    _tmp_list=
    _tmp_sorted=
    _tmp_out=
}

prefix_list="$TMPDIR/dna_merge_prefixes.$$"
cleanup_all() {
    cleanup_tmp
    "$BB" rm -f "$prefix_list"
    "$BB" rm -f "$prefix_list.sorted"
}
trap cleanup_all EXIT

has_parts() {
    for _p in "$1/$2".[0-9]*; do
        if [ -f "$_p" ]; then return 0; fi
    done
    return 1
}

merge_one() {
    _prefix=$1
    _tmp_list=
    _tmp_sorted=
    _tmp_out=

    case "$_prefix" in
        */*) echo "Error: Invalid file name: $_prefix"; return 1 ;;
    esac

    if has_parts "$OUT_DIR" "$_prefix"; then
        _parts_dir="$OUT_DIR"
    elif has_parts "$DNA_PRO" "$_prefix"; then
        _parts_dir="$DNA_PRO"
    else
        echo "Error: No parts found for $_prefix"
        return 1
    fi

    if [ "$_parts_dir" = "$OUT_DIR" ] && has_parts "$DNA_PRO" "$_prefix"; then
        echo "Warning: parts in the project root are ignored; using $_parts_dir"
    fi

    echo "Merging: $_prefix"

    _tmp_list=$("$BB" mktemp "$TMPDIR/dna-parts.XXXXXX") || {
        echo "Error: Cannot create temporary file"
        return 1
    }
    _tmp_sorted=$("$BB" mktemp "$TMPDIR/dna-parts.XXXXXX") || {
        echo "Error: Cannot create temporary file"
        cleanup_tmp
        return 1
    }

    : > "$_tmp_list"
    for _f in "$_parts_dir/$_prefix".[0-9]*; do
        if [ ! -f "$_f" ]; then continue; fi
        _base=${_f##*/}
        _suffix=${_base#"$_prefix".}
        case "$_suffix" in
            ''|*[!0-9]*) continue ;;
        esac
        printf '%s\n' "$_f" >> "$_tmp_list"
    done

    if [ ! -s "$_tmp_list" ]; then
        echo "Error: No parts found for $_prefix"
        cleanup_tmp
        return 1
    fi

    if ! "$BB" sort -V "$_tmp_list" > "$_tmp_sorted"; then
        echo "Error: Cannot sort parts for $_prefix"
        cleanup_tmp
        return 1
    fi

    set --
    _expected=
    while IFS= read -r _f; do
        _base=${_f##*/}
        _suffix=${_base#"$_prefix".}
        _n=$_suffix
        while [ "${_n#0}" != "$_n" ]; do _n=${_n#0}; done
        if [ -z "$_n" ]; then _n=0; fi
        if [ -z "$_expected" ]; then
            _expected=$_n
        fi
        if [ "$_n" -ne "$_expected" ]; then
            echo "Error: Missing or duplicate part in $_prefix (expected $_expected, found $_n)"
            cleanup_tmp
            return 1
        fi
        set -- "$@" "$_f"
        _expected=$((_n + 1))
    done < "$_tmp_sorted"

    _magic=
    _mode=
    _mode_known=0
    _meta="$_parts_dir/.$_prefix.splitmeta"
    if [ -f "$_meta" ]; then
        read -r _mode _rest < "$_meta" || :
        case "$_mode" in
            raw) _mode_known=1 ;;
            sparse) _mode_known=2 ;;
        esac
    fi
    if [ "$_mode_known" -eq 0 ]; then
        _magic=$("$BB" od -An -tx1 -N4 "$1" | "$BB" tr -d ' \n')
        case "$_magic" in
            3aff26ed) _mode_known=2 ;;
            *) _mode_known=1 ;;
        esac
    fi

    _tmp_out=$("$BB" mktemp "$OUT_DIR/.dna-merge.XXXXXX") || {
        echo "Error: Cannot create temporary output"
        cleanup_tmp
        return 1
    }

    if [ "$_mode_known" -eq 2 ]; then
        "$SIMG2IMG" "$@" "$_tmp_out"
    else
        "$BB" cat "$@" > "$_tmp_out"
    fi
    _rc=$?
    if [ "$_rc" -ne 0 ]; then
        echo "Error: Merge failed for $_prefix"
        cleanup_tmp
        return 1
    fi

    if [ ! -s "$_tmp_out" ]; then
        echo "Error: Merge produced an empty file for $_prefix"
        cleanup_tmp
        return 1
    fi

    if ! "$BB" mv -f "$_tmp_out" "$OUT_DIR/$_prefix"; then
        echo "Error: Cannot write $OUT_DIR/$_prefix"
        cleanup_tmp
        return 1
    fi
    _tmp_out=

    echo "Merge complete: $OUT_DIR/$_prefix"

    if [ "$silence" = 1 ]; then
        for _f in "$@"; do
            "$BB" rm -f "$_f"
        done
        "$BB" rm -f "$_meta"
    fi

    cleanup_tmp
    return 0
}

printf '%s\n' "$IMG" | "$BB" tr -d '\r' > "$prefix_list" || exit 1

rc=0
while IFS= read -r prefix; do
    if [ -z "$prefix" ]; then continue; fi
    merge_one "$prefix" || rc=1
done < "$prefix_list"

exit "$rc"
