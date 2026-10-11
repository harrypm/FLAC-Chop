//! Minimal SHA-256 (FIPS 180-4) for output-file checksums.
//!
//! The "safe to delete the source" workflow ends with a converted FLAC whose
//! only provenance is this tool. After a successful cut, the core appends a
//! `sha256(output): <hex>` note with this module's digest of the completed
//! output FILE BYTES — the same value `sha256sum <file>` / `certutil
//! -hashfile <file> SHA256` reports, so the file can be re-checked at any
//! time after the source has been deleted. Self-contained (no external
//! crate) — the core keeps its dependency set minimal; the implementation
//! is the standard 64-round compression with a 1 MiB streaming file reader,
//! so a multi-GB output is never loaded into memory.

use std::io::Read;
use std::path::Path;

/// The first 32 bits of the fractional parts of the cube roots of the first
/// 64 primes (FIPS 180-4 §4.2.2).
const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

/// Streaming SHA-256 state.
pub struct Sha256 {
    state: [u32; 8],
    buf: [u8; 64],
    buf_len: usize,
    /// Total message length in BYTES (bit length is derived at padding).
    len: u64,
}

impl Default for Sha256 {
    fn default() -> Self {
        Self::new()
    }
}

impl Sha256 {
    /// The initial hash values (§5.3.3): the first 32 bits of the fractional
    /// parts of the square roots of the first 8 primes.
    pub fn new() -> Self {
        Sha256 {
            state: [
                0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
            ],
            buf: [0u8; 64],
            buf_len: 0,
            len: 0,
        }
    }

    /// Feed a message chunk (any length; the 64-byte block boundary is
    /// handled internally).
    pub fn update(&mut self, mut data: &[u8]) {
        self.len = self.len.wrapping_add(data.len() as u64);
        if self.buf_len > 0 {
            let take = (64 - self.buf_len).min(data.len());
            self.buf[self.buf_len..self.buf_len + take].copy_from_slice(&data[..take]);
            self.buf_len += take;
            data = &data[take..];
            if self.buf_len == 64 {
                let block = self.buf;
                self.compress(&block);
                self.buf_len = 0;
            }
        }
        while data.len() >= 64 {
            let mut block = [0u8; 64];
            block.copy_from_slice(&data[..64]);
            self.compress(&block);
            data = &data[64..];
        }
        if !data.is_empty() {
            self.buf[..data.len()].copy_from_slice(data);
            self.buf_len = data.len();
        }
    }

    /// Finish: pad (0x80, zeros, 64-bit big-endian BIT length) and return
    /// the digest. Consumes self, matching the one-shot use pattern.
    pub fn finalize(mut self) -> [u8; 32] {
        let bit_len = self.len.wrapping_mul(8);
        self.update(&[0x80]);
        // update() already counted the pad byte into len, but the recorded
        // bit length must be the ORIGINAL message's — recompute from the
        // pre-pad value saved above (update() tracked it in self.len, so use
        // the saved value for the final block).
        while self.buf_len != 56 {
            self.update(&[0x00]);
        }
        let mut len_be = [0u8; 8];
        len_be.copy_from_slice(&bit_len.to_be_bytes());
        self.update(&len_be);
        let mut out = [0u8; 32];
        for (i, w) in self.state.iter().enumerate() {
            out[i * 4..i * 4 + 4].copy_from_slice(&w.to_be_bytes());
        }
        out
    }

    /// The 64-round compression function over one complete block.
    fn compress(&mut self, block: &[u8; 64]) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([
                block[i * 4],
                block[i * 4 + 1],
                block[i * 4 + 2],
                block[i * 4 + 3],
            ]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = self.state;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ ((!e) & g);
            let temp1 = h
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let temp2 = s0.wrapping_add(maj);
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(temp1);
            d = c;
            c = b;
            b = a;
            a = temp1.wrapping_add(temp2);
        }
        self.state[0] = self.state[0].wrapping_add(a);
        self.state[1] = self.state[1].wrapping_add(b);
        self.state[2] = self.state[2].wrapping_add(c);
        self.state[3] = self.state[3].wrapping_add(d);
        self.state[4] = self.state[4].wrapping_add(e);
        self.state[5] = self.state[5].wrapping_add(f);
        self.state[6] = self.state[6].wrapping_add(g);
        self.state[7] = self.state[7].wrapping_add(h);
    }
}

/// One-shot digest of a byte slice, as lowercase hex.
pub fn hex_digest(data: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update(data);
    to_hex(&h.finalize())
}

/// Streaming digest of the file at `path` (1 MiB reads; the file is never
/// held in memory), as lowercase hex.
pub fn file_sha256(path: &Path) -> Result<String, String> {
    let mut f = std::fs::File::open(path).map_err(|e| format!("open failed: {e}"))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf).map_err(|e| format!("read failed: {e}"))?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(to_hex(&h.finalize()))
}

fn to_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The standard FIPS 180-4 / NIST test vectors.
    #[test]
    fn nist_vectors() {
        assert_eq!(
            hex_digest(b""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            hex_digest(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(
            hex_digest(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        );
        // The million-'a' vector (verified against two independent
        // implementations: Windows CNG Get-FileHash and coreutils sha256sum
        // on the generated file — both cdc76e5c…, NOT the commonly mis-typed
        // cdc76e5e…).
        assert_eq!(
            hex_digest(&[0x61; 1_000_000]),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
        );
    }

    /// Update chunking must not matter: one 1 MiB feed == many odd-size feeds.
    #[test]
    fn streaming_matches_one_shot() {
        let data: Vec<u8> = (0u32..250_000).map(|v| (v * 7 + 3) as u8).collect();
        let one = hex_digest(&data);
        let mut h = Sha256::new();
        let mut off = 0;
        let mut chunk = 1;
        while off < data.len() {
            let end = (off + chunk).min(data.len());
            h.update(&data[off..end]);
            off = end;
            chunk = chunk * 3 + 7; // deliberately odd, crossing block boundaries
        }
        assert_eq!(to_hex(&h.finalize()), one);
    }

    /// The file reader produces the same digest as the in-memory path.
    #[test]
    fn file_digest_matches_memory() {
        let path = std::env::temp_dir().join("fc_test_sha256_file.bin");
        let data: Vec<u8> = (0u32..100_000).map(|v| (v ^ 0x5a) as u8).collect();
        std::fs::write(&path, &data).unwrap();
        assert_eq!(file_sha256(&path).unwrap(), hex_digest(&data));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn missing_file_errors_cleanly() {
        assert!(file_sha256(Path::new("/nonexistent-file-for-sha256")).is_err());
    }
}
