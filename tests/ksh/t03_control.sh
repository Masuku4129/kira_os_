# управление: if/while/until/for/case, функции, break/continue, ! && ||
if true; then echo t1; fi
if false; then echo no; else echo e1; fi
if false; then echo no; elif false; then echo no; elif true; then echo elif; else echo no; fi
if ! false; then echo neg; fi
true && echo and1; false || echo or1; false && echo no; true || echo no
false && echo no || echo chain; true || echo no && echo chain2
! true; echo "!true=$?"
i=0; until [ $i -ge 3 ]; do echo u$i; i=$((i+1)); done
i=0; while true; do i=$((i+1)); [ $i -eq 2 ] && continue; [ $i -gt 4 ] && break; echo w$i; done
for a in 1 2; do for b in x y z; do [ $b = y ] && continue; echo $a$b; done; done
for a in 1 2 3; do for b in x y; do echo $a$b; [ $a = 2 ] && break 2; done; done
for a in 1 2 3; do for b in x y; do [ $b = x ] && continue 2; echo no; done; done; echo after
for x; do echo arg-$x; done
set -- p q; for x; do echo pos=$x; done
for x in; do echo never; done; echo empty-for=$?
case abc in a) echo 1;; ab*) echo 2;; *) echo 3;; esac
case X in x|X) echo upper-or-lower;; esac
case "a b" in "a b") echo quoted;; esac
case x in a) echo a;; esac; echo nomatch=$?
case $unset in "") echo empty-case;; esac
case abc in [a-c]bc) echo range;; esac
case 5 in [!0-4]) echo notrange;; esac
case foo.txt in *.txt) echo txt;; *.sh) echo sh;; esac
fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(($1-1))) )); fi; }
echo fact=$(fact 10)
fib() { [ $1 -lt 2 ] && { echo $1; return; }; a=$(fib $(($1-1))); b=$(fib $(($1-2))); echo $((a+b)); }
echo fib=$(fib 12)
g() { echo "g args: $# $1 $2"; return 7; }
g one; echo "rc=$?"; g one two three; echo rc=$?
h() { local v=inner; echo $v; }
v=outer; h; echo $v
k() { v=changed; }; k; echo $v
lp() { local n=$1; [ $n -gt 0 ] && { echo lp$n; lp $((n-1)); }; return 0; }
lp 3
r() { return 5; echo no; }; r; echo r=$?
f1() { echo f1-old; }; f1; f1() { echo f1-new; }; f1
unset -f f1; f1 2>&1 | head -1
w() { for i in 1 2 3; do [ $i = 2 ] && return $i; done; }; w; echo w=$?
x=1; (x=2; echo sub=$x); echo out=$x
{ x=3; }; echo group=$x
(exit 3); echo exit-sub=$?
echo $(exit 4)$?
set -e; false || echo e-ok; if false; then :; fi; echo still; false; echo not-reached
