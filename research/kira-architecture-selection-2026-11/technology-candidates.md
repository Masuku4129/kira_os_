# Кандидаты по направлениям (A–M из CG-20261109-013)
Статус: [E] измерено в этом исследовании, [S] статически по коду KIRA, [X] внешние источники/ранее полученные чужие результаты, [H] гипотеза. Колонка «в исследовании» означает, что кандидата реально проверяли.

| Напр. | Кандидаты | Есть в KIRA | Проверяется здесь |
|---|---|---|---|
| A ядро | монолит, микроядро, гибрид, модульное/экзоядро | гибрид: ядро + userland-службы (fsrv, kdrv Ring3, kccrt) | [S] границы привилегий, syscall; [E] загрузка/тесты в QEMU |
| B языки | C, Rust, C++, Go(справка), Zig, Ada/SPARK, ассемблер, свой язык | C + немного ассемблера | [E] C/Rust/C++/ASM/Go(част.); Zig, Ada, свой язык — NOT RUN |
| C компиляторы | KCC, clang, gcc, (LLVM IR/MLIR — только [X]) | KCC (свой IR линейный) | [E] скорость/размер/время/память, вклад проходов |
| D ассемблер/линкер | KAS, nasm, GNU as, llvm-mc; KLINK, ld.lld, ld.bfd | KAS, KLINK | [E] равенство байтов, скорость |
| D форматы | ELF ET_REL/EXEC/DYN, PE/COFF (UEFI), плоский образ, свой KEX, WASM/WASI, eBPF-подобный | ELF + PE (UEFI) + .kdrv | [E] размеры/разбор/фаззинг; WASM [E]; eBPF-подобный NOT RUN |
| E ABI | SysV AMD64, AAPCS64, UEFI, собственный syscall | свой syscall + SysV внутри | [S]+[E] интероп kcc↔clang/gcc/Rust |
| F память/безопасность | W^X, NX, KASLR, IOMMU, SMMU, CFI | W^X, VT-d (virtio-blk), анти-откат | [E] в QEMU; железо NOT RUN |
| G IPC | pipe, SHM ring, futex, eventfd | futex.c, ipc | [X] чужие замеры; [E] свой тест в KIRA (QEMU) |
| H файловая система | журнал, CoW, CAS/Merkle, лог | KiraFS (kfs) | [S]; [E] ограниченно |
| I данные | fixed, varint, columnar, CBOR, zstd/lz4/zlib | свои форматы | [E] на данных KIRA |
| J исполнение | native, WASM, eBPF-подобная ВМ, JIT | native ELF | [E] WASM |
| K драйверы | конвейер открытых драйверов | KDRV Ring3 + подпись | [S]+[E] приёмка, откат; поиск лицензий — [H]/[X] |
| L ИИ | локальная модель, брокер политик | Разум + mind_build | [E] стенд, безопасность посредника |
| M интеграция | сквозная цепочка | kernel kcc+kas+klink грузится | [E] x86-64; ARM64 частично |
