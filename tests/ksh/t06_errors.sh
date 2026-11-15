# ошибки: синтаксис, неизвестные команды, имена файлов, арифметика, set -u, ${x:?}
nonexistent_cmd arg; echo rc=$?
eval 'if true; then echo x'; echo rc=$?
eval 'echo a ) b'; echo rc=$?
eval 'fi'; echo rc=$?
eval 'case x in a) echo;; esac esac'; echo rc=$?
eval 'echo "unterminated'; echo rc=$?
eval 'echo a &'; echo rc=$?
eval 'for 1x in a; do :; done'; echo rc=$?
eval 'while true; do'; echo rc=$?
eval 'echo >'; echo rc=$?
eval 'a=1 b=2; echo $a$b'; echo rc=$?
eval '() { :; }'; echo rc=$?
eval 'cat <<EOF
never closed'; echo rc=$?
echo $((1/0)) after; echo rc=$?
echo $((1+)) after; echo rc=$?
echo $((1 2)) after; echo rc=$?
echo $((09)) after; echo rc=$?
x=$((5%0)); echo "x=[$x] rc=$?"
echo ${novar:?must be set} not-printed; echo rc-not-reached
echo after-fatal
