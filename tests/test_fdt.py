#!/usr/bin/env python3
"""Host-only parser tests. These do not establish ANY guest T1-T7 result."""
import ctypes as c
import json, pathlib, random, struct, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
def word(n): return struct.pack('>I',n)
def align(b): return b+b'\0'*((-len(b))%4)
names=bytearray()
def prop(name,data):
    off=len(names); names.extend(name.encode()+b'\0')
    return word(3)+word(len(data))+word(off)+align(data)
def node(name): return word(1)+align(name.encode()+b'\0')
end=word(2)
cells=lambda: prop('#address-cells',word(2))+prop('#size-cells',word(2))
tree=node('')+cells()
tree+=node('chosen')+prop('bootargs',b'protoos.test=boot\0')+end
tree+=node('cpus')+prop('timebase-frequency',word(10000000))+end
tree+=node('soc')+cells()+prop('ranges',b'')
tree+=node('serial@10000000')+prop('compatible',b'ns16550a\0')
tree+=prop('reg',word(0)+word(0x10000000)+word(0)+word(0x100))+end+end+end+word(9)
offset=56; strings=offset+len(tree); total=strings+len(names)
blob=b''.join(map(word,[0xd00dfeed,total,offset,strings,40,17,16,0,len(names),len(tree)]))+bytes(16)+tree+names
class Config(c.Structure):
    _fields_=[('uart',c.c_size_t),('timebase_hz',c.c_uint32),('bootargs',c.c_char*128)]
with tempfile.TemporaryDirectory() as td:
    so=pathlib.Path(td)/'fdt.so'
    cmd=['gcc','-std=c11','-Wall','-Wextra','-Werror','-O2','-shared','-fPIC',str(root/'src/fdt.c'),'-o',str(so)]
    subprocess.run(cmd,check=True)
    lib=c.CDLL(str(so));lib.parse_dtb.argtypes=[c.c_void_p,c.c_size_t,c.POINTER(Config)];lib.parse_dtb.restype=c.c_int
    def parse(data):
        buf=c.create_string_buffer(bytes(data));out=Config()
        return lib.parse_dtb(buf,len(data),c.byref(out)),out
    rc,out=parse(blob)
    assert rc==0 and out.uart==0x10000000 and out.timebase_hz==10000000
    assert out.bootargs==b'protoos.test=boot'
    # Every truncated input must fail, even if the header still declares full size.
    for i in range(len(blob)):assert parse(blob[:i])[0]<0,i
    for off,val in [(0,0),(4,0xffffffff),(8,0xffffffff),(12,0xffffffff),(32,0xffffffff),(36,0xffffffff),(20,16),(24,18)]:
        bad=bytearray(blob);bad[off:off+4]=word(val);assert parse(bad)[0]<0,(off,val)
    missing_end=bytearray(blob);missing_end[strings-4:strings]=word(4)
    assert parse(missing_end)[0]<0
    # Mutation fuzz smoke: reject or parse safely; this is not an MMU test.
    rng=random.Random(18019)
    for _ in range(10000):
        bad=bytearray(blob)
        for _ in range(rng.randrange(1,6)):bad[rng.randrange(len(bad))]=rng.randrange(256)
        parse(bad)
report={'suite':'host_fdt','status':'PASS','valid_fixture':1,'truncations':len(blob),'invalid_headers':8,'missing_end':1,'mutation_smoke_cases':10000,'seed':18019,'scope':'Host C parser only; no RV64 assembly, guest execution, hardware protection or acceptance proof','compiler_command':cmd}
(root/'evidence/host_fdt.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
