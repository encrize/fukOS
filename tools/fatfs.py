import struct

SECTOR = 512

PART_LBA      = 2048
PART_SECTORS  = 262144
SPC           = 8
RESERVED      = 1
NUM_FATS      = 2
ROOT_ENTRIES  = 512
BPS           = 512

def _ceil_div(a, b):
    return (a + b - 1) // b

def _short_checksum(short11):
    s = 0
    for c in short11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s

def _is_plain_short(name):
    """True if `name` is representable as an all-UPPERCASE 8.3 name with no LFN."""
    if name in (".", ".."):
        return True
    if name != name.upper():
        return False
    base, dot, ext = name.partition(".")
    if dot and "." in ext:
        return False
    if len(base) == 0 or len(base) > 8 or len(ext) > 3:
        return False
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'()@^{}")
    return all(c in ok for c in base + ext)

def _make_short(name, used):
    """Build an 11-byte space-padded 8.3 short name, unique within `used`."""
    up = name.upper()
    base, dot, ext = up.partition(".")
    if "." in ext:
        parts = up.split(".")
        base = "".join(parts[:-1])
        ext = parts[-1]
    keep = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'()@^{}")
    base = "".join(c if c in keep else "_" for c in base).lstrip("_") or "_"
    ext = "".join(c if c in keep else "_" for c in ext)[:3]
    n = 1
    while True:
        tail = "~" + str(n)
        b = (base[: (8 - len(tail))] + tail)[:8]
        cand = b.ljust(8) + ext.ljust(3)
        if cand not in used:
            used.add(cand)
            return cand.encode("ascii")
        n += 1

def _plain_short_bytes(name, used):
    base, dot, ext = name.upper().partition(".")
    cand = base.ljust(8) + ext.ljust(3)
    used.add(cand)
    return cand.encode("ascii")

def _lfn_entries(name, checksum):
    """Return the LFN directory entries (bytes) for `name`, in physical order."""
    u = name.encode("utf-16-le")
    chars = [u[i:i + 2] for i in range(0, len(u), 2)]
    chars.append(b"\x00\x00")
    while len(chars) % 13 != 0:
        chars.append(b"\xff\xff")
    nchunks = len(chars) // 13
    out = []
    for i in range(nchunks):
        chunk = chars[i * 13:(i + 1) * 13]
        seq = i + 1
        if i == nchunks - 1:
            seq |= 0x40
        e = bytearray(32)
        e[0] = seq
        e[11] = 0x0F
        e[12] = 0
        e[13] = checksum
        e[26:28] = b"\x00\x00"
        pos = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
        for j in range(13):
            e[pos[j]:pos[j] + 2] = chunk[j]
        out.append(bytes(e))
    out.reverse()
    return out

class _Node:
    def __init__(self, name, is_dir):
        self.name = name
        self.is_dir = is_dir
        self.content = b""
        self.children = []
        self.first_cluster = 0

