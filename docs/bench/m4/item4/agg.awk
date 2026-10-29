# SPDX-License-Identifier: GPL-3.0-or-later
# M4 item 4: averages stats-mem lines by tab count. Usage: sed "s/stats-mem //" X-mem.txt | awk -f agg.awk
{ for(i=2;i<=NF;i++){split($i,kv,"="); v[kv[1]]=kv[2]}
  t=v["tabs"]; n[t]++; for(k in v) if(k!="t"&&k!="tabs") s[t,k]+=v[k];
  if (v["heap-arena"]>ma) ma=v["heap-arena"]; if (v["heap-inuse"]>mi) mi=v["heap-inuse"]; if(v["texture-bytes"]>mt) mt=v["texture-bytes"]; if(v["heap-mmap"]>mm) mm=v["heap-mmap"]}
END{ for(t=1;t<=4;t++) if(n[t]) printf "tabs=%d n=%d inuse=%.1fMB mmap=%.1fMB tex=%.1fMB surf=%.1fMB sb=%.1fMB glyph=%.2fMB free=%.1fMB\n",t,n[t],s[t,"heap-inuse"]/n[t]/1e6,s[t,"heap-mmap"]/n[t]/1e6,s[t,"texture-bytes"]/n[t]/1e6,s[t,"surface-bytes"]/n[t]/1e6,s[t,"sb-bytes"]/n[t]/1e6,s[t,"glyph-bytes"]/n[t]/1e6,s[t,"heap-free"]/n[t]/1e6;
 printf "max arena=%.1fMB max inuse=%.1fMB max tex=%.1fMB max mmap=%.1fMB\n",ma/1e6,mi/1e6,mt/1e6,mm/1e6}
