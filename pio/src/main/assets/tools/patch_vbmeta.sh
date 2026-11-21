#!/system/bin/sh

echo "
  _
 |_) _. _|_  _ |_  o ._   _
 |  (_|  |_ (_ | | | | | (_|
 \  / |_  ._ _   _ _|_  _._|
  \/  |_) | | | (/_ |_ (_|

  "

BIN=$START_DIR/tools
BB="$BIN/busybox"
[ -x "$BB" ] || BB=busybox

project=$($BB cat "$TMPDIR/DNA.ini" 2>/dev/null)
project=${project%$'\r'}
if [ -z "$project" ]; then
  echo "> DNA.ini is missing or empty: $TMPDIR/DNA.ini" >&2
  exit 1
fi

DNA_PRO="$DNA_DIR/$project"
out_dir="$DNA_PRO/out"
$BB mkdir -p "$out_dir" || exit 1

case "${PATCH_TYPE:-}" in
  verity)
    bits=1
    patch_msg=">> Disabling dm-verity..."
    ;;
  verification)
    bits=2
    patch_msg=">> Disabling dm-verification..."
    ;;
  both)
    bits=3
    patch_msg=">> Disabling both dm-verity & dm-verification..."
    ;;
  *)
    echo "> Invalid option: $PATCH_TYPE"
    exit 1
    ;;
esac

case "${silence:-0}" in
  1|true|TRUE|yes|on) delete_source=1 ;;
  *) delete_source=0 ;;
esac

TMP_RAW=
drop_temp() {
  if [ -n "$TMP_RAW" ]; then
    $BB rm -f "$TMP_RAW"
    TMP_RAW=
  fi
}
trap drop_temp EXIT
trap 'drop_temp; exit 1' HUP INT TERM

read_ascii() {
  $BB dd if="$1" bs=1 skip="$2" count="$3" status=none 2>/dev/null
}

read_le32() {
  _v=0
  _m=1
  for _b in $($BB od -An -tu1 -j "$2" -N 4 "$1" 2>/dev/null | $BB tr -s ' ' '\n'); do
    _v=$((_v + _b * _m))
    _m=$((_m * 256))
  done
  echo "$_v"
}

read_be64() {
  _v=0
  for _b in $($BB od -An -tu1 -j "$2" -N 8 "$1" 2>/dev/null | $BB tr -s ' ' '\n'); do
    _v=$((_v * 256 + _b))
  done
  echo "$_v"
}

is_sparse() {
  [ "$($BB od -An -tx1 -N 4 "$1" 2>/dev/null | $BB tr -d ' \n')" = "3aff26ed" ]
}

find_avb_offset() {
  _f=$1
  _sz=$2
  if [ "$(read_ascii "$_f" 0 4)" = "AVB0" ]; then
    echo 0
    return 0
  fi
  if [ "$_sz" -ge 256 ]; then
    _footer=$((_sz - 64))
    if [ "$(read_ascii "$_f" "$_footer" 4)" = "AVBf" ]; then
      _off=$(read_be64 "$_f" $((_footer + 20)))
      if [ $((_off + 256)) -le "$_sz" ] &&
         [ "$(read_ascii "$_f" "$_off" 4)" = "AVB0" ]; then
        echo "$_off"
        return 0
      fi
    fi
  fi
  return 1
}

set_flag_bits() {
  _f=$1
  _off=$(( $2 + 123 ))
  _old=$($BB od -An -tu1 -j "$_off" -N 1 "$_f" 2>/dev/null | $BB tr -d ' \n')
  [ -n "$_old" ] || return 1
  _new=$((_old | $3))
  printf "\\$(printf '%03o' "$_new")" | \
    $BB dd of="$_f" bs=1 seek="$_off" conv=notrunc status=none 2>/dev/null || return 1
  _check=$($BB od -An -tu1 -j "$_off" -N 1 "$_f" 2>/dev/null | $BB tr -d ' \n')
  [ "$_check" = "$_new" ]
}

patch_one() {
  _name=$1
  _src="$DNA_PRO/$_name"
  _dst="$out_dir/$_name"
  _sparse=0
  _blk=4096

  if [ ! -f "$_src" ]; then
    echo "> Skipping $_name (file not found)"
    rc=1
    return
  fi

  _work=$_src
  if is_sparse "$_src"; then
    _blk=$(read_le32 "$_src" 12)
    [ "$_blk" -gt 0 ] || _blk=4096
    TMP_RAW="${TMPDIR:-$out_dir}/.patch_vbmeta.$$.raw"
    if ! "$BIN/simg2img" "$_src" "$TMP_RAW" 2>/dev/null; then
      echo "> Skipping $_name (cannot unpack sparse image)"
      drop_temp
      rc=1
      return
    fi
    _work=$TMP_RAW
    _sparse=1
  fi

  _size=$($BB stat -c %s "$_work" 2>/dev/null)
  if [ -z "$_size" ] || [ "$_size" -lt 256 ] ||
     ! _off=$(find_avb_offset "$_work" "$_size"); then
    echo "> Skipping $_name (not vbmeta type)"
    drop_temp
    return
  fi

  echo "> Patching: $_name"
  echo "$patch_msg"

  _dst_dir=$($BB dirname "$_dst")
  if ! $BB mkdir -p "$_dst_dir"; then
    echo "> Failed to create output directory: $_dst_dir" >&2
    drop_temp
    rc=1
    return
  fi

  if [ "$_sparse" -eq 1 ]; then
    if ! set_flag_bits "$_work" "$_off" "$bits" ||
       ! "$BIN/img2simg" "$_work" "$_dst" "$_blk" >/dev/null 2>&1; then
      echo "> Failed to patch: $_name" >&2
      $BB rm -f "$_dst"
      drop_temp
      rc=1
      return
    fi
  else
    if ! $BB cp -f "$_src" "$_dst" ||
       ! set_flag_bits "$_dst" "$_off" "$bits"; then
      echo "> Failed to patch: $_name" >&2
      $BB rm -f "$_dst"
      rc=1
      return
    fi
  fi

  drop_temp
  echo "> Patched file saved at: $_dst"

  if [ "$delete_source" -eq 1 ]; then
    $BB rm -f "$_src" || echo "> Failed to delete source: $_src" >&2
  fi
}

rc=0
if [ "$#" -gt 0 ]; then
  for i in "$@"; do
    patch_one "$i"
  done
else
  _old_ifs=$IFS
  IFS='
'
  set -f
  for i in $IMG; do
    i=${i%$'\r'}
    [ -n "$i" ] || continue
    patch_one "$i"
  done
  set +f
  IFS=$_old_ifs
fi

exit "$rc"
