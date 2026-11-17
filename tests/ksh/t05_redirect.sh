# перенаправления, документы-вставки, конвейеры, группы, подоболочки
echo one > r.txt; echo two >> r.txt; echo three >> r.txt
cat < r.txt; wc -l < r.txt
echo over > r.txt; cat r.txt
: > empty.txt; wc -c empty.txt
> made.txt; ls made.txt
noprog_cmd 2> err.txt; cat err.txt
noprog_cmd 2>&1 | wc -l
echo to-err >&2
{ echo a; echo b; } > grp.txt; cat grp.txt
{ echo out1; echo err1 >&2; } > both.txt 2>&1; cat both.txt
{ echo out2; echo err2 >&2; } 2> only-err.txt; cat only-err.txt
( echo sub; echo sub2 ) | wc -l
f() { echo "in f: $1"; }
f hello > fout.txt; cat fout.txt
f() { cat; }; echo piped | f
i=0; while [ $i -lt 3 ]; do i=$((i+1)); echo line$i; done > loop.txt; cat loop.txt
while read a b; do echo "[$a][$b]"; done < loop.txt
for f in loop.txt r.txt; do echo "$f: $(wc -l < $f)"; done
n=0; cat loop.txt | while read l; do n=$((n+1)); done; echo "n=$n"
echo data | { read v; echo "got $v"; }
printf 'b\na\n' | cat | cat
cat <<EOF
plain $((1+1)) $(echo cmd) \$esc \\ back
EOF
cat <<'EOF'
literal $x $(cmd) \n
EOF
cat <<"EOF"
also literal $x
EOF
cat <<-EOF
	tab stripped
	  keeps spaces
	EOF
cat <<EOF | wc -l
l1
l2
EOF
x=val
cat <<EOF > hd.txt
x=$x
EOF
cat hd.txt
if true; then cat <<EOF
inside if $x
EOF
fi
cat <<A; cat <<B
first
A
second
B
echo $(cat <<EOF
subst
EOF
)
ls > list.txt; cat list.txt | wc -l
echo abc > a.txt; echo 1 > b.txt; cat a.txt b.txt > c.txt; cat c.txt
echo "x" > 'name with space' 2>&1
echo "x" > dir/file 2>&1
echo "x" > $(printf 'a%.0s' 1 2 3 4 5 6 7 8 9 10 1 2 3 4 5 6 7 8 9 10 1 2 3 4 5 6 7 8 9 10 1 2 3 4 5 6 7 8 9 10 1 2) 2>&1
cat < nofile 2>&1; echo rc=$?
echo ok 1>&2 2>&1
echo a > r1.txt > r2.txt; ls r1.txt r2.txt; cat r2.txt
cat nofile r.txt 2>&1
