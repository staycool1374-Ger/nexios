#include <fdt/libfdt.h>
#include <fdt/libfdt_internal.h>
#include <string.hpp>

extern "C" {

int fdt_check_header(const void *fdt) {
    if (!fdt) return -FDT_ERR_BADOFFSET;
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    if (fdt32_to_cpu(h->magic) != FDT_MAGIC)
        return -FDT_ERR_BADMAGIC;
    if (fdt32_to_cpu(h->version) < FDT_FIRST_SUPPORTED_VERSION)
        return -FDT_ERR_BADVERSION;
    if (fdt32_to_cpu(h->version) > FDT_LAST_SUPPORTED_VERSION)
        return -FDT_ERR_BADVERSION;
    if (fdt32_to_cpu(h->totalsize) < (int)sizeof(struct fdt_header))
        return -FDT_ERR_TRUNCATED;
    return 0;
}

int fdt_num_mem_rsv(const void *fdt) {
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    int count = 0;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions)
    int offset = fdt32_to_cpu(h->off_mem_rsvmap);

    for (;;) {
        const struct fdt_reserve_entry *re =
            (const struct fdt_reserve_entry *)((const char *)fdt + offset);
        uint64_t addr = fdt64_to_cpu(re->address);
        uint64_t size = fdt64_to_cpu(re->size);
        if (addr == 0 && size == 0) break;
        count++;
        offset += sizeof(struct fdt_reserve_entry);
    }
    return count;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int fdt_get_mem_rsv(const void *fdt, int n, uint64_t *addr, uint64_t *size) {
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions)
    int offset = fdt32_to_cpu(h->off_mem_rsvmap);

    for (int i = 0; i <= n; i++) {
        const struct fdt_reserve_entry *re =
            (const struct fdt_reserve_entry *)((const char *)fdt + offset);
        uint64_t a = fdt64_to_cpu(re->address);
        uint64_t s = fdt64_to_cpu(re->size);
        if (a == 0 && s == 0) return -FDT_ERR_NOTFOUND;
        if (i == n) {
            if (addr) *addr = a;
            if (size) *size = s;
            return 0;
        }
        offset += sizeof(struct fdt_reserve_entry);
    }
    return -FDT_ERR_NOTFOUND;
}

const char *fdt_string(const void *fdt, int stroffset) {
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    if (fdt_check_header(fdt) != 0) return nullptr;
    if (stroffset < 0) return nullptr;
    if (stroffset >= (int)fdt32_to_cpu(h->size_dt_strings)) return nullptr;
    return (const char *)fdt + fdt32_to_cpu(h->off_dt_strings) + stroffset;
}

const char *fdt_get_name(const void *fdt, int nodeoffset, int *len) {
    const struct fdt_node_header *nh;
    const char *name;
    int err;

    // NOLINTNEXTLINE(bugprone-assignment-in-if-condition)
    if ((err = fdt_check_header(fdt)) != 0)
        goto fail;

    err = fdt_check_node_offset_(fdt, nodeoffset);
    if (err < 0)
        goto fail;

    nh = (const struct fdt_node_header *)fdt_offset_ptr_(fdt, nodeoffset);
    if (!nh)
        goto fail;

    name = nh->name;
    if (len)
        // NOLINTNEXTLINE(bugprone-narrowing-conversions)
        *len = strlen(name);

    return name;

fail:
    if (len)
        *len = err;
    return nullptr;
}

int fdt_first_child(const void *fdt, int parentoffset) {
    int offset, next;
    uint32_t tag;

    if (fdt_check_header(fdt) != 0)
        return -FDT_ERR_BADOFFSET;

    tag = fdt_next_tag_(fdt, parentoffset, &next);
    if (tag != FDT_BEGIN_NODE)
        return -FDT_ERR_BADOFFSET;

    offset = next;
    tag = fdt_next_tag_(fdt, offset, &next);

    if (tag != FDT_BEGIN_NODE)
        return -FDT_ERR_NOTFOUND;

    return offset;
}

