#!/system/bin/sh

# Environment variables
export PATH="$({TOOLKIT}):$PATH"
export START_DIR="$({START_DIR})"
export TEMP_DIR="$({TEMP_DIR})"
export TMPDIR="$TEMP_DIR"
export APP_USER_ID="$({APP_USER_ID})"
# Determine if a working directory is specified, and change to the initial directory
if [[ "$START_DIR" != "" ]] && [[ -d "$START_DIR" ]]
then
    cd "$START_DIR"
fi

# Tools directory
export DNA_DIR=$({SDCARD_PATH})/DNA
if [ ! -d $DNA_DIR ];then
   mkdir -p $DNA_DIR
fi
if [ "$({ROOT_PERMISSION})" = "true" ];then
  export DNA_TMP=/data/DNA
else
  export DNA_TMP=$({START_DIR})/DNA
fi
if [ -f $TMPDIR/DNA.ini ]; then
  export dna_project=$(cat $TMPDIR/DNA.ini)
  export DNA_PRO=$DNA_DIR/$dna_project
  export DNA_DRO=$DNA_TMP/$dna_project
fi
chown -R $APP_USER_ID:$APP_USER_ID $START_DIR
# Run script
if [[ -f "$1" ]]; then
    chmod 755 "$1"
    source "$1"
else
    echo "${1} lost" 1>&2
fi
exit 0