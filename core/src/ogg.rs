//! Ogg FLAC (FLAC-in-Ogg) support.
//!
//! The vhs-decode / ld-decode `.ldf` RF capture format is **Ogg-wrapped FLAC**
//! (`ffmpeg … -acodec flac -f ogg out.ldf`, see `ld-compress` /
//! `lds-compress.bat` in vhs-decode), not native FLAC. The rest of FLAC-Chop
//! (claxon, the STREAMINFO repair, the tag splice, and — critically — the
//! bundled SoX, which is built `--without-ogg` with a libFLAC built
//! `WITH_OGG=OFF`) only understands native FLAC (`fLaC` marker at byte 0).
//!
//! This module bridges the gap without re-encoding anything:
//!
//!  * [`sniff_ogg`] recognises an Ogg FLAC stream by content (never by
//!    extension) and names other Ogg codecs so the user gets a real error.
//!  * [`read_header`] parses the mapping header + metadata packets (STREAMINFO,
//!    Vorbis comments) and exposes them as a native-FLAC metadata chain, so
//!    claxon can read the tags/STREAMINFO from it.
//!  * [`last_granule`] returns the **exact** sample count from the last valid
//!    Ogg page's granule position (64-bit, so it never wraps like the 36-bit
//!    STREAMINFO field, and it is correct even when the encoder was piped and
//!    never finalised STREAMINFO — the ffmpeg-written `.ldf` case).
//!  * [`remux_window`] extracts just the pages needed for a cut, located by a
//!    binary search over granule positions, into a standalone **native FLAC**
//!    file: Ogg packets *are* FLAC frames, so the frame bytes are copied
//!    losslessly. Frame/sample numbers in each frame header are rebased to 0
//!    (with CRC-8/CRC-16 recomputed) so the temp file is a fully valid
//!    stand-alone FLAC that libFLAC/SoX can seek in. The existing SoX / 12-bit
//!    pipeline then runs on that temp file unchanged, with the start offset
//!    shifted by [`Window::first_sample`].

use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::Path;

use crate::probe::{parse_frame_header, read_utf8_coded};

/// Granule position value meaning "no packet finishes on this page" (-1).
pub const NO_GRANULE: u64 = u64::MAX;

const PAGE_HDR_LEN: usize = 27;

// ---------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------

/// Ogg page CRC-32 (poly 0x04C11DB7, init 0, MSB-first, no final xor).
const OGG_CRC_TABLE: [u32; 256] = make_ogg_crc_table();

const fn make_ogg_crc_table() -> [u32; 256] {
    let mut t = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut r = (i as u32) << 24;
        let mut j = 0;
        while j < 8 {
            r = if r & 0x8000_0000 != 0 { (r << 1) ^ 0x04C1_1DB7 } else { r << 1 };
            j += 1;
        }
        t[i] = r;
        i += 1;
    }
    t
}

fn ogg_crc_update(mut c: u32, data: &[u8]) -> u32 {
    for &b in data {
        c = (c << 8) ^ OGG_CRC_TABLE[(((c >> 24) as u8) ^ b) as usize];
    }
    c
}

/// FLAC frame-header CRC-8 (poly 0x07, init 0).
const CRC8_TABLE: [u8; 256] = make_crc8_table();

const fn make_crc8_table() -> [u8; 256] {
    let mut t = [0u8; 256];
    let mut i = 0;
    while i < 256 {
        let mut r = i as u8;
        let mut j = 0;
        while j < 8 {
            r = if r & 0x80 != 0 { (r << 1) ^ 0x07 } else { r << 1 };
            j += 1;
        }
        t[i] = r;
        i += 1;
    }
    t
}

fn crc8(data: &[u8]) -> u8 {
    let mut c = 0u8;
    for &b in data {
        c = CRC8_TABLE[(c ^ b) as usize];
    }
    c
}

/// FLAC frame CRC-16 (poly 0x8005, init 0).
const CRC16_TABLE: [u16; 256] = make_crc16_table();

const fn make_crc16_table() -> [u16; 256] {
    let mut t = [0u16; 256];
    let mut i = 0;
    while i < 256 {
        let mut r = (i as u16) << 8;
        let mut j = 0;
        while j < 8 {
            r = if r & 0x8000 != 0 { (r << 1) ^ 0x8005 } else { r << 1 };
            j += 1;
        }
        t[i] = r;
        i += 1;
    }
    t
}

fn crc16(data: &[u8]) -> u16 {
    let mut c = 0u16;
    for &b in data {
        c = (c << 8) ^ CRC16_TABLE[(((c >> 8) as u8) ^ b) as usize];
    }
    c
}

// ---------------------------------------------------------------------------
// Sniffing
// ---------------------------------------------------------------------------

/// What an `OggS` file contains (decided from the first page's payload).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum OggKind {
    /// Not an Ogg stream at all.
    NotOgg,
    /// Ogg FLAC (`0x7F "FLAC"` mapping header).
    Flac,
    /// Some other Ogg codec (Vorbis, Opus, Speex, Theora, unknown).
    Other(&'static str),
}

/// Classify the first bytes of a file. `bytes` must hold at least the first
/// Ogg page header + segment table + 8 payload bytes (64 bytes is enough for
/// any real first page).
pub fn sniff_ogg(bytes: &[u8]) -> OggKind {
    if bytes.len() < 4 || &bytes[..4] != b"OggS" {
        return OggKind::NotOgg;
    }
    if bytes.len() <= PAGE_HDR_LEN {
        return OggKind::Other("unknown");
    }
    let nseg = bytes[26] as usize;
    let payload = bytes.get(PAGE_HDR_LEN + nseg..).unwrap_or(&[]);
    if payload.starts_with(b"\x7fFLAC") {
        OggKind::Flac
    } else if payload.starts_with(b"\x01vorbis") {
        OggKind::Other("Vorbis")
    } else if payload.starts_with(b"OpusHead") {
        OggKind::Other("Opus")
    } else if payload.starts_with(b"Speex   ") {
        OggKind::Other("Speex")
    } else if payload.starts_with(b"\x80theora") {
        OggKind::Other("Theora")
    } else {
        OggKind::Other("unknown")
    }
}

// ---------------------------------------------------------------------------
// Ogg pages
// ---------------------------------------------------------------------------

