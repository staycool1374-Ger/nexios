#pragma once

#include <fdt/fdt.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define FDT_ERR_NOTFOUND      1
#define FDT_ERR_EXISTS        2
#define FDT_ERR_NOSPACE       3
#define FDT_ERR_BADOFFSET     4
#define FDT_ERR_BADPATH       5
#define FDT_ERR_BADPHANDLE    6
#define FDT_ERR_BADSTATE      7
#define FDT_ERR_TRUNCATED     8
#define FDT_ERR_BADMAGIC      9
#define FDT_ERR_BADVERSION   10
#define FDT_ERR_BADSTRUCTURE 11
#define FDT_ERR_BADLAYOUT    12
#define FDT_ERR_INTERNAL     13
#define FDT_ERR_BADNCELLS    14
#define FDT_ERR_BADVALUE     15
#define FDT_ERR_BADOVERLAY   16
#define FDT_ERR_NOPHANDLES   17

#define FDT_ERR_MAX 17

/* Untrusted-DTB validation bounds (issue #350): the DTB is firmware
 * input — layout + structure must be proven before any public walk. */
#define FDT_MAX_TOTALSIZE (2u * 1024u * 1024u)
#define FDT_MAX_NODES 4096
#define FDT_MAX_DEPTH 32

/* Public API */
int fdt_check_header(const void *fdt);

/* Validate header offsets/sizes against totalsize (returns -FDT_ERR_*). */
int fdt_check_layout(const void *fdt);

/* Bounded structure-block walk: tags, lengths, name offsets, depth. */
int fdt_check_structure(const void *fdt);

int fdt_num_mem_rsv(const void *fdt);
int fdt_get_mem_rsv(const void *fdt, int n, uint64_t *addr, uint64_t *size);

int fdt_first_child(const void *fdt, int parentoffset);
int fdt_next_sibling(const void *fdt, int offset);
int fdt_next_subnode(const void *fdt, int offset);

const char *fdt_get_name(const void *fdt, int nodeoffset, int *len);
const char *fdt_string(const void *fdt, int stroffset);

int fdt_subnode_offset_namelen(const void *fdt, int parentoffset,
                               const char *name, int namelen);

const void *fdt_getprop_namelen(const void *fdt, int nodeoffset,
                                const char *name, int *lenp);
const void *fdt_getprop_by_offset(const void *fdt, int offset,
                                  const char **outname, int *lenp);

int fdt_node_offset_by_prop_value(const void *fdt, int startoffset,
                                  const char *propname,
                                  const void *propval, int proplen);

const char *fdt_strerror(int errval);

#ifdef __cplusplus
}
#endif
