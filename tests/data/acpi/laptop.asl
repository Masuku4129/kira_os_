/* Тестовая DSDT "ноутбук": настоящий ASL, собранный настоящим iasl (tests/data/acpi/build.sh).
   Не снимок чужой машины, а таблица с тем, что есть у ноутбуков: корневой мост PCI с _PRT, ссылки LNKx с _CRS через PCI-конфигурацию,
   встроенный контроллер (EC) с батареей, сетью питания, крышкой и датчиком температуры, кнопка питания, события GPE.
   Железо в тесте - модель (tests/aml_test.c), AML выполняется целиком настоящий. */
DefinitionBlock ("", "DSDT", 2, "KIRATS", "LAPTOP", 1)
{
    External (\_SB.NOSU, DeviceObj)
    Name (\_S5, Package () { 0x05, 0x00, 0x00, 0x00 })

    Scope (\_SB)
    {
        Name (INIV, 0)
        Method (_INI, 0, NotSerialized) { INIV = 0x77 }

        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (_CID, EisaId ("PNP0A03"))
            Name (_BBN, 0)
            Name (_PRT, Package ()
            {
                Package () { 0x0001FFFF, 0, LNKA, 0 },
                Package () { 0x0001FFFF, 1, LNKB, 0 },
                Package () { 0x0002FFFF, 0, LNKB, 0 },
                Package () { 0x0003FFFF, 3, LNKD, 0 },
                Package () { 0x0004FFFF, 0, 0, 19 },
                Package () { 0x001FFFFF, 2, LNKC, 0 }
            })

            Device (LPCB)
            {
                Name (_ADR, 0x001F0000)
                OperationRegion (PIRQ, PCI_Config, 0x60, 4)
                Field (PIRQ, ByteAcc, NoLock, Preserve)
                {
                    PIRA, 8, PIRB, 8, PIRC, 8, PIRD, 8
                }
            }
        }

        Method (LKST, 1, NotSerialized)         /* 0x0B - включена, 0x09 - отключена (бит 7 регистра PIRQ) */
        {
            If (Arg0 & 0x80) { Return (0x09) }
            Return (0x0B)
        }
        Method (LKCR, 1, NotSerialized)         /* _CRS ссылки: IRQ из регистра, в буфере ресурсов */
        {
            Name (BUF0, ResourceTemplate () { IRQ (Level, ActiveLow, Shared, ) {5} })
            CreateWordField (BUF0, 1, IRQW)
            If (Arg0 & 0x80) { IRQW = 0 } Else { IRQW = 1 << (Arg0 & 0x0F) }
            Return (BUF0)
        }
        Device (LNKA)
        {
            Name (_HID, EisaId ("PNP0C0F")) Name (_UID, 1)
            Method (_STA, 0, NotSerialized) { Return (LKST (\_SB.PCI0.LPCB.PIRA)) }
            Method (_CRS, 0, NotSerialized) { Return (LKCR (\_SB.PCI0.LPCB.PIRA)) }
        }
        Device (LNKB)
        {
            Name (_HID, EisaId ("PNP0C0F")) Name (_UID, 2)
            Method (_STA, 0, NotSerialized) { Return (LKST (\_SB.PCI0.LPCB.PIRB)) }
            Method (_CRS, 0, NotSerialized) { Return (LKCR (\_SB.PCI0.LPCB.PIRB)) }
        }
        Device (LNKC)
        {
            Name (_HID, EisaId ("PNP0C0F")) Name (_UID, 3)
            Method (_STA, 0, NotSerialized) { Return (LKST (\_SB.PCI0.LPCB.PIRC)) }
            Method (_CRS, 0, NotSerialized) { Return (LKCR (\_SB.PCI0.LPCB.PIRC)) }
        }
        Device (LNKD)
        {
            Name (_HID, EisaId ("PNP0C0F")) Name (_UID, 4)
            Method (_STA, 0, NotSerialized) { Return (LKST (\_SB.PCI0.LPCB.PIRD)) }
            Method (_CRS, 0, NotSerialized) { Return (LKCR (\_SB.PCI0.LPCB.PIRD)) }
        }

        Device (EC0)
        {
            Name (_HID, EisaId ("PNP0C09"))
            Name (_UID, 0)
            Name (_GPE, 0x1D)
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x0062, 0x0062, 0x00, 0x01)
                IO (Decode16, 0x0066, 0x0066, 0x00, 0x01)
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
            Method (_Q50, 0, NotSerialized)         /* запрос EC 0x50: батарея или сеть изменились */
            {
                Notify (\_SB.BAT0, 0x80)
                Notify (\_SB.ADP1, 0x80)
            }
            OperationRegion (ECOR, EmbeddedControl, 0, 0x100)
            Field (ECOR, ByteAcc, NoLock, Preserve)
            {
                BPRS, 1, ACPR, 1, LIDO, 1, , 5,
                Offset (0x04),
                BSTA, 8,
                BRAT, 16,
                BREM, 16,
                BVLT, 16,
                BDCP, 16,
                BLFC, 16,
                TMP0, 8
            }
        }

        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, 1)
            Method (_STA, 0, NotSerialized)
            {
                If (\_SB.EC0.BPRS) { Return (0x1F) }
                Return (0x0F)
            }
            Name (BIFP, Package ()
            {
                0x01, 0xFFFFFFFF, 0xFFFFFFFF, 0x01, 10800, 0x00, 0x00, 0x01, 0x01,
                "KiraTest", "0001", "LION", "Kira"
            })
            Method (_BIF, 0, NotSerialized)
            {
                BIFP [1] = \_SB.EC0.BDCP
                BIFP [2] = \_SB.EC0.BLFC
                Return (BIFP)
            }
            Name (BIXP, Package ()
            {
                0x00, 0x00, 0xFFFFFFFF, 0xFFFFFFFF, 0x01, 10800, 0x00, 0x00, 0x00, 100000,
                0x00, 0x00, 0x00, 0x00, 0x01, 0x01, "KiraTest", "0001", "LION", "Kira", 0x00
            })
            Method (_BIX, 0, NotSerialized)
            {
                BIXP [2] = \_SB.EC0.BDCP
                BIXP [3] = \_SB.EC0.BLFC
                Return (BIXP)
            }
            Name (BSTP, Package () { 0x00, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF })
            Method (_BST, 0, NotSerialized)
            {
                BSTP [0] = \_SB.EC0.BSTA & 0x07
                BSTP [1] = \_SB.EC0.BRAT
                BSTP [2] = \_SB.EC0.BREM
                BSTP [3] = \_SB.EC0.BVLT
                Return (BSTP)
            }
        }

        Device (ADP1)
        {
            Name (_HID, "ACPI0003")
            Method (_PSR, 0, NotSerialized) { Return (\_SB.EC0.ACPR) }
            Method (_PCL, 0, NotSerialized) { Return (Package () { \_SB }) }
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID, 0, NotSerialized) { Return (\_SB.EC0.LIDO) }
        }

        Device (PWRB)
        {
            Name (_HID, EisaId ("PNP0C0C"))
        }
    }

    Scope (\_TZ)
    {
        ThermalZone (TZ0)
        {
            Method (_TMP, 0, NotSerialized) { Return ((\_SB.EC0.TMP0 * 10) + 2732) }   /* десятые доли Кельвина */
            Method (_CRT, 0, NotSerialized) { Return (3732) }
        }
    }

    Scope (\_GPE)
    {
        Method (_L1C, 0, NotSerialized)         /* уровневое событие GPE 0x1C: батарея и сеть питания изменились */
        {
            Notify (\_SB.BAT0, 0x80)
            Notify (\_SB.ADP1, 0x80)
        }
        Method (_E05, 0, NotSerialized) { Notify (\_SB.LID0, 0x80) }
    }

    /* Набор проверок самого интерпретатора: ожидаемые значения в tests/aml_test.c посчитаны вручную. */
    Device (TST)
    {
        Name (_HID, "KIRA0001")
        Name (BUFA, Buffer (8) { 1, 2, 3, 4, 5, 6, 7, 8 })
        CreateDWordField (BUFA, 0, DW0)
        CreateWordField (BUFA, 4, WD4)
        CreateByteField (BUFA, 7, BY7)
        CreateField (BUFA, 12, 6, FLD)
        Name (PKG0, Package () { 10, 20, "abc", Package () { 7, 8 } })
        Name (NUM, 5)
        Name (STR0, "Kira")
        Method (TADD, 2, NotSerialized) { Return (Arg0 + Arg1) }
        Method (TSUB, 2, NotSerialized) { Return (Arg0 - Arg1) }
        Method (TMUL, 2, NotSerialized) { Return (Arg0 * Arg1) }
        Method (TDIV, 2, NotSerialized) { Local0 = Arg0 / Arg1  Return (Local0) }
        Method (TMOD, 2, NotSerialized) { Return (Arg0 % Arg1) }
        Method (TSHL, 2, NotSerialized) { Return (Arg0 << Arg1) }
        Method (TSHR, 2, NotSerialized) { Return (Arg0 >> Arg1) }
        Method (TBIT, 2, NotSerialized) { Return ((Arg0 & Arg1) | ((Arg0 ^ Arg1) << 8)) }
        Method (TNOT, 1, NotSerialized) { Not (Arg0, Local0) Return (Local0) }
        Method (TLGC, 2, NotSerialized)
        {
            Local0 = 0
            If (Arg0 == Arg1) { Local0 |= 1 }
            If (Arg0 != Arg1) { Local0 |= 2 }
            If (Arg0 < Arg1) { Local0 |= 4 }
            If (Arg0 > Arg1) { Local0 |= 8 }
            If (Arg0 <= Arg1) { Local0 |= 16 }
            If (Arg0 >= Arg1) { Local0 |= 32 }
            If (Arg0 && Arg1) { Local0 |= 64 }
            If (Arg0 || Arg1) { Local0 |= 128 }
            If (!Arg0) { Local0 |= 256 }
            Return (Local0)
        }
        Method (TSUM, 1, NotSerialized)         /* 1+2+...+n циклом While с Break и Continue */
        {
            Local0 = 0
            Local1 = 1
            While (One)
            {
                If (Local1 > Arg0) { Break }
                If (Local1 == 3) { Increment (Local1) Continue }
                Add (Local0, Local1, Local0)
                Increment (Local1)
            }
            Return (Local0)
        }
        Method (TFAC, 1, NotSerialized) { If (Arg0 <= 1) { Return (1) } Return (Arg0 * TFAC (Arg0 - 1)) }
        Method (TREC, 0, NotSerialized) { Return (TREC ()) }
        Method (TSPN, 0, NotSerialized) { Local0 = 0 While (One) { Increment (Local0) } }
        Method (TNAP, 1, NotSerialized) { Sleep (Arg0) Return (1) }
        Method (TFND, 1, NotSerialized)
        {
            Local0 = Match (PKG0, MEQ, Arg0, MTR, 0, 0)
            Return (Local0)
        }
        Method (TGTD, 0, NotSerialized) { Return (DW0) }
        Method (TGTW, 0, NotSerialized) { Return (WD4) }
        Method (TGTB, 0, NotSerialized) { Return (BY7) }
        Method (TGTF, 0, NotSerialized) { Return (FLD) }
        Method (TPTD, 1, NotSerialized) { DW0 = Arg0 Return (BUFA) }
        Method (TPTF, 1, NotSerialized) { FLD = Arg0 Return (BUFA) }
        Method (TPKI, 1, NotSerialized) { Return (DerefOf (PKG0 [Arg0])) }
        Method (TPKN, 0, NotSerialized) { Return (SizeOf (PKG0)) }
        Method (TINR, 0, NotSerialized) { Return (DerefOf (DerefOf (PKG0 [3]) [1])) }
        Method (TSTL, 0, NotSerialized) { Return (SizeOf (STR0)) }
        Method (TCAT, 0, NotSerialized) { Return (Concatenate (STR0, "-OS")) }
        Method (TMID, 0, NotSerialized) { Return (Mid ("ABCDEFG", 2, 3)) }
        Method (TTOH, 1, NotSerialized) { Return (ToHexString (Arg0)) }
        Method (TTOD, 1, NotSerialized) { Return (ToDecimalString (Arg0)) }
        Method (TBCD, 1, NotSerialized) { ToBCD (Arg0, Local0) Return (Local0) }
        Method (TFBC, 1, NotSerialized) { FromBCD (Arg0, Local0) Return (Local0) }
        Method (TBFX, 0, NotSerialized)         /* буфер на месте: Index в буфер, запись байта */
        {
            Name (B, Buffer (4) { 0, 0, 0, 0 })
            B [2] = 0x5A
            Return (B)
        }
        Method (TSET, 1, NotSerialized) { NUM = Arg0 Return (NUM) }
        Method (TLOC, 0, NotSerialized)
        {
            Local0 = 5
            Local1 = Local0
            Local0 = 6
            Return ((Local1 * 10) + Local0)
        }
        Method (TREF, 0, NotSerialized)
        {
            Local0 = 41
            Local1 = RefOf (Local0)
            Return (DerefOf (Local1) + 1)
        }
        Method (TOBJ, 1, NotSerialized) { Return (ObjectType (Arg0)) }
        Method (TCND, 0, NotSerialized)
        {
            Local0 = 0
            If (CondRefOf (\_SB.NOSU, Local1)) { Local0 |= 1 }
            If (CondRefOf (\_SB.BAT0, Local1)) { Local0 |= 2 }
            Return (Local0)
        }
        Method (TFSL, 1, NotSerialized) { Return (FindSetLeftBit (Arg0)) }
        Method (TFSR, 1, NotSerialized) { Return (FindSetRightBit (Arg0)) }
        Method (TOSI, 1, NotSerialized) { Return (\_OSI (Arg0)) }
        Method (TOSN, 0, NotSerialized) { Return (\_OS) }
        Method (TOOB, 0, NotSerialized) { Return (DerefOf (PKG0 [9])) }
        Method (TDV0, 1, NotSerialized) { Return (10 / Arg0) }
        Method (TMEM, 0, NotSerialized)         /* область SystemMemory и поля разной ширины */
        {
            Return (\TST.MR32)
        }
        OperationRegion (MEMO, SystemMemory, 0xFED00000, 0x40)
        Field (MEMO, DWordAcc, NoLock, Preserve) { MR32, 32, MR16, 16, , 16, MRQ, 64 }
        OperationRegion (IOO, SystemIO, 0x1000, 8)
        Field (IOO, ByteAcc, NoLock, WriteAsZeros) { IO00, 8, IO01, 4, IO02, 4 }
        Method (TIOW, 1, NotSerialized) { IO01 = Arg0 Return (IO01) }
        Method (TMRQ, 0, NotSerialized) { Return (MRQ) }
        OperationRegion (IDXR, SystemIO, 0x2000, 2)
        Field (IDXR, ByteAcc, NoLock, Preserve) { IDX, 8, DAT, 8 }
        IndexField (IDX, DAT, ByteAcc, NoLock, Preserve) { IF00, 8, IF01, 16, IF02, 8 }
        Method (TIFR, 0, NotSerialized) { Return (IF01) }
        Method (TIFW, 1, NotSerialized) { IF02 = Arg0 Return (IF02) }
    }
}
