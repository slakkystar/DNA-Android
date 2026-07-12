#Просмотр проектов
CK () {
for i in $(ls -d $DNA_DIR/DNA_*);do
  echo "${i##*/}"
done
}
#Создать проект
XJ () {
if [ -d $DNA_DIR/DNA_"$T" ]; then
  echo "> Проект уже существует, будет автоматически переименован!"
  T="$T"-`date "+%Y%m%d%H%M%S"`
  echo "> Создаётся: DNA_${T}"
  mkdir -p $DNA_DIR/DNA_"$T"
else
  echo "> Создаётся: DNA_${T}"
  mkdir -p $DNA_DIR/DNA_"$T"
fi
echo "> Создан успешно!"
echo "DNA_${T}" > $TMPDIR/DNA.ini
}
#Удалить проект
SC () {
for i in ${TSS};do
  echo "> Удаляется: ${i}"
  rm -rf $DNA_DIR/$i
  rm -rf $DNA_TMP/$i
  if [ "$i" = "$project" ];then
    rm -rf $TMPDIR/DNA.ini
  fi
  echo "> Удалён!"
done
}
#Удалить плагин
sub (){
for i in ${sub};do
  echo "> Удаляется: $i"
  rm -rf $START_DIR/module/$i
  echo "> Удалён!"
done
}

$1