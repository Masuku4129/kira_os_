# встроенные команды
echo -n a; echo -n b; echo; echo -e 'x\ty\\z\x41\0102\c'; echo; echo -E 'no\tesc'; echo -- -n; echo
printf '%s|%5s|%-5s|%.2s\n' abc abc abc abc
printf '%d %i %u %o %x %X %c %%\n' 42 -7 3 8 255 255 hello
printf '%05d|%+d|% d|%5.1f|%e|%g\n' 42 5 5 3.14159 1234.5 0.0001
printf '%b|%s\n' 'a\nb' 'a\nb'
printf '[%s]\n' one two three
printf '%s=%d\n' a 1 b 2
printf 'no args\n'; printf '%d\n'; printf '%s|\n'
printf '\x41\102\n'; printf '%d\n' "'A" 0x10 010
printf '%*d|%-*d|\n' 4 7 4 7
printf 'абв %s\n' ёж
touch_() { : > "$1"; }
touch_ e.txt; echo content > f.txt
[ -e f.txt ] && echo e1; [ -f f.txt ] && echo f1; [ -d . ] && echo d1; [ -d f.txt ] || echo d0
[ -s f.txt ] && echo s1; [ -s e.txt ] || echo s0; [ -e nope ] || echo e0
[ -z "" ] && echo z; [ -n "x" ] && echo n; [ -n "" ] || echo n0
[ a = a ] && echo eq; [ a != b ] && echo ne; [ a = b ] || echo neq
[ 3 -eq 3 ] && echo i1; [ 3 -ne 4 ] && echo i2; [ 3 -lt 4 ] && echo i3; [ 4 -le 4 ] && echo i4; [ 5 -gt 4 ] && echo i5; [ 5 -ge 5 ] && echo i6
[ -n x -a -z "" ] && echo and; [ -z x -o -n x ] && echo or; [ ! -e nope ] && echo not; [ ! -z x -a ! -z y ] && echo notand
[ \( 1 -lt 2 \) -a \( 2 -lt 3 \) ] && echo paren
test 1 -lt 2 && echo test-works; test -n && echo lone-n; test && echo empty || echo empty-false
[ "x" ] && echo str; [ "" ] || echo nostr; [ ! ] && echo bang-str
[ 1 -lt abc ] 2>&1; echo rc=$?
[ 1 = 1 2>&1; echo rc=$?
echo $(expr 2 + 3) $(expr 7 - 10) $(expr 6 '*' 7) $(expr 17 / 5) $(expr 17 % 5)
expr 3 '<' 5; expr 5 '<' 3; expr abc = abc; expr 1 '|' 0; expr 0 '|' 7; expr 0 '&' 7; expr 4 '&' 5
expr length привет; expr substr abcdef 2 3; expr '(' 1 + 2 ')' '*' 3
expr 5 / 0; echo rc=$?; expr; echo rc=$?; expr 0; echo rc=$?
printf 'one\ntwo\nthree\nfour\n' > lines.txt
cat lines.txt; cat -n lines.txt; head -n 2 lines.txt; head -1 lines.txt; head lines.txt | wc -l
wc lines.txt; wc -l lines.txt; wc -w lines.txt; wc -c lines.txt; wc -l -w lines.txt f.txt; echo "a b c" | wc -w
cat lines.txt f.txt | wc -l; cat < f.txt; echo x | cat - f.txt
cp f.txt g.txt; cat g.txt; mv g.txt h.txt; ls; cp h.txt h.txt 2>&1; rm h.txt e.txt; ls -l; ls f.txt nope 2>&1
rm -f nope; echo rm-f=$?; rm nope 2>&1; echo rm=$?
pwd; cd; cd /; cd . ; cd sub 2>&1; echo cd=$?
x=1; export x; export y=2 z; export | head -3
set -- a b c; shift; echo $@; shift 2; echo "[$@]"; shift 2>&1; echo shift=$?
eval 'v=5; echo eval $v'; eval echo a b c; eval "echo \$v"
unset v x; echo "[$v$x]"
echo 'echo sourced $1 $#; sv=from-source; return 7; echo no' > s.sh
. s.sh p q; echo rc=$? sv=$sv; source s.sh; echo $?
read a b c <<EOF
one two three four five
EOF
echo "$a|$b|$c"
echo "  lead  trail  " | read line; echo "[$line]"
read -r x <<EOF
back\slash
EOF
echo "$x"
read x <<EOF
back\slash
EOF
echo "$x"
printf 'l1\nl2\nl3' > in.txt
while read l; do echo "got $l"; done < in.txt; echo "last=$l"
while IFS=: read u rest; do echo "$u -> $rest"; done <<EOF
root:x:0
bob:y:1:2
EOF
read -p "prompt> " pv <<EOF
val
EOF
echo $pv
read </dev/null 2>&1
: ; true; false; echo $?; :
set -x; echo traced; set +x; echo untraced
