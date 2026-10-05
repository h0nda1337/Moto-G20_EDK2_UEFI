"""Bounded DTB validator fixtures; no target or disk mutation."""
import pathlib, struct, subprocess, json, hashlib
ROOT=pathlib.Path(__file__).resolve().parent.parent
OUT=ROOT/'evidence'/'cases'
OUT.mkdir(parents=True,exist_ok=True)
def u32(n): return struct.pack('>I',n)
def u64(n): return struct.pack('>Q',n)
def reg(base,size): return u64(base)+u64(size)
def make(*,memory=(0x80000000,0xfffff000), reserved=(0x94000000,0x6000000),
         memreserve=None, root_cells=2, res_cells=2, compatible=b'sprd,ums512\0',
         memory_status=None,dynamic=False, include_reserved=True, include_memory=True):
    strings=bytearray(); names={}; parts=[]
    def node(n):
        b=n.encode()+b'\0'; parts.append(u32(1)+b+b'\0'*((-len(b))%4))
    def end(): parts.append(u32(2))
    def prop(n,b):
        if n not in names: names[n]=len(strings); strings.extend(n.encode()+b'\0')
        parts.append(u32(3)+u32(len(b))+u32(names[n])+b+b'\0'*((-len(b))%4))
    node('');prop('#address-cells',u32(root_cells));prop('#size-cells',u32(2));prop('compatible',compatible)
    if include_memory:
        node('memory');prop('device_type',b'memory\0');prop('reg',reg(*memory))
        if memory_status: prop('status',memory_status)
        end()
    if include_reserved:
        node('reserved-memory');prop('#address-cells',u32(res_cells));prop('#size-cells',u32(2));prop('ranges',b'')
        node('reserve@94000000');prop('reg',reg(*reserved))
        if dynamic: prop('size',u64(0x1000))
        end();end()
    end();parts.append(u32(9));body=b''.join(parts)
    rsv=reg(*memreserve) if memreserve else b''
    rsv+=b'\0'*16; offset=40+len(rsv)
    return b''.join(u32(x) for x in [0xd00dfeed,offset+len(body)+len(strings),offset,offset+len(body),40,17,16,0,len(strings),len(body)])+rsv+body+strings
cases=[]
def add(name, blob, expected):
    file=OUT/(name+'.dtb');file.write_bytes(blob)
    cases.append((name,expected))
base=make();add('normal',base,0)
add('wrong_board',make(compatible=b'other,soc\0'),6)
add('compatible_without_nul',make(compatible=b'sprd,ums512'),3)
add('bad_cells',make(root_cells=1),4)
add('bad_reserved_cells',make(res_cells=1),4)
add('disabled_memory',make(memory_status=b'disabled\0'),7)
add('missing_memory',make(include_memory=False),6)
add('missing_reserved',make(include_reserved=False),6)
add('low_dram_only',make(memory=(0x80000000,0x80000000)),7)
add('overflow_dram',make(memory=(0x80000000,0xffffffffffffffff)),7)
add('zero_dram',make(memory=(0x80000000,0)),7)
add('overlap_low',make(reserved=(0xb0000000,0x1000)),8)
add('overlap_high',make(reserved=(0x110000000,0x1000)),8)
add('overlap_memreserve',make(memreserve=(0xb0000000,0x1000)),8)
add('adjacent_low_before',make(reserved=(0x9f000000,0x1000000)),0)
add('adjacent_low_after',make(reserved=(0xe3200000,0x1000)),0)
add('adjacent_high_before',make(reserved=(0x100000000,0x200000)),0)
add('adjacent_high_after',make(reserved=(0x17fe00000,0x100000)),0)
add('dynamic',make(dynamic=True),9)
add('bad_magic',b'BAD!'+base[4:],2)
for n in [0,1,4,8,20,39,40,len(base)-1]:add('truncated_'+str(n),base[:n],2)
for field,val in [(8,41),(8,0xfffffffc),(12,0xfffffff0),(16,41),(16,0xfffffff0),(20,18),(24,18),(32,0xffffffff),(36,0xffffffff)]:
    b=bytearray(base);struct.pack_into('>I',b,field,val);add('header_%d'%field,b,2)
add('reserved_overflow',make(reserved=(0x94000000,0xffffffffffffffff)),7)
add('reserved_zero_size',make(reserved=(0x94000000,0)),7)
# MP_BOARD is enum 5 (keep fixture readable, correct value here).
cases=[(name,5 if name=='wrong_board' else expected) for name,expected in cases]
source=ROOT/'tests'/'fixtures'/'live_fdt.dts'
subprocess.run(['dtc','-q','-I','dts','-O','dtb','-o',str(OUT/'live_dt.dtb'),str(source)],check=True)
cases.append(('live_dt',0))
(OUT/'manifest.txt').write_text(''.join('%s %d\n'%x for x in cases))
meta={ 'case_count':len(cases),'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
      'candidate_regions':[{'base':hex(b),'size':hex(s),'MiB':s//1048576} for b,s in [(0xa0000000,0x43200000),(0x100200000,0x7fc00000)]],
      'added_MiB':3118,'including_existing_MiB':3184 }
(ROOT/'evidence'/'MEMORY_PLAN.json').write_text(json.dumps(meta,indent=2)+'\n')
print(json.dumps(meta))
