/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "memory_plan.h"
#define MAX_RANGES 128u
#define MAX_DEPTH 64u
#define MAX_NODES 4096u
struct node {
  const mp_u8 *reg;
  mp_u32 reglen, acells, scells;
  int is_memory, is_reserved_root, is_reserved_child, has_dynamic, disabled, memory_type;
};
static mp_u32 be32(const mp_u8 *p) {
  return ((mp_u32)p[0]<<24)|((mp_u32)p[1]<<16)|((mp_u32)p[2]<<8)|p[3];
}
static mp_u64 be64(const mp_u8 *p) { return ((mp_u64)be32(p)<<32)|be32(p+4); }
static int eq(const mp_u8 *a, const char *b) {
  while (*a && *b && *a==(mp_u8)*b) { ++a; ++b; }
  return !*a && !*b;
}
static int prefix(const mp_u8 *a, const char *b) {
  while (*b) { if (*a!=(mp_u8)*b) return 0; ++a; ++b; } return 1;
}
static int string_list(const mp_u8 *p, mp_u32 n, const char *wanted) {
  mp_u32 i=0, j; int found=0;
  while (i<n) {
    for (j=i;j<n && p[j];++j) {}
    if (j==n) return -1;
    if (eq(p+i,wanted)) found=1;
    i=j+1;
  } return found;
}
static int valid_range(mp_u64 base, mp_u64 size) {
  return size!=0 && base<=~(mp_u64)0-size;
}
static int intersects(struct mp_range a, struct mp_range b) {
  return a.base<b.base+b.size && b.base<a.base+a.size;
}
static enum mp_result append(struct mp_range *a, mp_u32 *count, const mp_u8 *p, mp_u32 len) {
  mp_u32 i;
  if (!len || len%16) return MP_CELLS;
  for(i=0;i<len;i+=16) {
    if(*count==MAX_RANGES || !valid_range(be64(p+i),be64(p+i+8))) return MP_RANGE;
    a[*count].base=be64(p+i); a[*count].size=be64(p+i+8); ++*count;
  } return MP_OK;
}
enum mp_result mp_make_plan(const void *blob, mp_size available, struct mp_plan *out) {
  const mp_u8 *b=(const mp_u8*)blob, *strings, *p, *end, *name, *value;
  struct node stack[MAX_DEPTH];
  struct mp_range dram[MAX_RANGES], reserved[MAX_RANGES];
  struct mp_range candidates[2]={{0xa0000000ULL,0x43200000ULL},{0x100200000ULL,0x7fc00000ULL}};
  mp_u32 total, os, oz, rs, ns, nz, pos, token, len, noff, depth=0, nodes=0;
  mp_u32 memory_count=0, reserved_count=0, root_ac=0, root_sc=0, reserved_roots=0, i,j;
  int compatible=0, ended=0, root_seen=0, covered;
  enum mp_result result;
  if(!b || !out) return MP_ARGUMENT;
  out->count=0; out->total=0; out->reserved_count=0; out->memory_count=0;
  if(available<40 || available>0x100000u || be32(b)!=0xd00dfeedu) return MP_HEADER;
  total=be32(b+4); os=be32(b+8); oz=be32(b+12); rs=be32(b+16);
  ns=be32(b+36); nz=be32(b+32);
  if(total<40 || total>available || be32(b+20)!=17 || be32(b+24)>17 ||
     os<40 || oz<40 || rs<40 || os%4 || rs%8 || ns<12 || ns%4 ||
     os>total || ns>total-os || oz>total || nz>total-oz || !nz || rs>total-16 ||
     (os<oz+nz && oz<os+ns)) return MP_HEADER;
  for(pos=rs;;pos+=16) {
    if(pos>total-16 || (pos<os+ns && os<pos+16) || (pos<oz+nz && oz<pos+16)) return MP_HEADER;
    if(be64(b+pos)==0 && be64(b+pos+8)==0) break;
    result=append(reserved,&reserved_count,b+pos,16); if(result) return result;
  }
  strings=b+oz; p=b+os; end=p+ns;
  while(p<end) {
    if((mp_size)(end-p)<4) return MP_STRUCTURE;
    token=be32(p); p+=4;
    if(token==1) {
      if(depth==MAX_DEPTH || ++nodes>MAX_NODES || (!depth && root_seen)) return MP_STRUCTURE;
      name=p; while(p<end && *p) ++p; if(p==end) return MP_STRUCTURE;
      len=(mp_u32)(p-name)+1; len=(len+3u)&~3u;
      if((mp_size)(end-name)<len) return MP_STRUCTURE;
      p=name+len;
      stack[depth].reg=0; stack[depth].reglen=0; stack[depth].acells=2;
      stack[depth].scells=2; stack[depth].has_dynamic=0;
      stack[depth].disabled=0; stack[depth].memory_type=0;
      stack[depth].is_memory=depth==1 && (eq(name,"memory")||prefix(name,"memory@"));
      stack[depth].is_reserved_root=depth==1 && eq(name,"reserved-memory");
      stack[depth].is_reserved_child=depth==2 && stack[1].is_reserved_root;
      if(!depth) { if(*name) return MP_STRUCTURE; root_seen=1; }
      if(depth==1 && stack[depth].is_reserved_root) ++reserved_roots;
      ++depth;
    } else if(token==2) {
      struct node *n;
      if(!depth) return MP_STRUCTURE;
      n=&stack[--depth];
      if(n->is_reserved_root && (n->acells!=2 || n->scells!=2)) return MP_CELLS;
      if(n->is_memory || n->is_reserved_child) {
        if(n->is_memory && (n->disabled || !n->memory_type)) return MP_RANGE;
        if(n->has_dynamic) return MP_DYNAMIC_RESERVATION;
        if(!n->reg) return MP_MISSING;
        result=append(n->is_memory?dram:reserved,n->is_memory?&memory_count:&reserved_count,n->reg,n->reglen);
        if(result) return result;
      }
    } else if(token==3) {
      if(!depth || (mp_size)(end-p)<8) return MP_STRUCTURE;
      len=be32(p); noff=be32(p+4); p+=8;
      if(len>(mp_size)(end-p) || noff>=nz) return MP_STRUCTURE;
      for(i=noff;i<nz && strings[i];++i) {} if(i==nz) return MP_STRUCTURE;
      name=strings+noff; value=p;
      if(len>0xfffffffcu || ((len+3u)&~3u)>(mp_size)(end-p)) return MP_STRUCTURE;
      p+=(len+3u)&~3u;
      if(eq(name,"#address-cells")||eq(name,"#size-cells")) {
        if(len!=4) return MP_CELLS;
        if(eq(name,"#address-cells")) { stack[depth-1].acells=be32(value); if(depth==1) root_ac=be32(value); }
        else { stack[depth-1].scells=be32(value); if(depth==1) root_sc=be32(value); }
      }
      if(depth==1 && eq(name,"compatible")) { compatible=string_list(value,len,"sprd,ums512"); if(compatible<0) return MP_STRUCTURE; }
      if(eq(name,"device_type") && depth==2 && string_list(value,len,"memory")==1) {
        stack[1].is_memory=1; stack[1].memory_type=1;
      }
      if(eq(name,"status") && string_list(value,len,"okay")!=1 && string_list(value,len,"ok")!=1) stack[depth-1].disabled=1;
      if(eq(name,"reg")) { if(stack[depth-1].reg) return MP_STRUCTURE; stack[depth-1].reg=value; stack[depth-1].reglen=len; }
      if(stack[depth-1].is_reserved_child && (eq(name,"size")||eq(name,"alloc-ranges"))) stack[depth-1].has_dynamic=1;
      if(stack[depth-1].is_reserved_root && eq(name,"ranges") && len) return MP_RANGE;
    } else if(token==4) {
      /* FDT_NOP */
    } else if(token==9) {
      if(depth || !root_seen || p!=end) return MP_STRUCTURE;
      ended=1; break;
    } else return MP_STRUCTURE;
  }
  if(!ended) return MP_STRUCTURE;
  if(root_ac!=2 || root_sc!=2) return MP_CELLS;
  if(!compatible) return MP_BOARD;
  if(!memory_count || reserved_roots!=1 || reserved_count<1) return MP_MISSING;
  for(i=0;i<2;++i) {
    covered=0;
    for(j=0;j<memory_count;++j) if(candidates[i].base>=dram[j].base && candidates[i].base+candidates[i].size<=dram[j].base+dram[j].size) covered=1;
    if(!covered) return MP_RANGE;
    for(j=0;j<reserved_count;++j) if(intersects(candidates[i],reserved[j])) return MP_OVERLAP;
  }
  out->regions[0]=candidates[0]; out->regions[1]=candidates[1]; out->count=2;
  out->total=candidates[0].size+candidates[1].size;
  out->reserved_count=reserved_count; out->memory_count=memory_count;
  return MP_OK;
}