int fdt_next_sibling(const void *fdt, int offset) {
    int depth = 1;
    uint32_t tag;

    if (fdt_check_header(fdt) != 0)
        return -FDT_ERR_BADOFFSET;

    // The offset must aim at the node's BEGIN_NODE tag; anything else is a
    // caller error (issue #183: first_child/subnode_offset produce BEGIN
    // offsets, so the pair stays coherent).
    tag = fdt_next_tag_(fdt, offset, &offset);
    if (tag != FDT_BEGIN_NODE)
        return -FDT_ERR_BADOFFSET;

    // Consume the current node's whole subtree (props, children, END_NODE).
    while (depth > 0) {
        tag = fdt_next_tag_(fdt, offset, &offset);
        if (tag == FDT_BEGIN_NODE) {
            depth++;
        } else if (tag == FDT_END_NODE) {
            depth--;
        } else if (tag == FDT_END) {
            return -FDT_ERR_NOTFOUND;
        }
    }
    // Offset now aims at the tag following the node: a sibling's BEGIN_NODE,
    // the parent's END_NODE, or FDT_END.
    int sibling = offset;
    tag = fdt_next_tag_(fdt, offset, &offset);
    if (tag == FDT_BEGIN_NODE)
        return sibling;
    return -FDT_ERR_NOTFOUND;
}

int fdt_next_subnode(const void *fdt, int offset) {
    return fdt_next_sibling(fdt, offset);
}

int fdt_subnode_offset_namelen(const void *fdt, int parentoffset,
                               const char *name, int namelen) {
    int offset, next;
    uint32_t tag;

    if (fdt_check_header(fdt) != 0)
        return -FDT_ERR_BADOFFSET;

    tag = fdt_next_tag_(fdt, parentoffset, &next);
    if (tag != FDT_BEGIN_NODE)
        return -FDT_ERR_BADOFFSET;

    offset = next;

    for (;;) {
        tag = fdt_next_tag_(fdt, offset, &next);
        if (tag != FDT_BEGIN_NODE) {
            if (tag == FDT_END_NODE)
                return -FDT_ERR_NOTFOUND;
            if (tag == FDT_END)
                return -FDT_ERR_NOTFOUND;
            offset = next;
            continue;
        }

        const char *nname = (const char *)fdt_offset_ptr_(fdt, offset + FDT_TAGSIZE);
        if (!nname)
            return -FDT_ERR_TRUNCATED;

        if (strncmp(nname, name, (size_t)namelen) == 0 && nname[namelen] == '\0')
            return offset;

        /* Skip this node's children */
        int depth = 1;
        int skip = next;
        while (depth > 0) {
            uint32_t stag = fdt_next_tag_(fdt, skip, &skip);
            if (stag == FDT_BEGIN_NODE) depth++;
            else if (stag == FDT_END_NODE) depth--;
            else if (stag == FDT_END) return -FDT_ERR_NOTFOUND;
        }
        offset = skip;
    }
}

const void *fdt_getprop_namelen(const void *fdt, int nodeoffset,
                                const char *name, int *lenp) {
    int offset, next;
    uint32_t tag;

    if (fdt_check_header(fdt) != 0) {
        if (lenp) *lenp = -FDT_ERR_BADOFFSET;
        return nullptr;
    }

    tag = fdt_next_tag_(fdt, nodeoffset, &next);
    if (tag != FDT_BEGIN_NODE) {
        if (lenp) *lenp = -FDT_ERR_BADOFFSET;
        return nullptr;
    }

    offset = next;

    for (;;) {
        tag = fdt_next_tag_(fdt, offset, &next);
        if (tag == FDT_PROP) {
            const uint32_t *lenp_tag = (const uint32_t *)fdt_offset_ptr_(fdt, offset + FDT_TAGSIZE);
            if (!lenp_tag) break;
            uint32_t proplen = fdt32_to_cpu(*lenp_tag);
            const uint32_t *nameoffp = (const uint32_t *)fdt_offset_ptr_(fdt, offset + FDT_TAGSIZE + FDT_TAGSIZE);
            if (!nameoffp) break;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions)
        int nameoff = static_cast<int>(fdt32_to_cpu(*nameoffp));
            const char *pname = fdt_string(fdt, nameoff);

            if (pname && strcmp(pname, name) == 0) {
                if (lenp) *lenp = (int)proplen;
                return fdt_offset_ptr_(fdt, offset + FDT_TAGSIZE + FDT_TAGSIZE + FDT_TAGSIZE);
            }
            offset = next;
        } else if (tag == FDT_END_NODE || tag == FDT_END) {
            break;
        } else {
            offset = next;
        }
    }

    if (lenp) *lenp = -FDT_ERR_NOTFOUND;
    return nullptr;
}