/// One parsed, CRC-verified Ogg page.
#[derive(Debug)]
pub struct Page {
    /// Byte offset of the page start in the file.
    pub offset: u64,
    pub header_type: u8,
    /// Granule position; [`NO_GRANULE`] when no packet completes here.
    pub granule: u64,
    pub serial: u32,
    pub segments: Vec<u8>,
    pub payload: Vec<u8>,
}

impl Page {
    pub fn len(&self) -> u64 {
        (PAGE_HDR_LEN + self.segments.len() + self.payload.len()) as u64
    }
    /// Offset of the byte just past this page (the next page's start).
    pub fn end(&self) -> u64 {
        self.offset + self.len()
    }
    /// True when this page continues a packet begun on the previous page.
    pub fn continued(&self) -> bool {
        self.header_type & 0x01 != 0
    }
    pub fn bos(&self) -> bool {
        self.header_type & 0x02 != 0
    }
}

/// Why a page could not be read.
#[derive(Debug)]
pub enum PageErr {
    /// Clean end of file exactly at a page boundary.
    Eof,
    /// The file ends inside a page (interrupted / truncated capture).
    Truncated,
    /// Not a valid page (no capture pattern, bad version, CRC mismatch).
    Bad(String),
}

fn read_fully<R: Read>(r: &mut R, buf: &mut [u8]) -> std::io::Result<usize> {
    let mut n = 0;
    while n < buf.len() {
        match r.read(&mut buf[n..]) {
            Ok(0) => break,
            Ok(k) => n += k,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(n)
}

/// Read one page at the reader's current position (`offset` is only used for
/// bookkeeping / messages). The page CRC is verified.
pub fn read_page<R: Read>(r: &mut R, offset: u64) -> Result<Page, PageErr> {
    let mut h = [0u8; PAGE_HDR_LEN];
    let n = read_fully(r, &mut h).map_err(|e| PageErr::Bad(format!("read error at {offset}: {e}")))?;
    if n == 0 {
        return Err(PageErr::Eof);
    }
    if n < PAGE_HDR_LEN {
        return Err(PageErr::Truncated);
    }
    if &h[..4] != b"OggS" {
        return Err(PageErr::Bad(format!("no Ogg capture pattern at offset {offset}")));
    }
    if h[4] != 0 {
        return Err(PageErr::Bad(format!("unsupported Ogg page version {} at offset {offset}", h[4])));
    }
    let nseg = h[26] as usize;
    let mut segments = vec![0u8; nseg];
    if read_fully(r, &mut segments).map_err(|e| PageErr::Bad(e.to_string()))? < nseg {
        return Err(PageErr::Truncated);
    }
    let plen: usize = segments.iter().map(|&s| s as usize).sum();
    let mut payload = vec![0u8; plen];
    if read_fully(r, &mut payload).map_err(|e| PageErr::Bad(e.to_string()))? < plen {
        return Err(PageErr::Truncated);
    }
    let stored = u32::from_le_bytes([h[22], h[23], h[24], h[25]]);
    let mut hz = h;
    hz[22..26].copy_from_slice(&[0, 0, 0, 0]);
    let mut c = ogg_crc_update(0, &hz);
    c = ogg_crc_update(c, &segments);
    c = ogg_crc_update(c, &payload);
    if c != stored {
        return Err(PageErr::Bad(format!(
            "Ogg page CRC mismatch at offset {offset} (corrupt capture)"
        )));
    }
    Ok(Page {
        offset,
        header_type: h[5],
        granule: u64::from_le_bytes([h[6], h[7], h[8], h[9], h[10], h[11], h[12], h[13]]),
        serial: u32::from_le_bytes([h[14], h[15], h[16], h[17]]),
        segments,
        payload,
    })
}

fn read_page_at(f: &mut File, offset: u64) -> Result<Page, PageErr> {
    f.seek(SeekFrom::Start(offset))
        .map_err(|e| PageErr::Bad(format!("seek failed: {e}")))?;
    read_page(f, offset)
}

fn page_err(e: PageErr, what: &str) -> String {
    match e {
        PageErr::Eof => format!("{what}: unexpected end of file"),
        PageErr::Truncated => format!("{what}: file ends inside an Ogg page"),
        PageErr::Bad(m) => format!("{what}: {m}"),
    }
}

/// First valid page (same `serial`) whose start is in `[from, limit)`.
fn find_page_at_or_after(
    f: &mut File,
    from: u64,
    limit: u64,
    serial: u32,
) -> Result<Option<Page>, String> {
    let mut pos = from;
    while pos + PAGE_HDR_LEN as u64 <= limit {
        let want = (limit - pos).min(256 * 1024) as usize;
        f.seek(SeekFrom::Start(pos)).map_err(|e| e.to_string())?;
        let mut buf = vec![0u8; want];
        let n = read_fully(f, &mut buf).map_err(|e| e.to_string())?;
        if n < 4 {
            break;
        }
        let mut i = 0;
        while i + 4 <= n {
            if &buf[i..i + 4] == b"OggS" {
                if let Ok(p) = read_page_at(f, pos + i as u64) {
                    if p.serial == serial && p.offset < limit {
                        return Ok(Some(p));
                    }
                }
            }
            i += 1;
        }
        // Overlap 3 bytes so a capture pattern split across reads is found.
        pos += (n.saturating_sub(3)).max(1) as u64;
    }
    Ok(None)
}

/// Split a page's payload into packets. `carry` holds the unfinished tail of a
/// packet that continues onto the next page. Complete packets are appended to
/// `out`.
fn split_packets(page: &Page, carry: &mut Vec<u8>, out: &mut Vec<Vec<u8>>) {
    if !page.continued() && !carry.is_empty() {
        // A packet was left unfinished but this page does not continue it:
        // the stream was damaged. Drop the partial packet.
        carry.clear();
    }
    let mut pos = 0usize;
    for &seg in &page.segments {
        let s = seg as usize;
        carry.extend_from_slice(&page.payload[pos..pos + s]);
        pos += s;
        if seg < 255 {
            out.push(std::mem::take(carry));
        }
    }
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

/// Parsed Ogg FLAC headers.
#[derive(Debug, Clone)]
pub struct OggFlacHeader {
    /// Logical stream serial number.
    pub serial: u32,
    /// FLAC metadata blocks in order: `(type, payload)`; the first is
    /// STREAMINFO (34 bytes).
    pub blocks: Vec<(u8, Vec<u8>)>,
    /// File offset of the first audio page.
    pub audio_offset: u64,
}

impl OggFlacHeader {
    pub fn streaminfo(&self) -> &[u8] {
        &self.blocks[0].1
    }

    /// The header as a native FLAC metadata chain (`fLaC` + blocks, the last
    /// block flagged), so native-FLAC readers (claxon) can parse it.
    pub fn native_bytes(&self) -> Vec<u8> {
        let mut v = b"fLaC".to_vec();
        let last = self.blocks.len() - 1;
        for (i, (kind, payload)) in self.blocks.iter().enumerate() {
            push_block(&mut v, *kind, payload, i == last);
        }
        v
    }

    fn vorbis_comment(&self) -> Option<&[u8]> {
        self.blocks.iter().find(|(k, _)| *k == 4).map(|(_, p)| p.as_slice())
    }
}

fn push_block(out: &mut Vec<u8>, kind: u8, payload: &[u8], last: bool) {
    let len = payload.len();
    out.push(kind | if last { 0x80 } else { 0 });
    out.extend_from_slice(&[(len >> 16) as u8, (len >> 8) as u8, len as u8]);
    out.extend_from_slice(payload);
}

/// Parse the mapping header + metadata packets of an Ogg FLAC file.
pub fn read_header(path: &Path) -> Result<OggFlacHeader, String> {
    let f = File::open(path).map_err(|e| format!("open failed: {e}"))?;
    let mut r = BufReader::new(f);
    let mut off = 0u64;
    let mut carry: Vec<u8> = Vec::new();
    let mut serial: Option<u32> = None;
    let mut blocks: Vec<(u8, Vec<u8>)> = Vec::new();
    let mut packets_seen = 0usize;
    let mut done = false;

    for _ in 0..4096 {
        let page = read_page(&mut r, off).map_err(|e| page_err(e, "Ogg FLAC header"))?;
        let pend = page.end();
        match serial {
            None => {
                if !page.bos() {
                    return Err("not an Ogg FLAC stream: the first Ogg page is not a BOS page".into());
                }
                serial = Some(page.serial);
            }
            Some(s) if page.serial != s => {
                off = pend;
                continue;
            }
            _ => {}
        }
        let mut pk: Vec<Vec<u8>> = Vec::new();
        split_packets(&page, &mut carry, &mut pk);
        for p in pk {
            if done {
                return Err(
                    "Ogg FLAC audio data shares a page with the header packets (unsupported layout)".into(),
                );
            }
            if packets_seen == 0 {
                // Mapping header: 0x7F "FLAC" major minor nheaders(2, BE) "fLaC"
                // then the first metadata block (STREAMINFO).
                if p.len() < 13 + 4 + 34 || p[0] != 0x7f || &p[1..5] != b"FLAC" || &p[9..13] != b"fLaC" {
                    return Err("not an Ogg FLAC stream (bad mapping header)".into());
                }
                let mut pos = 13usize;
                loop {
                    if pos + 4 > p.len() {
                        return Err("Ogg FLAC mapping header: truncated metadata block".into());
                    }
                    let kind = p[pos] & 0x7f;
                    let last = p[pos] & 0x80 != 0;
                    let len = (usize::from(p[pos + 1]) << 16) | (usize::from(p[pos + 2]) << 8) | usize::from(p[pos + 3]);
                    if pos + 4 + len > p.len() {
                        return Err("Ogg FLAC mapping header: metadata block overruns the packet".into());
                    }
                    blocks.push((kind, p[pos + 4..pos + 4 + len].to_vec()));
                    pos += 4 + len;
                    if last {
                        done = true;
                    }
                    if pos >= p.len() || last {
                        break;
                    }
                }
                if blocks[0].0 != 0 || blocks[0].1.len() != 34 {
                    return Err("Ogg FLAC: first metadata block is not a 34-byte STREAMINFO".into());
                }
            } else {
                if p.len() < 4 {
                    return Err("Ogg FLAC: truncated metadata packet".into());
                }
                let kind = p[0] & 0x7f;
                let last = p[0] & 0x80 != 0;
                let len = (usize::from(p[1]) << 16) | (usize::from(p[2]) << 8) | usize::from(p[3]);
                if 4 + len != p.len() {
                    return Err(format!(
                        "Ogg FLAC: metadata packet length mismatch (header says {len}, packet holds {})",
                        p.len() - 4
                    ));
                }
                blocks.push((kind, p[4..].to_vec()));
                if last {
                    done = true;
                }
            }
            packets_seen += 1;
        }
        if done {
            if !carry.is_empty() {
                return Err(
                    "Ogg FLAC audio data shares a page with the header packets (unsupported layout)".into(),
                );
            }
            return Ok(OggFlacHeader {
                serial: serial.unwrap(),
                blocks,
                audio_offset: pend,
            });
        }
        off = pend;
    }
    Err("Ogg FLAC header is incomplete (no last-metadata-block flag found)".into())
}

// ---------------------------------------------------------------------------
// Exact length
// ---------------------------------------------------------------------------

/// Exact total sample count: the granule position of the last valid page that
/// has one. Scans backwards through a growing tail window, verifying page
/// CRCs, so a truncated / interrupted capture still yields the length of the
/// audio that is actually present. `None` only if no page with a granule can
/// be found after the headers.
pub fn last_granule(path: &Path, hdr: &OggFlacHeader) -> Option<u64> {
    let mut f = File::open(path).ok()?;
    let size = f.metadata().ok()?.len();
    for window in [256u64 << 10, 4 << 20, 64 << 20, u64::MAX] {
        let start = size.saturating_sub(window).max(hdr.audio_offset);
        if size <= start {
            return None;
        }
        f.seek(SeekFrom::Start(start)).ok()?;
        let mut buf = vec![0u8; (size - start) as usize];
        if read_fully(&mut f, &mut buf).ok()? != buf.len() {
            return None;
        }
        if let Some(g) = scan_last_granule(&buf, hdr.serial) {
            return Some(g);
        }
        if start == hdr.audio_offset {
            return None;
        }
    }
    None
}

/// Walk an in-memory byte range, returning the granule of the last CRC-valid
/// page (of `serial`) that carries one.
fn scan_last_granule(buf: &[u8], serial: u32) -> Option<u64> {
    let mut best: Option<u64> = None;
    let mut i = 0usize;
    while i + PAGE_HDR_LEN <= buf.len() {
        if &buf[i..i + 4] != b"OggS" || buf[i + 4] != 0 {
            i += 1;
            continue;
        }
        let nseg = buf[i + 26] as usize;
        let seg_end = i + PAGE_HDR_LEN + nseg;
        if seg_end > buf.len() {
            i += 1;
            continue;
        }
        let plen: usize = buf[i + PAGE_HDR_LEN..seg_end].iter().map(|&s| s as usize).sum();
        let end = seg_end + plen;
        if end > buf.len() {
            i += 1;
            continue;
        }
        let stored = u32::from_le_bytes([buf[i + 22], buf[i + 23], buf[i + 24], buf[i + 25]]);
        let mut hz = [0u8; PAGE_HDR_LEN];
        hz.copy_from_slice(&buf[i..i + PAGE_HDR_LEN]);
        hz[22..26].copy_from_slice(&[0, 0, 0, 0]);
        let mut c = ogg_crc_update(0, &hz);
        c = ogg_crc_update(c, &buf[i + PAGE_HDR_LEN..end]);
        if c != stored {
            i += 1;
            continue;
        }
        let ser = u32::from_le_bytes([buf[i + 14], buf[i + 15], buf[i + 16], buf[i + 17]]);
        let g = u64::from_le_bytes([
            buf[i + 6], buf[i + 7], buf[i + 8], buf[i + 9], buf[i + 10], buf[i + 11], buf[i + 12], buf[i + 13],
        ]);
        if ser == serial && g != NO_GRANULE {
            best = Some(g);
        }
        i = end;
    }
    best
}

// ---------------------------------------------------------------------------
// Window remux
// ---------------------------------------------------------------------------

/// Result of [`remux_window`].
#[derive(Debug, Clone, Copy)]
pub struct Window {
    /// Sample index (in the original stream) of the temp file's first sample.
    /// The caller's cut start must be shifted down by this.
    pub first_sample: u64,
    /// Number of samples in the temp file.
    pub samples: u64,
    /// Number of FLAC frames copied.
    pub frames: u64,
}

/// Find a page boundary at or before sample `start` where a fresh (not
/// continued) packet begins. Returns `(file_offset, samples_before)`.
fn find_boundary(
    f: &mut File,
    file_size: u64,
    hdr: &OggFlacHeader,
    start: u64,
) -> Result<(u64, u64), String> {
    let mut target = start;
    for _ in 0..64 {
        // Binary search for the last page whose granule is <= target.
        let (mut lo_off, mut lo_g) = (hdr.audio_offset, 0u64);
        let mut hi_off = file_size;
        while hi_off.saturating_sub(lo_off) > (512 << 10) {
            let mid = lo_off + (hi_off - lo_off) / 2;
            let p = match find_page_at_or_after(f, mid, hi_off, hdr.serial)? {
                Some(p) => p,
                None => {
                    hi_off = mid;
                    continue;
                }
            };
            // Walk forward past pages with no granule to get a usable one.
            let first_off = p.offset;
            let mut q = p;
            let mut guard = 0;
            while q.granule == NO_GRANULE {
                guard += 1;
                if guard > 4096 {
                    break;
                }
                match read_page_at(f, q.end()) {
                    Ok(n) if n.serial == hdr.serial => q = n,
                    Ok(n) => {
                        q = Page { granule: NO_GRANULE, ..n };
                    }
                    Err(_) => break,
                }
            }
            if q.granule != NO_GRANULE && q.granule <= target {
                lo_off = q.end();
                lo_g = q.granule;
            } else {
                hi_off = first_off;
            }
        }

        // Linear walk from `lo`, remembering the latest page start that is a
        // fresh-packet boundary with samples_before <= target.
        let mut off = lo_off;
        let mut prev_g = lo_g;
        let mut cand: Option<(u64, u64)> = None;
        loop {
            let page = match read_page_at(f, off) {
                Ok(p) => p,
                Err(PageErr::Eof) | Err(PageErr::Truncated) => break,
                Err(PageErr::Bad(m)) => return Err(m),
            };
            if page.serial != hdr.serial {
                off = page.end();
                continue;
            }
            if !page.continued() && prev_g <= target {
                cand = Some((page.offset, prev_g));
            }
            if page.granule != NO_GRANULE {
                if page.granule > target {
                    break;
                }
                prev_g = page.granule;
            }
            off = page.end();
        }
        if let Some(c) = cand {
            return Ok(c);
        }
        // No usable boundary (the page holding `start` continues a packet
        // and lo itself was mid-packet): retry from earlier in the file.
        if lo_off <= hdr.audio_offset {
            break;
        }
        target = lo_g.saturating_sub(1);
    }
    Ok((hdr.audio_offset, 0))
}

fn push_utf8_coded(out: &mut Vec<u8>, n: u64) {
    if n < 0x80 {
        out.push(n as u8);
        return;
    }
    let bits = 64 - n.leading_zeros();
    let size: usize = if bits <= 11 {
        2
    } else if bits <= 16 {
        3
    } else if bits <= 21 {
        4
    } else if bits <= 26 {
        5
    } else if bits <= 31 {
        6
    } else {
        7
    };
    let first_mask: u8 = match size {
        2 => 0xC0,
        3 => 0xE0,
        4 => 0xF0,
        5 => 0xF8,
        6 => 0xFC,
        _ => 0xFE,
    };
    // The size class above guarantees the leading data bits fit the first byte
    // (for the 7-byte form n < 2^36, so n >> 36 == 0).
    out.push(first_mask | (n >> (6 * (size - 1))) as u8);
    for k in (0..size - 1).rev() {
        out.push(0x80 | ((n >> (6 * k)) & 0x3F) as u8);
    }
}

/// Rewrite the frame/sample number of a FLAC frame (one Ogg packet) to
/// `number - first_number`, recomputing the header CRC-8 and frame CRC-16.
fn renumber_frame(
    pkt: &[u8],
    hlen: usize,
    number: u64,
    first_number: u64,
    out: &mut Vec<u8>,
) -> Result<(), String> {
    if pkt.len() < hlen + 2 {
        return Err("FLAC frame packet is too short".into());
    }
    let crc_at = pkt.len() - 2;
    let stored = u16::from_be_bytes([pkt[crc_at], pkt[crc_at + 1]]);
    if crc16(&pkt[..crc_at]) != stored {
        return Err("FLAC frame CRC-16 mismatch inside an Ogg packet (corrupt capture)".into());
    }
    let (_, old_utf8) = read_utf8_coded(&pkt[4..]).ok_or("bad frame number encoding")?;
    out.clear();
    out.extend_from_slice(&pkt[..4]);
    push_utf8_coded(out, number - first_number);
    out.extend_from_slice(&pkt[4 + old_utf8..hlen - 1]);
    let h = crc8(out);
    out.push(h);
    out.extend_from_slice(&pkt[hlen..crc_at]);
    let c = crc16(out);
    out.extend_from_slice(&c.to_be_bytes());
    Ok(())
}

/// Extract the pages that cover samples `[start, start + len)` of the Ogg FLAC
/// file `src` into a standalone native FLAC at `dst`.
///
/// The window starts at a page boundary at or before `start` (see
/// [`Window::first_sample`]) and ends at the first page whose granule reaches
/// `start + len` (or at the end of the stream). The temp file carries the
/// source's STREAMINFO (total patched to the window length, MD5 cleared — the
/// spec's "unknown") and Vorbis comments, then the frames with numbering
/// rebased to 0.
pub fn remux_window(
    src: &Path,
    dst: &Path,
    start: u64,
    len: u64,
    cancelled: &dyn Fn() -> bool,
) -> Result<Window, String> {
    let hdr = read_header(src)?;
    let mut f = File::open(src).map_err(|e| format!("open failed: {e}"))?;
    let file_size = f.metadata().map_err(|e| e.to_string())?.len();
    let known_total = last_granule(src, &hdr);
    if let Some(t) = known_total {
        if start >= t {
            return Err(format!("cut start {start} is at or past the end of the stream ({t} samples)"));
        }
    }
    let (b_off, g_before) = match known_total {
        Some(_) => find_boundary(&mut f, file_size, &hdr, start)?,
        None => (hdr.audio_offset, 0),
    };
    // A zero-length request still needs the page holding `start`.
    let end_target = start.saturating_add(len.max(1));

    // --- header: fLaC + STREAMINFO (+ VORBIS_COMMENT) ---
    // Read+write: the STREAMINFO total is patched in place once the window
    // length is known.
    let out_file = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(true)
        .open(dst)
        .map_err(|e| format!("create {}: {e}", dst.display()))?;
    let mut w = BufWriter::with_capacity(1 << 20, out_file);
    // STREAMINFO body layout: min/max block (4) + min/max frame (6), then the
    // packed rate/ch/bps/total word at body[10..18], then the 16-byte MD5.
    let mut si = hdr.streaminfo().to_vec();
    for b in &mut si[18..34] {
        *b = 0; // MD5 = "not computed" (the spec's unknown value)
    }
    // Zero the 36-bit total for now; it is patched once the window is known.
    let mut packed = u64::from_be_bytes(si[10..18].try_into().unwrap());
    packed &= !((1u64 << 36) - 1);
    si[10..18].copy_from_slice(&packed.to_be_bytes());
    let vc = hdr.vorbis_comment().map(|p| p.to_vec());
    let mut head = b"fLaC".to_vec();
    push_block(&mut head, 0, &si, vc.is_none());
    if let Some(v) = &vc {
        push_block(&mut head, 4, v, true);
    }
    w.write_all(&head).map_err(|e| format!("write failed: {e}"))?;

    // --- frames ---
    f.seek(SeekFrom::Start(b_off)).map_err(|e| e.to_string())?;
    let mut r = BufReader::with_capacity(4 << 20, f);
    let mut off = b_off;
    let mut carry: Vec<u8> = Vec::new();
    let mut scratch: Vec<u8> = Vec::new();
    let mut first_number: Option<u64> = None;
    let mut frames = 0u64;
    let mut frame_samples = 0u64;
    let mut last_g: Option<u64> = None;
    let mut first_page = true;
    loop {
        if cancelled() {
            return Err("cancelled by user".into());
        }
        let page = match read_page(&mut r, off) {
            Ok(p) => p,
            Err(PageErr::Eof) | Err(PageErr::Truncated) => break,
            Err(PageErr::Bad(m)) => return Err(m),
        };
        off = page.end();
        if page.serial != hdr.serial {
            continue;
        }
        if first_page {
            if page.continued() {
                return Err("internal error: window starts on a continued Ogg page".into());
            }
            first_page = false;
        }
        let mut pk: Vec<Vec<u8>> = Vec::new();
        split_packets(&page, &mut carry, &mut pk);
        for p in &pk {
            let (bs, hlen, number, _variable) = parse_frame_header(p).ok_or_else(|| {
                format!(
                    "Ogg packet on the page at offset {} is not a valid FLAC frame (bad sync / header CRC-8)",
                    page.offset
                )
            })?;
            let fnum = *first_number.get_or_insert(number);
            if fnum == 0 {
                w.write_all(p).map_err(|e| format!("write failed: {e}"))?;
            } else {
                renumber_frame(p, hlen, number, fnum, &mut scratch)?;
                w.write_all(&scratch).map_err(|e| format!("write failed: {e}"))?;
            }
            frames += 1;
            frame_samples += u64::from(bs);
        }
        if page.granule != NO_GRANULE {
            last_g = Some(page.granule);
            if page.granule >= end_target {
                break;
            }
        }
    }

    let last_g = last_g.ok_or("no Ogg page with a granule position in the requested window")?;
    if frames == 0 {
        return Err("no audio frames in the requested window".into());
    }
    let samples = last_g
        .checked_sub(g_before)
        .ok_or("Ogg granule positions went backwards (corrupt stream)")?;
    if samples != frame_samples {
        return Err(format!(
            "Ogg granule span ({samples}) does not match the copied FLAC frames ({frame_samples} samples) — unsupported or corrupt stream"
        ));
    }

    // Patch STREAMINFO total (36-bit field; 0 = unknown when it cannot fit).
    let mut out_file = w.into_inner().map_err(|e| format!("flush failed: {}", e.error()))?;
    let total_field = if samples < (1u64 << 36) { samples } else { 0 };
    out_file.seek(SeekFrom::Start(18)).map_err(|e| e.to_string())?;
    let mut pb = [0u8; 8];
    out_file.read_exact(&mut pb).map_err(|e| e.to_string())?;
    let packed = (u64::from_be_bytes(pb) & !((1u64 << 36) - 1)) | total_field;
    out_file.seek(SeekFrom::Start(18)).map_err(|e| e.to_string())?;
    out_file.write_all(&packed.to_be_bytes()).map_err(|e| e.to_string())?;
    out_file.flush().map_err(|e| e.to_string())?;

    if g_before > start || g_before + samples <= start {
        return Err(format!(
            "internal error: window [{g_before}, {}) does not contain the requested start {start}",
            g_before + samples
        ));
    }
    Ok(Window { first_sample: g_before, samples, frames })
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
pub(crate) mod testutil {
    //! Synthetic Ogg FLAC builder: wraps a native FLAC (from the in-tree
    //! enc12 encoder) into Ogg pages, with real lacing, continued packets and
    //! granule-less pages, so the demuxer is exercised against the same
    //! structures libogg/ffmpeg produce.
    use super::*;

    pub fn lcg_samples(n: usize, seed: u64) -> Vec<i16> {
        let mut s = seed;
        (0..n)
            .map(|_| {
                s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                (((s >> 33) as i32 % 4096) - 2048) as i16
            })
            .collect()
    }

    /// Build a native 12-bit mono FLAC (verbatim frames, block 4096).
    pub fn native_flac(samples: &[i16], rate: u32, name: &str) -> Vec<u8> {
        let p = std::env::temp_dir().join(name);
        crate::enc12::encode_12bit_mono(
            samples.iter().map(|&v| Ok(v)),
            rate,
            samples.len() as u64,
            &p,
        )
        .unwrap();
        let b = std::fs::read(&p).unwrap();
        let _ = std::fs::remove_file(&p);
        b
    }

    /// Split a native FLAC into (streaminfo payload, frames).
    pub fn split_native(native: &[u8]) -> (Vec<u8>, Vec<Vec<u8>>) {
        assert_eq!(&native[..4], b"fLaC");
        let si = native[8..8 + 34].to_vec();
        let audio = 4 + 4 + 34;
        let mut starts = vec![audio];
        let mut num = 0u64;
        let mut pos = audio;
        loop {
            let (_, hlen, n, _) = parse_frame_header(&native[pos..]).unwrap();
            assert_eq!(n, num);
            let mut q = pos + hlen;
            let mut found = None;
            while q + 17 <= native.len() {
                if native[q] == 0xFF {
                    if let Some((_, _, n2, _)) = parse_frame_header(&native[q..]) {
                        if n2 == num + 1 {
                            found = Some(q);
                            break;
                        }
                    }
                }
                q += 1;
            }
            match found {
                Some(q) => {
                    starts.push(q);
                    pos = q;
                    num += 1;
                }
                None => break,
            }
        }
        let mut frames = Vec::new();
        for (i, &s) in starts.iter().enumerate() {
            let e = starts.get(i + 1).copied().unwrap_or(native.len());
            frames.push(native[s..e].to_vec());
        }
        (si, frames)
    }

    fn write_page(out: &mut Vec<u8>, ht: u8, granule: u64, serial: u32, seq: u32, segs: &[u8], payload: &[u8]) {
        let mut pg = Vec::new();
        pg.extend_from_slice(b"OggS");
        pg.push(0);
        pg.push(ht);
        pg.extend_from_slice(&granule.to_le_bytes());
        pg.extend_from_slice(&serial.to_le_bytes());
        pg.extend_from_slice(&seq.to_le_bytes());
        pg.extend_from_slice(&[0, 0, 0, 0]);
        pg.push(segs.len() as u8);
        pg.extend_from_slice(segs);
        pg.extend_from_slice(payload);
        let c = ogg_crc_update(0, &pg);
        pg[22..26].copy_from_slice(&c.to_le_bytes());
        out.extend_from_slice(&pg);
    }

    /// Wrap into Ogg FLAC. `max_segs` bounds the lacing segments per page
    /// (small values force packets to span pages). `comments` adds a
    /// VORBIS_COMMENT header packet. `zero_total` clears the STREAMINFO
    /// total like a piped ffmpeg capture.
    pub fn wrap_ogg(
        si: &[u8],
        frames: &[Vec<u8>],
        block: u64,
        total: u64,
        max_segs: usize,
        comments: Option<&[u8]>,
        zero_total: bool,
    ) -> Vec<u8> {
        let serial = 0x1234_5678u32;
        let mut out = Vec::new();
        let mut seq = 0u32;
        let mut si = si.to_vec();
        if zero_total {
            let mut packed = u64::from_be_bytes(si[10..18].try_into().unwrap());
            packed &= !((1u64 << 36) - 1);
            si[10..18].copy_from_slice(&packed.to_be_bytes());
        }
        // Mapping header page (BOS).
        let mut map = vec![0x7f, b'F', b'L', b'A', b'C', 1, 0, 0, if comments.is_some() { 1 } else { 0 }];
        map.extend_from_slice(b"fLaC");
        map.push(if comments.is_some() { 0x00 } else { 0x80 });
        map.extend_from_slice(&[0, 0, 34]);
        map.extend_from_slice(&si);
        let segs = lace(map.len());
        write_page(&mut out, 0x02, 0, serial, seq, &segs, &map);
        seq += 1;
        if let Some(vc) = comments {
            let mut pk = vec![0x84, (vc.len() >> 16) as u8, (vc.len() >> 8) as u8, vc.len() as u8];
            pk.extend_from_slice(vc);
            let segs = lace(pk.len());
            write_page(&mut out, 0x00, 0, serial, seq, &segs, &pk);
            seq += 1;
        }
        // Audio: lacing across pages with at most `max_segs` segments each.
        let mut segs: Vec<u8> = Vec::new();
        let mut payload: Vec<u8> = Vec::new();
        let mut completed: Option<u64> = None;
        let mut continued = false;
        let mut cum = 0u64;
        let n = frames.len();
        let flush = |out: &mut Vec<u8>,
                     segs: &mut Vec<u8>,
                     payload: &mut Vec<u8>,
                     completed: &mut Option<u64>,
                     continued: &mut bool,
                     seq: &mut u32,
                     eos: bool| {
            let ht = (if *continued { 1 } else { 0 }) | (if eos { 4 } else { 0 });
            write_page(out, ht, completed.unwrap_or(NO_GRANULE), serial, *seq, segs, payload);
            *seq += 1;
            *continued = segs.last().map_or(false, |&s| s == 255);
            segs.clear();
            payload.clear();
            *completed = None;
        };
        for (i, fr) in frames.iter().enumerate() {
            cum = if i + 1 == n { total } else { cum + block };
            let lacing = lace(fr.len());
            let mut pos = 0usize;
            for &s in &lacing {
                if segs.len() == max_segs {
                    flush(&mut out, &mut segs, &mut payload, &mut completed, &mut continued, &mut seq, false);
                }
                segs.push(s);
                payload.extend_from_slice(&fr[pos..pos + s as usize]);
                pos += s as usize;
                if s < 255 {
                    completed = Some(cum);
                }
            }
        }
        flush(&mut out, &mut segs, &mut payload, &mut completed, &mut continued, &mut seq, true);
        out
    }

    fn lace(len: usize) -> Vec<u8> {
        let mut v = vec![255u8; len / 255];
        v.push((len % 255) as u8);
        v
    }

    /// Write a complete synthetic `.ldf` (Ogg FLAC, 12-bit mono, block 4096)
    /// of `n` pseudo-random samples to the temp dir; returns the path and the
    /// exact samples so tests can compare decoded output sample-for-sample.
    pub fn make_ogg_ldf(
        name: &str,
        n: usize,
        rate: u32,
        max_segs: usize,
        zero_total: bool,
        with_comments: bool,
    ) -> (std::path::PathBuf, Vec<i16>) {
        let samples = lcg_samples(n, 42);
        let native = native_flac(&samples, rate, &format!("{name}.native.flac"));
        let (si, frames) = split_native(&native);
        let vc = {
            // VORBIS_COMMENT payload: vendor "t", 2 comments.
            let mut v = Vec::new();
            v.extend_from_slice(&1u32.to_le_bytes());
            v.extend_from_slice(b"t");
            v.extend_from_slice(&2u32.to_le_bytes());
            for c in ["PROJECT=ldf", "NOTES=a=b"] {
                v.extend_from_slice(&(c.len() as u32).to_le_bytes());
                v.extend_from_slice(c.as_bytes());
            }
            v
        };
        let ogg = wrap_ogg(
            &si,
            &frames,
            4096,
            n as u64,
            max_segs,
            if with_comments { Some(&vc) } else { None },
            zero_total,
        );
        let p = std::env::temp_dir().join(format!("{name}.ldf"));
        std::fs::write(&p, ogg).unwrap();
        (p, samples)
    }
}

#[cfg(test)]
mod tests {
    use super::testutil::*;
    use super::*;
    use claxon::FlacReader;

    // 292 full 4096-sample blocks + a 3_968-sample tail; ~1.8 MB of Ogg, so the
    // granule binary search (which only runs above 512 KiB) is exercised.
    const N: usize = 1_200_000;
    const RATE: u32 = 20_000;

    fn build(name: &str, max_segs: usize, zero_total: bool, comments: bool) -> (std::path::PathBuf, Vec<i16>) {
        make_ogg_ldf(name, N, RATE, max_segs, zero_total, comments)
    }

    fn decode(path: &Path) -> Vec<i32> {
        let mut r = FlacReader::open(path).unwrap();
        r.samples().map(|s| s.unwrap()).collect()
    }

    #[test]
    fn sniff_classifies_ogg_payloads() {
        let (p, _) = build("fc_ogg_sniff", 255, false, false);
        let b = std::fs::read(&p).unwrap();
        assert_eq!(sniff_ogg(&b[..64]), OggKind::Flac);
        assert_eq!(sniff_ogg(b"fLaC....."), OggKind::NotOgg);
        let mut v = b[..27].to_vec();
        v[26] = 1;
        v.push(30);
        v.extend_from_slice(b"\x01vorbis\0\0\0");
        assert_eq!(sniff_ogg(&v), OggKind::Other("Vorbis"));
        let mut o = b[..27].to_vec();
        o[26] = 1;
        o.push(19);
        o.extend_from_slice(b"OpusHead\x01\x02");
        assert_eq!(sniff_ogg(&o), OggKind::Other("Opus"));
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn header_and_native_view_parse_with_claxon() {
        let (p, _) = build("fc_ogg_hdr", 255, true, true);
        let h = read_header(&p).unwrap();
        assert_eq!(h.blocks[0].0, 0);
        assert!(h.vorbis_comment().is_some());
        assert!(h.audio_offset > 0);
        let opts = claxon::FlacReaderOptions { metadata_only: true, read_vorbis_comment: true };
        let rd = FlacReader::new_ext(std::io::Cursor::new(h.native_bytes()), opts).unwrap();
        let si = rd.streaminfo();
        assert_eq!(si.sample_rate, RATE);
        assert_eq!(si.bits_per_sample, 12);
        assert_eq!(si.samples, None, "zero_total stream must report unknown total");
        assert_eq!(rd.get_tag("PROJECT").next(), Some("ldf"));
        assert_eq!(rd.get_tag("NOTES").next(), Some("a=b"));
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn last_granule_is_exact_even_with_zero_streaminfo_total() {
        for max_segs in [255usize, 3] {
            let (p, _) = build(&format!("fc_ogg_gran{max_segs}"), max_segs, true, false);
            let h = read_header(&p).unwrap();
            assert_eq!(last_granule(&p, &h), Some(N as u64), "max_segs={max_segs}");
            let _ = std::fs::remove_file(&p);
        }
    }

    #[test]
    fn last_granule_survives_truncated_tail() {
        let (p, _) = build("fc_ogg_trunc", 255, true, false);
        let full = std::fs::read(&p).unwrap();
        // Chop the file mid-way through the last page: the granule of the
        // previous complete page must be returned (and be < the full total).
        std::fs::write(&p, &full[..full.len() - 100]).unwrap();
        let h = read_header(&p).unwrap();
        let g = last_granule(&p, &h).unwrap();
        assert!(g < N as u64 && g > 0, "g={g}");
        assert_eq!(g % 4096, 0);
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn remux_windows_are_sample_exact() {
        // max_segs 3 => every ~6 KB frame spans ~8 pages (continued packets,
        // granule-less pages); 255 => a few frames per page.
        for max_segs in [3usize, 255] {
            let (p, samples) = build(&format!("fc_ogg_win{max_segs}"), max_segs, true, true);
            let dst = std::env::temp_dir().join(format!("fc_ogg_win{max_segs}.win.flac"));
            let n = N as u64;
            for (start, len) in [
                (0u64, 1u64),
                (0, 5000),
                (4095, 2),
                (4096, 4096),
                (12_345, 30_000),
                (600_000, 100),
                (600_000, 300_000),
                (n / 3 + 7, 12_000),
                (n - 5_000, 5_000),
                (n - 1, 1),
                (n - 10_000, 500_000), // len past the end
            ] {
                let w = remux_window(&p, &dst, start, len, &|| false).unwrap();
                assert!(w.first_sample <= start, "start {start}: window starts at {}", w.first_sample);
                assert!(w.first_sample + w.samples > start);
                assert_eq!(w.first_sample % 4096, 0);
                let got = decode(&dst);
                assert_eq!(got.len() as u64, w.samples);
                let end = (start + len).min(N as u64);
                assert!(w.first_sample + w.samples >= end, "window must reach the requested end");
                let lo = (start - w.first_sample) as usize;
                let hi = (end - w.first_sample) as usize;
                let want: Vec<i32> = samples[start as usize..end as usize].iter().map(|&v| v as i32).collect();
                assert_eq!(&got[lo..hi], &want[..], "start {start} len {len} max_segs {max_segs}");
                // Standalone-valid: STREAMINFO total == window length.
                let rd = FlacReader::open(&dst).unwrap();
                assert_eq!(rd.streaminfo().samples, Some(w.samples));
                // Tags are carried through to the window file.
                assert_eq!(rd.get_tag("PROJECT").next(), Some("ldf"));
            }
            let _ = std::fs::remove_file(&p);
            let _ = std::fs::remove_file(&dst);
        }
    }

    #[test]
    fn remux_rebases_frame_numbers_to_zero() {
        let (p, _) = build("fc_ogg_rebase", 255, true, false);
        let dst = std::env::temp_dir().join("fc_ogg_rebase.win.flac");
        // Mid-file so the binary search moves the resume point off the first
        // page (the file is above the 512 KiB bracket threshold).
        let w = remux_window(&p, &dst, 900_000, 10, &|| false).unwrap();
        assert!(w.first_sample > 0, "window must start mid-stream");
        let b = std::fs::read(&dst).unwrap();
        let audio = 4 + 4 + 34;
        let (_, _, n0, _) = parse_frame_header(&b[audio..]).unwrap();
        assert_eq!(n0, 0, "first frame of the window must be numbered 0");
        let _ = std::fs::remove_file(&p);
        let _ = std::fs::remove_file(&dst);
    }

    #[test]
    fn corrupt_page_is_a_hard_error() {
        let (p, _) = build("fc_ogg_corrupt", 255, true, false);
        let h = read_header(&p).unwrap();
        let mut b = std::fs::read(&p).unwrap();
        // Flip one payload byte of the FIRST audio page (deterministic spot
        // inside the frame data, never in a header field).
        let nseg = b[h.audio_offset as usize + 26] as usize;
        let at = h.audio_offset as usize + PAGE_HDR_LEN + nseg + 10;
        b[at] ^= 0xFF;
        std::fs::write(&p, &b).unwrap();
        let dst = std::env::temp_dir().join("fc_ogg_corrupt.win.flac");
        // The window reads that page, so the page CRC must reject it instead
        // of silently producing bad audio.
        let err = remux_window(&p, &dst, 0, u64::MAX / 2, &|| false).unwrap_err();
        assert!(err.contains("CRC"), "got: {err}");
        let _ = std::fs::remove_file(&p);
        let _ = std::fs::remove_file(&dst);
    }

    #[test]
    fn start_past_end_is_rejected() {
        let (p, _) = build("fc_ogg_pastend", 255, true, false);
        let dst = std::env::temp_dir().join("fc_ogg_pastend.win.flac");
        let err = remux_window(&p, &dst, N as u64, 10, &|| false).unwrap_err();
        assert!(err.contains("past the end"), "got: {err}");
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn cancel_aborts_the_remux() {
        let (p, _) = build("fc_ogg_cancel", 255, true, false);
        let dst = std::env::temp_dir().join("fc_ogg_cancel.win.flac");
        let err = remux_window(&p, &dst, 0, N as u64, &|| true).unwrap_err();
        assert!(err.contains("cancelled"), "got: {err}");
        let _ = std::fs::remove_file(&p);
        let _ = std::fs::remove_file(&dst);
    }

    #[test]
    fn utf8_frame_number_roundtrips_through_probe_reader() {
        for n in [0u64, 1, 127, 128, 2047, 2048, 65535, 65536, 0x1F_FFFF, 0x20_0000, 0x3FF_FFFF, 0x400_0000, 0x7FFF_FFFF, 0x8000_0000, 0xF_FFFF_FFFF] {
            let mut v = Vec::new();
            push_utf8_coded(&mut v, n);
            let (back, used) = read_utf8_coded(&v).unwrap();
            assert_eq!((back, used), (n, v.len()), "n={n:#x}");
        }
    }

    #[test]
    fn crc_tables_match_reference_frame_crcs() {
        // The enc12 encoder's CRC-8/CRC-16 are independently implemented;
        // every frame it wrote must verify under this module's tables.
        let samples = lcg_samples(9000, 7);
        let native = native_flac(&samples, RATE, "fc_ogg_crcref.flac");
        let (_, frames) = split_native(&native);
        for fr in &frames {
            let (_, hlen, _, _) = parse_frame_header(fr).unwrap();
            assert_eq!(crc8(&fr[..hlen - 1]), fr[hlen - 1]);
            let at = fr.len() - 2;
            assert_eq!(crc16(&fr[..at]), u16::from_be_bytes([fr[at], fr[at + 1]]));
        }
    }
}
