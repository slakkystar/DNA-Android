if [ ! -d $DNA_PRO/out ];then
 mkdir -p $DNA_PRO/out
fi
for i in $IMG ;do
info=$(dna gettype $DNA_PRO/$i)
if [ "$info" = "ext" ] || [ "$info" = "erofs" ]; then
  echo "> Начало преобразования: $i"
    img2simg $DNA_PRO/$i $DNA_PRO/out/$i
elif [ "$info" = "sparse" ]; then
  echo "> Начало преобразования: $i"
  simg2img $DNA_PRO/$i $DNA_PRO/out/$i
else
  echo "> Преобразование данного формата не поддерживается: $i"
fi
if [[ $silence = 1 ]]; then
  echo "> Удаление: $i"
  rm -rf $DNA_PRO/$i
fi
if [ -f $DNA_PRO/out/$i ];then
  echo "> Преобразование завершено, файл сохранён в: $DNA_PRO/out"
else
  echo "> Преобразование не удалось, сделайте скриншот и свяжитесь с разработчиком для исправления!!!"
fi
done