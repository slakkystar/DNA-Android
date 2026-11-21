#!/system/bin/sh

echo "
  _                            
 |_) _. _|_  _ |_  o ._   _    
 |  (_|  |_ (_ | | | | | (_|   
 \  / |_  ._ _   _ _|_  _._|   
  \/  |_) | | | (/_ |_ (_|     
                               
  "

BIN=$START_DIR/tools
project=$(cat "$TMPDIR/DNA.ini")
DNA_PRO="$DNA_DIR/$project"

if [ ! -d "$DNA_PRO/out" ]; then
  mkdir -p "$DNA_PRO/out"
fi

for i in $IMG; do
  info=$(dna gettype "$DNA_PRO/$i")

  if [ "$info" = "vbmeta" ]; then
    echo "> Patching: $i"
    cp -rf "$DNA_PRO/$i" "$DNA_PRO/out/$i"

    case "$PATCH_TYPE" in
      verity)
        echo ">> Disabling dm-verity..."
        $BIN/magiskboot hexpatch "$DNA_PRO/out/$i" \
          0000000000000000617662746F6F6C20 \
          0000000200000000617662746F6F6C20
        ;;
      verification)
        echo ">> Disabling dm-verification..."
        $BIN/magiskboot hexpatch "$DNA_PRO/out/$i" \
          00000000000000007665726966792000 \
          00000000000000007665726966790000
        ;;
      both)
        echo ">> Disabling both dm-verity & dm-verification..."
        $BIN/magiskboot hexpatch "$DNA_PRO/out/$i" \
          0000000000000000617662746F6F6C20 \
          0000000200000000617662746F6F6C20
        $BIN/magiskboot hexpatch "$DNA_PRO/out/$i" \
          00000000000000007665726966792000 \
          00000000000000007665726966790000
        ;;
      *)
        echo "> Invalid option: $PATCH_TYPE"
        exit 1
        ;;
    esac

    echo "> Patched file saved at: $DNA_PRO/out/$i"
  else
    echo "> Skipping $i (not vbmeta type)"
  fi
done
