# кавычки, экранирование, разбиение на слова, подстановки
a='x  y'; b="p q"
echo '$a' "$a" $a '\n' "\$a" \$a "a\"b" 'it'\''s'
echo "$b"-"$b" $b-$b
set -- one "two words" three
echo "$#:$1:$2:$3"
for w in "$@"; do echo "<$w>"; done
for w in "$*"; do echo "<$w>"; done
for w in $*; do echo "<$w>"; done
IFS=:; set -- a:b::c; echo $#; for w in $1; do echo "[$w]"; done; IFS=' '
unset q
echo "${q-unset}" "${q:-empty}" "${q+set}" "${q:+set}"
q=; echo "${q-unset}" "${q:-empty}" "${q+set}" "${q:+set}"
q=val; echo "${q-unset}" "${q:-empty}" "${q+set}" "${q:+set}"
echo ${nope:=assigned} $nope
path=/usr/local/lib/file.tar.gz
echo ${path#*/} ${path##*/} ${path%.*} ${path%%.*}
echo ${#path} ${#nope}
s=привет
echo ${#s} $s
echo $(echo $(echo nested)) "$(echo "a b")"
echo `echo \`echo inner\``
echo $((1+2*3)) $(( (1+2)*3 )) $((10/3)) $((-7%3)) $((2<<3|1)) $((5>3&&2>1)) $((0||0)) $((!0)) $((~0))
v=7; echo $((v+=3)) $v $((v<<=1)) $v $((v==20?100:200))
echo $((0x1f)) $((010)) $((1<2)) $((3-5))
x=1; echo $((x+y)) $((undefined_var))
touch_() { : > "$1"; }
touch_ a1.txt; touch_ a2.txt; touch_ b1.log
echo *.txt a?.txt [ab]1.* "*.txt" '*.log' nomatch*
for f in *.txt; do echo f=$f; done
echo ~ "~" a\ b
e=''; echo "[$e]" [$e] "[${e}x]"
echo "multi
line"
echo a"b"'c'd
echo \
joined
echo "a\\b" 'a\\b' "a\$b" "a\`b" "a\zb"
echo $(echo)end
echo "$@"x; set --; echo "$@" end; echo "[$@]"; echo "$#"
