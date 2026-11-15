# граничные случаи
break; echo rc=$?
continue 2>&1; echo rc=$?
return 2>&1; echo rc=$?
local zz 2>&1; echo rc=$?
f() { exit 9; echo no; }; (f); echo exit-in-func-sub=$?
g() { for i in 1 2 3; do if [ $i = 2 ]; then return 4; fi; done; }; g; echo g=$?
h() { while true; do break; done; echo after-break; }; h
loop_in_fn() { for i in 1 2; do ctl_fn() { break; }; ctl_fn 2>&1; done; echo fn-loop-done; }; loop_in_fn
set -- a b c
IFS=,; echo "$*" "$@" $*; IFS=' '
IFS=; echo "$*"; unset IFS; echo "$*"
set --; echo "[$*][$@]" $# "${#}"
echo "${1:-none}" "${1-none}"
unset -v nothing; echo "[${nothing}]"
printf 'x' | { read r; echo "r=$r rc=$?"; }
printf 'x\n' | { read r; echo "r=$r rc=$?"; }
printf '' | { read r; echo "r=[$r] rc=$?"; }
printf 'a:b:c\n' | { IFS=: read p q; echo "$p|$q"; }
printf '  a   b  \n' | { read p q; echo "[$p][$q]"; }
printf 'only\n' | { read p q; echo "[$p][$q]"; }
echo "x y z" | { read -r a b; echo "$b"; }
! echo x | cat; echo neg-pipe=$?
! echo y | cat | false; echo neg2=$?
! true | true; echo neg3=$?
true | false; echo pipe-last=$?
false | true; echo pipe-last2=$?
printf 'abc\n' | wc -c; printf 'a b\nc\n' | wc; echo -n | wc -l
{ echo 1; echo 2; } | while read n; do echo "n$n"; done
(echo in; echo sub) > sub.txt; cat sub.txt
case a in a|b|c) echo alt;; esac
case "$(echo b)" in a|b) echo cmd-case;; esac
case a in (a) echo paren-pat;; esac
case x in
  x)
    echo multi-line
    ;;
  *)
    echo no
    ;;
esac
case ab in a?) echo q;; esac; case 'a?' in 'a?') echo quoted-q;; esac; case ab in 'a?') echo bad;; *) echo quoted-no;; esac
v=abc; case $v in *b*) echo contains;; esac
x=a; case $x in "$x") echo var-pat;; esac
pat='a*'; case abc in $pat) echo unq-var-pat;; esac; case abc in "$pat") echo bad;; *) echo q-var-pat;; esac
f() { echo "args=$#"; }; f; f ""; f "" ""; f $empty; f "$empty"; f "a b" c
f2() { echo "$1|$2"; shift; echo "$1|$2"; }; f2 x y z
nested() { inner() { echo inner-$1; }; inner "$1"; }; nested n; inner still-defined
if true
then
  echo then-on-next-line
fi
while false
do
  :
done
for i in a b
do
  echo $i
done
echo a; echo b &&
  echo c
echo $((  1 + 2  ))$(( 3 ))
echo "$((1+1))$(echo x)${HOME:-h}"
a=1; b=$a; a=2; echo $b
n=0; until [ $n -ge 2 ]; do n=$((n+1)); done; echo $n
echo a\
b
echo 'a
b'
echo ab#cd #comment
echo "#notcomment" '#also'
#
echo end
