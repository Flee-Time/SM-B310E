"""Read validation FAT32 images independently of the firmware's FAT code."""
from pathlib import Path
import struct

class Fat32:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        self.start = struct.unpack_from('<I', self.data, 454)[0]*512
        b = self.data[self.start:self.start+512]
        self.sector = struct.unpack_from('<H', b, 11)[0]
        self.spc = b[13]
        reserved = struct.unpack_from('<H', b, 14)[0]
        self.fat_sectors = struct.unpack_from('<I', b, 36)[0]
        self.fat = self.start + reserved*self.sector
        self.base = self.fat + b[16]*self.fat_sectors*self.sector
        self.root = struct.unpack_from('<I', b, 44)[0]
        assert self.sector == 512 and self.spc and self.root >= 2

    def chain(self, cluster):
        seen = set()
        while 2 <= cluster < 0xffffff8:
            assert cluster not in seen and cluster*4 < self.fat_sectors*self.sector
            seen.add(cluster)
            offset = self.base + (cluster-2)*self.sector*self.spc
            assert offset+self.sector*self.spc <= len(self.data)
            yield self.data[offset:offset+self.sector*self.spc]
            cluster = struct.unpack_from('<I', self.data, self.fat+cluster*4)[0] & 0xfffffff

    def entries(self, cluster):
        parts = {}
        for block in self.chain(cluster):
            for offset in range(0,len(block),32):
                e = block[offset:offset+32]
                if not e[0]: return
                if e[0] == 0xe5: parts.clear(); continue
                if e[11] == 15:
                    if e[0] & 64: parts.clear()
                    parts[e[0]&31] = e[1:11]+e[14:26]+e[28:32]
                    continue
                if e[11]&8: parts.clear(); continue
                name = e[:8].decode('ascii').rstrip()
                ext = e[8:11].decode('ascii').rstrip()
                if ext: name += '.'+ext
                if parts:
                    name = b''.join(parts[k] for k in sorted(parts)).decode('utf-16le').split('\0')[0].rstrip('\uffff')
                parts.clear()
                cluster2 = struct.unpack_from('<H',e,20)[0]<<16 | struct.unpack_from('<H',e,26)[0]
                yield name,cluster2,struct.unpack_from('<I',e,28)[0],e[11]

    def find(self, path):
        cluster = self.root
        for name in path.strip('/').split('/'):
            result = next(e for e in self.entries(cluster) if e[0].casefold()==name.casefold())
            cluster = result[1]
        return result

    def read(self, path):
        _,cluster,size,attr = self.find(path)
        assert not attr&16
        return b''.join(self.chain(cluster))[:size]

if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('image'); p.add_argument('file')
    args = p.parse_args()
    print(Fat32(args.image).read(args.file).decode('utf-8'))