class FatFS:
    def __init__(self):
        self.root = _Node("", True)
        self.root_dir_sectors = _ceil_div(ROOT_ENTRIES * 32, BPS)
        tmp1 = PART_SECTORS - (RESERVED + self.root_dir_sectors)
        tmp2 = (256 * SPC) + NUM_FATS
        self.fat_size = _ceil_div(tmp1, tmp2)
        self.data_start = RESERVED + NUM_FATS * self.fat_size + self.root_dir_sectors
        self.total_clusters = (PART_SECTORS - self.data_start) // SPC
        self.cluster_bytes = SPC * BPS
        self.next_free = 2
        self.fat = [0] * (self.total_clusters + 2)

    def _find_dir(self, parts):
        node = self.root
        for p in parts:
            nxt = None
            for c in node.children:
                if c.is_dir and c.name.lower() == p.lower():
                    nxt = c
                    break
            if nxt is None:
                nxt = _Node(p, True)
                node.children.append(nxt)
            node = nxt
        return node

    def add_dir(self, path):
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        self._find_dir(parts)

    def add_file(self, path, content):
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        d = self._find_dir(parts[:-1])
        f = _Node(parts[-1], False)
        f.content = content
        d.children.append(f)

    def _alloc(self, nbytes):
        nclu = max(1, _ceil_div(nbytes, self.cluster_bytes))
        first = self.next_free
        for i in range(nclu):
            cl = self.next_free
            self.next_free += 1
            if self.next_free - 2 > self.total_clusters:
                raise RuntimeError("FAT16 partition full")
            self.fat[cl] = 0xFFFF if i == nclu - 1 else cl + 1
        return first, nclu

    def _dir_entry_bytes(self, node, used):
        """Return the directory-entry byte block for one child node."""
        name = node.name
        if _is_plain_short(name):
            short = _plain_short_bytes(name, used)
            lfns = b""
        else:
            short = _make_short(name, used)
            lfns = b"".join(_lfn_entries(name, _short_checksum(short)))
        e = bytearray(32)
        e[0:11] = short
        e[11] = 0x10 if node.is_dir else 0x20
        fc = node.first_cluster
        e[20:22] = struct.pack("<H", (fc >> 16) & 0xFFFF)
        e[26:28] = struct.pack("<H", fc & 0xFFFF)
        e[28:32] = struct.pack("<I", 0 if node.is_dir else len(node.content))
        e[22:24] = struct.pack("<H", 0)
        e[24:26] = struct.pack("<H", 0x21)
        return lfns + bytes(e)

    def _dot_entry(self, name, cluster):
        e = bytearray(32)
        e[0:11] = name.ljust(11).encode("ascii")
        e[11] = 0x10
        e[26:28] = struct.pack("<H", cluster & 0xFFFF)
        e[20:22] = struct.pack("<H", (cluster >> 16) & 0xFFFF)
        e[24:26] = struct.pack("<H", 0x21)
        return bytes(e)

    def _serialize_dir(self, node, parent_cluster, cluster_store):
        """Recursively lay out a directory's contents into clusters.
        Returns the directory's raw entry bytes (WITHOUT reserving a cluster
        for the root, which lives in the fixed root region)."""
        used = set()
        body = b""
        if node is not self.root:
            body += self._dot_entry(".", node.first_cluster)
            body += self._dot_entry("..", parent_cluster)
        for c in node.children:
            if c.is_dir:
                c.first_cluster, _ = self._alloc(self.cluster_bytes)
            else:
                if len(c.content) == 0:
                    c.first_cluster = 0
                else:
                    c.first_cluster, _ = self._alloc(len(c.content))
        for c in node.children:
            body += self._dir_entry_bytes(c, used)
        for c in node.children:
            if c.is_dir:
                sub = self._serialize_dir(c, node.first_cluster, cluster_store)
                cluster_store[c.first_cluster] = sub
            elif c.content:
                cluster_store[c.first_cluster] = c.content
        return body

    def serialize(self):
        cluster_store = {}
        root_body = self._serialize_dir(self.root, 0, cluster_store)
        if len(root_body) > self.root_dir_sectors * BPS:
            raise RuntimeError("too many root entries")

        img = bytearray(PART_SECTORS * BPS)

        bs = img
        bs[0:3] = b"\xeb\x3c\x90"
        bs[3:11] = b"MYOS    "
        struct.pack_into("<H", bs, 11, BPS)
        bs[13] = SPC
        struct.pack_into("<H", bs, 14, RESERVED)
        bs[16] = NUM_FATS
        struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
        struct.pack_into("<H", bs, 19, PART_SECTORS if PART_SECTORS < 0x10000 else 0)
        bs[21] = 0xF8
        struct.pack_into("<H", bs, 22, self.fat_size)
        struct.pack_into("<H", bs, 24, 63)
        struct.pack_into("<H", bs, 26, 255)
        struct.pack_into("<I", bs, 28, PART_LBA)
        struct.pack_into("<I", bs, 32, PART_SECTORS if PART_SECTORS >= 0x10000 else 0)
        bs[36] = 0x80
        bs[38] = 0x29
        struct.pack_into("<I", bs, 39, 0x1234ABCD)
        bs[43:54] = b"MYOS DISK  "
        bs[54:62] = b"FAT16   "
        bs[510] = 0x55
        bs[511] = 0xAA

        fat_bytes = bytearray(self.fat_size * BPS)
        self.fat[0] = 0xFFF8
        self.fat[1] = 0xFFFF
        for i, v in enumerate(self.fat):
            if i * 2 + 2 <= len(fat_bytes):
                struct.pack_into("<H", fat_bytes, i * 2, v & 0xFFFF)
        for k in range(NUM_FATS):
            off = (RESERVED + k * self.fat_size) * BPS
            img[off:off + len(fat_bytes)] = fat_bytes

        root_off = (RESERVED + NUM_FATS * self.fat_size) * BPS
        img[root_off:root_off + len(root_body)] = root_body

        for cl, data in cluster_store.items():
            off = (self.data_start + (cl - 2) * SPC) * BPS
            img[off:off + len(data)] = data

        return bytes(img)

