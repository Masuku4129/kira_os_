// Go-эквиваленты 4 нагрузок (только справка: рантайм Go не freestanding; PARTIAL). Отдельная программа, печатает результаты четырёх нагрузок.
package main

import (
	"fmt"
	"math/bits"
	"os"
)

var K = [64]uint32{0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2}

func ror(x uint32, n int) uint32 { return bits.RotateLeft32(x, -n) }

func blk(st *[8]uint32, p []byte) {
	var w [64]uint32
	for i := 0; i < 16; i++ {
		w[i] = uint32(p[4*i])<<24 | uint32(p[4*i+1])<<16 | uint32(p[4*i+2])<<8 | uint32(p[4*i+3])
	}
	for i := 16; i < 64; i++ {
		s0 := ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3)
		s1 := ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10)
		w[i] = w[i-16] + s0 + w[i-7] + s1
	}
	a, b, c, d, e, f, g, h := st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]
	for i := 0; i < 64; i++ {
		t1 := h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (^e & g)) + K[i] + w[i]
		t2 := (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2
	}
	st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h
}

var buf [64 * 1024]byte

func sha256() int {
	st := [8]uint32{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}
	for i := range buf {
		buf[i] = byte(i*7 + 3)
	}
	for r := 0; r < 60; r++ {
		for o := 0; o < len(buf); o += 64 {
			blk(&st, buf[o:o+64])
		}
	}
	return int((st[0] ^ st[3] ^ st[7]) & 0xffffff)
}

var v [200000]int32

func qs(a []int32, lo, hi int) {
	for lo < hi {
		p := a[(lo+hi)/2]
		i, j := lo, hi
		for i <= j {
			for a[i] < p { i++ }
			for a[j] > p { j-- }
			if i <= j { a[i], a[j] = a[j], a[i]; i++; j-- }
		}
		if j-lo < hi-i { qs(a, lo, j); lo = i } else { qs(a, i, hi); hi = j }
	}
}

func sortw() int {
	var x uint32 = 12345
	var chk int64
	for r := 0; r < 20; r++ {
		for i := 0; i < 200000; i++ {
			x = x*1664525 + 1013904223
			v[i] = int32(x >> 8)
		}
		qs(v[:], 0, 199999)
		for i := 1; i < 200000; i++ {
			if v[i-1] > v[i] { return 1 }
		}
		chk += int64(v[100000])
	}
	return int(chk & 0xffffff)
}

var A, B, C [120][120]int32

func matmul() int {
	for i := 0; i < 120; i++ {
		for j := 0; j < 120; j++ {
			A[i][j] = int32(i + j)
			B[i][j] = int32(i - j)
		}
	}
	var t int64
	for r := 0; r < 40; r++ {
		for i := 0; i < 120; i++ {
			for j := 0; j < 120; j++ {
				var s int32
				for k := 0; k < 120; k++ { s += A[i][k] * B[k][j] }
				C[i][j] = s
			}
		}
		t += int64(C[r][r])
	}
	return int(t & 0xffffff)
}

var T [1 << 20]byte

func cnt(s []byte, c byte) int { k := 0; for i := range s { if s[i] == c { k++ } }; return k }
func fnv(s []byte) uint32 {
	h := uint32(2166136261)
	for i := range s { h ^= uint32(s[i]); h *= 16777619 }
	return h
}
func strscan() int {
	for i := 0; i < 1<<20; i++ { T[i] = byte('a' + (i*31+(i>>5))%26) }
	var s int64
	for r := 0; r < 80; r++ {
		s += int64(cnt(T[:], 'e'))
		s ^= int64(fnv(T[:]))
		T[r]++
	}
	return int(s & 0xffffff)
}

func main() {
	if len(os.Args) < 2 { fmt.Println("usage: bench4 sha256|sort|matmul|strscan"); os.Exit(2) }
	switch os.Args[1] {
	case "sha256": fmt.Println(sha256())
	case "sort": fmt.Println(sortw())
	case "matmul": fmt.Println(matmul())
	case "strscan": fmt.Println(strscan())
	}
}
