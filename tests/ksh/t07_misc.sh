# set -u, set -e, local, функции и прочее; разбор имён, лимиты и вложенность
(set -u; echo ${u1:-ok}; echo $u1; echo not-reached) 2>&1; echo rc=$?
(set -e; true; false; echo not-reached); echo rc=$?
(set -e; false || true; echo e-ok; ! false; echo e-ok2); echo rc=$?
x=1; f() { local x=2; g; echo f=$x; }; g() { echo g=$x; x=3; }; f; echo top=$x
f() { local a b=5; a=${a:-unset}; echo "$a $b"; }; f
set -- 1 2 3 4 5 6 7 8 9 10 11
echo $10 ${10} ${11} $#
FOO=bar echo "tmp: $FOO"; echo "after: [$FOO]"
f2() { echo in-$FOO; }; FOO=y f2; echo out-[$FOO]
export E1=one; E1=two echo tmp; echo $E1
a=1 b=2; echo $a $b
a=$(echo x)$(echo y); echo $a
z=$(false); echo "z=[$z] $?"
$(echo echo dynamic); `echo echo backtick`
cmd=echo; $cmd via-var; "$cmd" quoted-var
empty=; $empty echo after-empty
if [ -z "$(echo)" ]; then echo empty-subst; fi
echo "$(printf 'a\n\n\n')end"
# длинные имена и ограничения песочницы
n40=$(printf 'n%.0s' 1 2 3 4 5 6 7 8 9 10 1 2 3 4 5 6 7 8 9 10 1 2 3 4 5 6 7 8 9 10 1 2 3 4 5 6 7 8 9 10)
echo ${#n40}; echo x > $n40; ls $n40 | wc -c; rm $n40
echo x > 'a:b' 2>&1; echo x > .hidden; ls; rm .hidden
# вложенность: глубокий разбор, подстановки и рекурсия не обрушивают оболочку (текст ошибки зависит от запаса стека - проверяем только факт)
s=''; i=0; while [ $i -lt 400 ]; do s="$s("; i=$((i+1)); done
eval "$s" 2> err.txt; [ -s err.txt ] && echo deep-paren-reported
s=''; e=''; i=0; while [ $i -lt 300 ]; do s="$s\$("; e="$e)"; i=$((i+1)); done
eval "echo ${s}echo hi$e" > out.txt 2> err.txt; [ -s err.txt ] && echo deep-subst-reported
s=''; i=0; while [ $i -lt 300 ]; do s="${s}if true; then "; i=$((i+1)); done
eval "$s" 2> err.txt; [ -s err.txt ] && echo deep-if-reported
(deep() { deep; }; deep) 2> err.txt; echo "deep-rc=$?"; [ -s err.txt ] && echo recursion-reported
d2() { [ $1 -gt 0 ] && d2 $(($1-1)); return 0; }; d2 5; echo shallow=$?
echo done
