#View projects
CK () {
for i in $(ls -d $DNA_DIR/DNA_*);do
  echo "${i##*/}"
done
}
#Create project
XJ () {
if [ -d $DNA_DIR/DNA_"$T" ]; then
  echo "> Project already exists, it will be automatically renamed!"
  T="$T"-`date "+%Y%m%d%H%M%S"`
  echo "> Creating: DNA_${T}"
  mkdir -p $DNA_DIR/DNA_"$T"
else
  echo "> Creating: DNA_${T}"
  mkdir -p $DNA_DIR/DNA_"$T"
fi
echo "> Created successfully!"
echo "DNA_${T}" > $TMPDIR/DNA.ini
}
#Delete project
SC () {
for i in ${TSS};do
  echo "> Deleting: ${i}"
  rm -rf $DNA_DIR/$i
  rm -rf $DNA_TMP/$i
  if [ "$i" = "$project" ];then
    rm -rf $TMPDIR/DNA.ini
  fi
  echo "> Deleted!"
done
}
#Delete plugin
sub (){
for i in ${sub};do
  echo "> Deleting: $i"
  rm -rf $START_DIR/module/$i
  echo "> Deleted!"
done
}
#Clear project
RM () {
if [ "$O" = "Yes" ] || [ "$O" = "YES" ] || [ "$O" = "yes" ]; then
    if [ -z "$DNA_DRO" ]; then
        echo "> Error: project is empty!"
        return 1
    fi
    if [ -d "$DNA_DRO" ]; then
      echo "> Clearing $dna_project!"
      rm -rf "$DNA_DRO"
      echo "> $dna_project cleared"
    else
      echo "> $DNA_DRO not found"
    fi
else
    echo "> canceling the clear"
fi
}

$1