def read_fs(img):
    """Parse a FAT16 image and return a dict path >> bytes (files only)."""
    bps = struct.unpack_from("<H", img, 11)[0]
    spc = img[13]
    reserved = struct.unpack_from("<H", img, 14)[0]
    num_fats = img[16]
    root_entries = struct.unpack_from("<H", img, 17)[0]
    fat_size = struct.unpack_from("<H", img, 22)[0]
    root_dir_sectors = _ceil_div(root_entries * 32, bps)
    fat_off = reserved * bps
    root_off = (reserved + num_fats * fat_size) * bps
    data_start = reserved + num_fats * fat_size + root_dir_sectors

    def next_cluster(cl):
        return struct.unpack_from("<H", img, fat_off + cl * 2)[0]

    def read_chain(first, size=None):
        out = b""
        cl = first
        while 2 <= cl < 0xFFF8:
            off = (data_start + (cl - 2) * spc) * bps
            out += img[off:off + spc * bps]
            cl = next_cluster(cl)
        return out[:size] if size is not None else out

    result = {}

    def parse_dir(entries_bytes, prefix):
        lfn = ""
        i = 0
        while i + 32 <= len(entries_bytes):
            e = entries_bytes[i:i + 32]
            i += 32
            if e[0] == 0x00:
                break
            if e[0] == 0xE5:
                lfn = ""
                continue
            attr = e[11]
            if attr == 0x0F:
                part = b""
                for p in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
                    part += e[p:p + 2]
                s = part.decode("utf-16-le")
                s = s.split("\x00", 1)[0]
                lfn = s + lfn
                continue
            if attr & 0x08:
                lfn = ""
                continue
            short = e[0:11]
            base = short[0:8].decode("ascii", "replace").rstrip()
            ext = short[8:11].decode("ascii", "replace").rstrip()
            name = lfn if lfn else (base + ("." + ext if ext else ""))
            lfn = ""
            first = struct.unpack_from("<H", e, 26)[0] | (struct.unpack_from("<H", e, 20)[0] << 16)
            size = struct.unpack_from("<I", e, 28)[0]
            if name in (".", ".."):
                continue
            if attr & 0x10:
                sub = read_chain(first)
                parse_dir(sub, prefix + name + "/")
            else:
                result[prefix + name] = read_chain(first, size) if first else b""

    parse_dir(img[root_off:root_off + root_dir_sectors * bps], "")
    return result

if __name__ == "__main__":
    fs = FatFS()
    fs.add_file("HELLO.TXT", b"hi\n")
    fs.add_file("photos/ryo.jpg", b"x" * 5000)
    fs.add_dir("empty")
    data = fs.serialize()
    open("/tmp/fat_demo.img", "wb").write(data)
    print("wrote /tmp/fat_demo.img", len(data), "bytes")
    print("files:", {k: len(v) for k, v in read_fs(data).items()})
