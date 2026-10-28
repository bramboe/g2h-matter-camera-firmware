/* glibc 2.19 combines ordinary and PLT REL ranges; keep them adjacent. */
SECTIONS { .rel.plt : { *(.rel.plt) *(.rel.iplt) } } INSERT AFTER .rel.dyn;