const void *fdt_getprop_by_offset(const void *fdt, int offset,
                                  const char **outname, int *lenp) {
    uint32_t tag;
    int next;

    if (fdt_check_header(fdt) != 0) {
        if (lenp) *lenp = -FDT_ERR_BADOFFSET;
        return nullptr;
    }

    tag = fdt_next_tag_(fdt, offset, &next);
    if (tag != FDT_PROP) {
        if (lenp) *lenp = -FDT_ERR_BADOFFSET;
        return nullptr;
    }

    {
        const uint32_t *lenp_tag = (const uint32_t *)fdt_offset_ptr_(fdt, offset + FDT_TAGSIZE);
        if (!lenp_tag) {
            if (lenp) *lenp = -FDT_ERR_TRUNCATED;
            return nullptr;
        }
        uint32_t proplen = fdt32_to_cpu(*lenp_tag);
        const uint32_t *nameoffp = (const uint32_t *)fdt_offset_ptr_(fdt, offset + FDT_TAGSIZE + FDT_TAGSIZE);
        if (!nameoffp) {
            if (lenp) *lenp = -FDT_ERR_TRUNCATED;
            return nullptr;
        }
        int nameoff = static_cast<int>(fdt32_to_cpu(*nameoffp));

        if (outname)
            *outname = fdt_string(fdt, nameoff);
        if (lenp)
            *lenp = (int)proplen;

        return fdt_offset_ptr_(fdt, offset + FDT_TAGSIZE + FDT_TAGSIZE + FDT_TAGSIZE);
    }
}

int fdt_node_offset_by_prop_value(const void *fdt, int startoffset,
                                  const char *propname,
                                  const void *propval, int proplen) {
    int offset, next;
    uint32_t tag;

    if (fdt_check_header(fdt) != 0)
        return -FDT_ERR_BADOFFSET;

    if (startoffset < 0)
        offset = 0;
    else
        offset = startoffset;

    for (;;) {
        tag = fdt_next_tag_(fdt, offset, &next);
        if (tag == FDT_BEGIN_NODE) {
            /* Check properties of this node */
            int prop_offset = next;
            int prop_next;

            for (;;) {
                uint32_t ptag = fdt_next_tag_(fdt, prop_offset, &prop_next);
                if (ptag == FDT_PROP) {
                    const uint32_t *lenp = (const uint32_t *)fdt_offset_ptr_(fdt, prop_offset + FDT_TAGSIZE);
                    if (!lenp) return -FDT_ERR_TRUNCATED;
                    uint32_t plen = fdt32_to_cpu(*lenp);
                    if ((int)plen == proplen) {
                        const uint32_t *nameoffp = (const uint32_t *)fdt_offset_ptr_(fdt, prop_offset + FDT_TAGSIZE + FDT_TAGSIZE);
                        if (!nameoffp) return -FDT_ERR_TRUNCATED;
                        const char *pn = fdt_string(fdt, static_cast<int>(fdt32_to_cpu(*nameoffp)));
                        if (pn && strcmp(pn, propname) == 0) {
                            if (propval) {
                                const void *val = fdt_offset_ptr_(fdt, prop_offset + FDT_TAGSIZE + FDT_TAGSIZE + FDT_TAGSIZE);
                                if (val && memcmp(val, propval, (size_t)proplen) == 0)
                                    return offset;
                            } else {
                                return offset;
                            }
                        }
                    }
                    prop_offset = prop_next;
                } else if (ptag == FDT_END_NODE || ptag == FDT_END) {
                    break;
                } else {
                    prop_offset = prop_next;
                }
            }

            offset = next;
        } else if (tag == FDT_END) {
            return -FDT_ERR_NOTFOUND;
        } else {
            offset = next;
        }
    }
}

} /* extern "C" */
