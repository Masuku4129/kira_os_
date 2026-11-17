# переменные, подстановки, арифметика
x=5; y="a b"
echo $x "$y" ${x}x ${z:-def} ${#y} $((x*3+1))
echo ${y#a } ${y%b} $(echo hi) `echo bt`
if [ $x -gt 3 ] && [ -z "$z" ]; then echo big; elif true; then echo mid; else echo small; fi
for i in 1 2 3; do echo i=$i; done
n=0; while [ $n -lt 3 ]; do n=$((n+1)); done; echo n=$n
case foo in f*) echo match;; *) echo no;; esac
f() { local a=$1; echo "f:$a:$#:$@"; return 3; }
f A B; echo rc=$?
echo hello | cat | wc -c
printf '%s-%d-%5.2f|%x\n' abc 42 3.14159 255
echo a > o.txt; echo b >> o.txt; cat o.txt; wc -l o.txt; ls; rm o.txt; ls
cat <<EOF2
x is $x
EOF2
set -- p q r; for a in "$@"; do echo [$a]; done; shift; echo $# $1
echo $(( (1+2)*3 )) $((7%3)) $((1<<4)) $((x>2?10:20))
echo done; exit 4
