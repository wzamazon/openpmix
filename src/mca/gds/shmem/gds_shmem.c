/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "gds_shmem.h"
#include "gds_shmem_utils.h"
#include "gds_shmem_store.h"
#include "gds_shmem_fetch.h"

#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"
#include "src/util/pmix_vmem.h"

#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"

//
// Notes for developers:
// We cannot use PMIX_CONSTRUCT for data that are stored in shared memory
// because their address is on the stack of the process in which they are
// constructed.
//

// Some items for future consideration:
// * Address FT case at some point. We need to have a broader conversion about
//   how we go about doing this. Ralph has some ideas.
// * Is it worth adding memory arena boundary checks to our TMA allocators?

/**
 * Key names used to find shared-memory segment info.
 */
#define SHMEM_SEG_BLOB_KEY "PMIX_GDS_SHMEM_SEG_BLOB"
#define SHMEM_SEG_NSID_KEY "PMIX_GDS_SHMEM_NSPACEID"
#define SHMEM_SEG_SMID_KEY "PMIX_GDS_SHMEM_SMSEGID"
#define SHMEM_SEG_PATH_KEY "PMIX_GDS_SHMEM_SEG_PATH"
#define SHMEM_SEG_SIZE_KEY "PMIX_GDS_SHMEM_SEG_SIZE"
#define SHMEM_SEG_ADDR_KEY "PMIX_GDS_SHMEM_SEG_ADDR"

/**
 * Stores packed job statistics.
 */
typedef struct {
    size_t packed_size;
    size_t hash_table_size;
} pmix_gds_shmem_local_job_stats_t;

/**
 * Store unpacked shared-memory segment information.
 */
typedef struct {
    pmix_object_t super;
    char *nsid;
    pmix_gds_shmem_job_shmem_id_t smid;
    char *seg_path;
    size_t seg_size;
    size_t seg_addr;
} pmix_gds_shmem_unpacked_seg_blob_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_unpacked_seg_blob_t);

/**
 * String to size_t.
 */
static inline pmix_status_t
strtost(
    const char *str,
    int base,
    size_t *maybe_val
) {
    *maybe_val = 0;

    errno = 0;
    char *end = NULL;
    const long long val = strtoll(str, &end, base);
    const int err = errno;

    if ((err == ERANGE && val == LLONG_MAX) ||
        (err == ERANGE && val == LLONG_MIN) ||
        *end != '\0') {
        return PMIX_ERROR;
    }
    *maybe_val = (size_t)val;
    return PMIX_SUCCESS;
}

/**
 * Architecture-specific address alignment.
 */
static inline void *
addr_align(
    void *base,
    size_t size
) {
#if 0 // Helpful debug
    PMIX_GDS_SHMEM_VVOUT("------------------------ADDRINN=%p,%zd", base, size);
#endif
    void *const res = (void *)(((uintptr_t)base + size + 7) & ~(uintptr_t)0x07);
#if 0 // Helpful debug
    // Make sure that it's 8-byte aligned.
    assert ((uintptr_t)res % 8 == 0);
    PMIX_GDS_SHMEM_VVOUT("------------------------ADDROUT=%p,%zd", res, size);
#endif
    return res;
}

static inline void *
tma_malloc(
    pmix_tma_t *tma,
    size_t size
) {
    void *const current = *(tma->data_ptr);
#if PMIX_ENABLE_DEBUG
    memset(current, 0, size);
#endif
    *(tma->data_ptr) = addr_align(current, size);
    return current;
}

static inline void *
tma_calloc(
    struct pmix_tma *tma,
    size_t nmemb,
    size_t size
) {
    const size_t real_size = nmemb * size;
    void *const current = *(tma->data_ptr);
    memset(current, 0, real_size);
    *(tma->data_ptr) = addr_align(current, real_size);
    return current;
}

static inline void *
tma_realloc(
    pmix_tma_t *tma,
    void *ptr,
    size_t size
) {
    PMIX_HIDE_UNUSED_PARAMS(tma, ptr, size);
    // We don't support realloc.
    assert(false);
    return NULL;
}

static inline char *
tma_strdup(
    pmix_tma_t *tma,
    const char *s
) {
    void *const current = *(tma->data_ptr);
    const size_t size = strlen(s) + 1;
    *(tma->data_ptr) = addr_align(current, size);
    return (char *)memmove(current, s, size);
}

static inline void *
tma_memmove(
    struct pmix_tma *tma,
    const void *src,
    size_t size
) {
    void *const current = *(tma->data_ptr);
    *(tma->data_ptr) = addr_align(current, size);
    return memmove(current, src, size);
}

static inline void
tma_free(
    struct pmix_tma *tma,
    void *ptr
) {
    PMIX_HIDE_UNUSED_PARAMS(tma, ptr);
}

static void
tma_init_function_pointers(
    pmix_tma_t *tma
) {
    tma->tma_malloc = tma_malloc;
    tma->tma_calloc = tma_calloc;
    tma->tma_realloc = tma_realloc;
    tma->tma_strdup = tma_strdup;
    tma->tma_memmove = tma_memmove;
    tma->tma_free = tma_free;
}

static void
tma_init(
    pmix_tma_t *tma,
    void *data_ptr
) {
    tma_init_function_pointers(tma);
    tma->data_ptr = data_ptr;
}

static void
unpacked_seg_blob_construct(
    pmix_gds_shmem_unpacked_seg_blob_t *ub
) {
    ub->nsid = NULL;
    ub->smid = PMIX_GDS_SHMEM_INVALID_ID;
    ub->seg_path = NULL;
    ub->seg_size = 0;
    ub->seg_addr = 0;
}

static void
unpacked_seg_blob_destruct(
    pmix_gds_shmem_unpacked_seg_blob_t *ub
) {
    free(ub->nsid);
    free(ub->seg_path);
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_unpacked_seg_blob_t,
    pmix_object_t,
    unpacked_seg_blob_construct,
    unpacked_seg_blob_destruct
);

static void
host_alias_construct(
    pmix_gds_shmem_host_alias_t *a
) {
    a->name = NULL;
}

static void
host_alias_destruct(
    pmix_gds_shmem_host_alias_t *a
) {
    pmix_tma_t *const tma = pmix_obj_get_tma(&a->super.super);
    if (a->name) {
        pmix_tma_free(tma, a->name);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_host_alias_t,
    pmix_list_item_t,
    host_alias_construct,
    host_alias_destruct
);

static void
nodeinfo_construct(
    pmix_gds_shmem_nodeinfo_t *n
) {
    pmix_tma_t *const tma = pmix_obj_get_tma(&n->super.super);

    n->nodeid = UINT32_MAX;
    n->hostname = NULL;
    n->aliases = PMIX_NEW(pmix_list_t, tma);
    n->info = PMIX_NEW(pmix_list_t, tma);
}

static void
nodeinfo_destruct(
    pmix_gds_shmem_nodeinfo_t *n
) {
    pmix_tma_t *const tma = pmix_obj_get_tma(&n->super.super);

    pmix_tma_free(tma, n->hostname);
    if (n->aliases) {
        PMIX_LIST_DESTRUCT(n->aliases);
    }
    if (n->info) {
        PMIX_LIST_DESTRUCT(n->info);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_nodeinfo_t,
    pmix_list_item_t,
    nodeinfo_construct,
    nodeinfo_destruct
);

static void
job_construct(
    pmix_gds_shmem_job_t *job
) {
    job->nspace_id = NULL;
    job->nspace = NULL;
    // Job
    job->shmem_status = 0;
    job->shmem = PMIX_NEW(pmix_shmem_t);
    job->smdata = NULL;
    // Modex
    job->modex_shmem_status = 0;
    job->modex_shmem = PMIX_NEW(pmix_shmem_t);
    job->smmodex = NULL;
}

static void
emit_shmem_usage_stats(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_tma_t *tma = NULL;
    const char *smname = NULL;

    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_by_id(
        job, shmem_id, &shmem
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    switch (shmem_id) {
        case PMIX_GDS_SHMEM_JOB_ID:
            tma = &job->smdata->tma;
            smname = "smdata";
            break;
        case PMIX_GDS_SHMEM_MODEX_ID:
            tma = &job->smmodex->tma;
            smname = "smmodex";
            break;
        case PMIX_GDS_SHMEM_INVALID_ID:
        default:
            PMIX_ERROR_LOG(rc);
            return;
    }

    const size_t shmem_size = shmem->size;
    const size_t bytes_used = (size_t)((uintptr_t)*(tma->data_ptr)
                            - (uintptr_t)shmem->base_address);
    const float utilization = (bytes_used / (float)shmem_size) * 100.0;

    PMIX_GDS_SHMEM_VOUT(
        "%s memory statistics: "
        "segment size=%zd, bytes used=%zd, utilization=%.2f %%",
        smname, shmem_size, bytes_used, utilization
    );
}

static void
job_destruct(
    pmix_gds_shmem_job_t *job
) {
    pmix_status_t rc = PMIX_SUCCESS;

    static const pmix_gds_shmem_job_shmem_id_t shmem_ids[] = {
        PMIX_GDS_SHMEM_JOB_ID,
        PMIX_GDS_SHMEM_MODEX_ID,
        PMIX_GDS_SHMEM_INVALID_ID
    };

    if (job->nspace_id) {
        free(job->nspace_id);
    }
    if (job->nspace) {
        PMIX_RELEASE(job->nspace);
    }
    for (int i = 0; shmem_ids[i] != PMIX_GDS_SHMEM_INVALID_ID; ++i) {
        const pmix_gds_shmem_job_shmem_id_t sid = shmem_ids[i];

        pmix_shmem_t *shmem;
        rc = pmix_gds_shmem_get_job_shmem_by_id(job, sid, &shmem);
        if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        if (pmix_gds_shmem_has_status(job, sid, PMIX_GDS_SHMEM_RELEASE)) {
            // Emit usage status before we destroy the segment.
            emit_shmem_usage_stats(job, sid);
        }
        // Invalidate the shmem flags.
        pmix_gds_shmem_clearall_status(job, sid);
        // Releases memory for the structures located in shared-memory.
        PMIX_RELEASE(shmem);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_job_t,
    pmix_list_item_t,
    job_construct,
    job_destruct
);

static void
app_construct(
    pmix_gds_shmem_app_t *a
) {
    pmix_tma_t *const tma = pmix_obj_get_tma(&a->super.super);

    a->appnum = 0;
    a->appinfo = PMIX_NEW(pmix_list_t, tma);
    a->nodeinfo = PMIX_NEW(pmix_list_t, tma);
    a->job = NULL;
}

static void
app_destruct(
    pmix_gds_shmem_app_t *a
) {
    if (a->appinfo) {
        PMIX_LIST_DESTRUCT(a->appinfo);
    }
    if (a->nodeinfo) {
        PMIX_LIST_DESTRUCT(a->nodeinfo);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_app_t,
    pmix_list_item_t,
    app_construct,
    app_destruct
);

static void
session_construct(
    pmix_gds_shmem_session_t *s
) {
    pmix_tma_t *const tma = pmix_obj_get_tma(&s->super.super);

    s->session = UINT32_MAX;
    s->sessioninfo = PMIX_NEW(pmix_list_t, tma);
    s->nodeinfo = PMIX_NEW(pmix_list_t, tma);
}

static void
session_destruct(
    pmix_gds_shmem_session_t *s
) {
    if (s->sessioninfo) {
        PMIX_LIST_DESTRUCT(s->sessioninfo);
    }
    if (s->nodeinfo) {
        PMIX_LIST_DESTRUCT(s->nodeinfo);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_session_t,
    pmix_list_item_t,
    session_construct,
    session_destruct
);

static pmix_status_t
job_smdata_construct(
    pmix_gds_shmem_job_t *job,
    size_t htsize
) {
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // job know about its data located at the base of the segment.
    void *const baseaddr = job->shmem->base_address;
    job->smdata = baseaddr;
    memset(job->smdata, 0, sizeof(*job->smdata));
    // Save the starting address for TMA memory allocations.
    job->smdata->current_addr = baseaddr;
    // Setup the TMA.
    tma_init(&job->smdata->tma, &job->smdata->current_addr);
    // Now we need to update the TMA's pointer to account for our using up some
    // space for its header.
    *(job->smdata->tma.data_ptr) = addr_align(baseaddr, sizeof(*job->smdata));
    // We can now safely get our TMA.
    pmix_tma_t *const tma = &job->smdata->tma;
    // Now that we know the TMA, initialize smdata structures using it.
    job->smdata->session = NULL;
    job->smdata->jobinfo = PMIX_NEW(pmix_list_t, tma);
    job->smdata->nodeinfo = PMIX_NEW(pmix_list_t, tma);
    job->smdata->appinfo = PMIX_NEW(pmix_list_t, tma);
    // Will always have local data, so set it up.
    job->smdata->local_hashtab = PMIX_NEW(pmix_hash_table_t, tma);
    pmix_hash_table_init(job->smdata->local_hashtab, htsize);

    pmix_gds_shmem_vout_smdata(job);

    return PMIX_SUCCESS;
}

static pmix_status_t
modex_smdata_construct(
    pmix_gds_shmem_job_t *job,
    size_t htsize
) {
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // job know about its data located at the base of the segment.
    void *const baseaddr = job->modex_shmem->base_address;
    job->smmodex = baseaddr;
    memset(job->smmodex, 0, sizeof(*job->smmodex));
    // Save the starting address for TMA memory allocations.
    job->smmodex->current_addr = baseaddr;
    // Setup the TMA.
    tma_init(&job->smmodex->tma, &job->smmodex->current_addr);
    // Now we need to update the TMA's pointer to account for our using up some
    // space for its header.
    *(job->smmodex->tma.data_ptr) = addr_align(baseaddr, sizeof(*job->smmodex));
    // We can now safely get our TMA.
    pmix_tma_t *const tma = &job->smmodex->tma;
    // Now that we know the TMA, initialize smdata structures using it.
    job->smmodex->hashtab = PMIX_NEW(pmix_hash_table_t, tma);
    pmix_hash_table_init(job->smmodex->hashtab, htsize);

    pmix_gds_shmem_vout_smmodex(job);

    return PMIX_SUCCESS;
}

/**
 * Returns page size.
 */
static inline size_t
get_page_size(void)
{
    const long i = sysconf(_SC_PAGE_SIZE);
    if (-1 == i) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return 0;
    }
    return i;
}

/**
 * Returns amount needed to pad provided size to page boundary.
 */
static inline size_t
pad_amount_to_page(
    size_t size
) {
    const size_t page_size = get_page_size();
    return ((~size) + page_size + 1) & (page_size - 1);
}

/**
 * Returns the base temp directory.
 */
static inline const char *
fetch_base_tmpdir(
    pmix_gds_shmem_job_t *job
) {
    pmix_status_t rc = PMIX_SUCCESS;

    static char fetched_path[PMIX_PATH_MAX] = {'\0'};
    // Keys we may fetch, in priority order.
    char *fetch_keys[] = {
        PMIX_NSDIR,
        PMIX_TMPDIR,
        NULL
    };
    // Did we get a usable fetched key/value?
    bool fetched_kv = false;

    for (int i = 0; NULL != fetch_keys[i]; ++i) {
        pmix_cb_t cb;
        PMIX_CONSTRUCT(&cb, pmix_cb_t);

        pmix_proc_t wildcard;
        PMIX_LOAD_PROCID(
            &wildcard,
            job->nspace->nspace,
            PMIX_RANK_WILDCARD
        );

        cb.key = fetch_keys[i];
        cb.proc = &wildcard;
        cb.copy = true;
        cb.scope = PMIX_LOCAL;

        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        if (rc != PMIX_SUCCESS) {
            PMIX_DESTRUCT(&cb);
            break;
        }
        // We should only have one item here.
        assert(1 == pmix_list_get_size(&cb.kvs));
        // Get a pointer to the only item in the list.
        pmix_kval_t *kv = (pmix_kval_t *)pmix_list_get_first(&cb.kvs);
        // Make sure we are dealing with the right stuff.
        assert(PMIX_CHECK_KEY(kv, fetch_keys[i]));
        assert(kv->value->type == PMIX_STRING);
        // Copy the value over.
        size_t nw = snprintf(
            fetched_path, PMIX_PATH_MAX, "%s",
            kv->value->data.string
        );
        PMIX_DESTRUCT(&cb);
        if (nw >= PMIX_PATH_MAX) {
            // Try another.
            continue;
        }
        else {
            // We got a usable fetched key.
            fetched_kv = true;
            break;
        }
    }
    // Didn't find a specific temp basedir, so just use a general one.
    if (!fetched_kv) {
        static const char *tmpdir = NULL;
        if (NULL == (tmpdir = getenv("TMPDIR"))) {
            tmpdir = "/tmp";
        }
        return tmpdir;
    }
    else {
        return fetched_path;
    }
}

/**
 * Returns a valid path or NULL on error.
 */
static inline const char *
get_shmem_backing_path(
    pmix_gds_shmem_job_t *job,
    const char *id
) {
    static char path[PMIX_PATH_MAX] = {'\0'};
    const char *basedir = fetch_base_tmpdir(job);
    // Now that we have the base path, append unique name.
    size_t nw = snprintf(
        path, PMIX_PATH_MAX, "%s/%s-gds-%s.%s-%s.%s.%d",
        basedir, PACKAGE_NAME, PMIX_GDS_SHMEM_NAME,
        pmix_globals.hostname, job->nspace_id, id, getpid()
    );
    if (nw >= PMIX_PATH_MAX) {
        return NULL;
    }
    return path;
}

/**
 * Attaches to the given shared-memory segment.
 */
static pmix_status_t
shmem_attach(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    uintptr_t req_addr
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_by_id(
        job, shmem_id, &shmem
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    uintptr_t mmap_addr = 0;
    rc = pmix_shmem_segment_attach(
        shmem, (void *)req_addr, &mmap_addr
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Make sure that we mapped to the requested address.
    if (PMIX_UNLIKELY(mmap_addr != req_addr)) {
        pmix_show_help(
            "help-gds-shmem.txt",
            "shmem-segment-attach:address-mismatch",
            true, (size_t)req_addr, (size_t)mmap_addr
        );
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: mmapd at address=0x%zx", __func__, (size_t)mmap_addr
    );
out:
    if (PMIX_SUCCESS != rc) {
        (void)pmix_shmem_segment_detach(shmem);
    }
    else {
        pmix_gds_shmem_set_status(
            job, shmem_id, PMIX_GDS_SHMEM_ATTACHED
        );
    }
    return rc;
}

static inline pmix_status_t
init_client_side_sm_data(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id
) {
    switch (shmem_id) {
        case PMIX_GDS_SHMEM_JOB_ID:
            job->smdata = job->shmem->base_address;
            pmix_gds_shmem_vout_smdata(job);
            break;
        case PMIX_GDS_SHMEM_MODEX_ID:
            job->smmodex = job->modex_shmem->base_address;
            pmix_gds_shmem_vout_smmodex(job);
            break;
        case PMIX_GDS_SHMEM_INVALID_ID:
        default:
            PMIX_ERROR_LOG(PMIX_ERROR);
            return PMIX_ERROR;
    }
    // Segment is ready for use by the client.
    pmix_gds_shmem_set_status(job, shmem_id, PMIX_GDS_SHMEM_READY_FOR_USE);
    // Note: don't update the TMA to point to its local function pointers
    // because clients should only be reading from the shared-memory segment.
    return PMIX_SUCCESS;
}

static pmix_status_t
shmem_segment_attach_and_init(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_unpacked_seg_blob_t *seginfo
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_by_id(
        job, seginfo->smid, &shmem
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Initialize the segment path.
    const size_t buffmax = sizeof(shmem->backing_path);
    pmix_string_copy(shmem->backing_path, seginfo->seg_path, buffmax);
    // Initialize the segment size.
    shmem->size = seginfo->seg_size;

    const uintptr_t req_addr = (uintptr_t)seginfo->seg_addr;
    rc = shmem_attach(job, seginfo->smid, req_addr);
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Now we can safely initialize our shared data structures.
    rc = init_client_side_sm_data(job, seginfo->smid);
#if 0
    // Protect memory: clients can only read from here.
    mprotect(
        shmem->base_address, shmem->size, PROT_READ
    );
#endif
    return rc;
}

/**
 * Create and attach to a shared-memory segment.
 */
static pmix_status_t
shmem_segment_create_and_attach(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    const char *segment_name,
    size_t segment_size
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Pad given size to fill remaining space on the last page.
    const size_t real_segsize = segment_size + pad_amount_to_page(segment_size);
    // Find a hole in virtual memory that meets our size requirements.
    size_t base_addr = 0;
    rc = pmix_vmem_find_hole(
        VMEM_HOLE_BIGGEST, &base_addr, real_segsize
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s:%s found vmhole at address=0x%zx",
        __func__, segment_name, base_addr
    );
    // Find a unique path for the shared-memory backing file.
    const char *segment_path = get_shmem_backing_path(job, segment_name);
    if (PMIX_UNLIKELY(!segment_path)) {
        rc = PMIX_ERROR;
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: segment backing file path is %s (size=%zd B)",
        __func__, segment_path, real_segsize
    );
    // Get a handle to the appropriate shmem.
    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_by_id(job, shmem_id, &shmem);
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Create a shared-memory segment backing store at the given path.
    rc = pmix_shmem_segment_create(shmem, real_segsize, segment_path);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        goto out;
    }
    // Attach to the shared-memory segment.
    rc = shmem_attach(job, shmem_id, (uintptr_t)base_addr);
out:
    if (PMIX_SUCCESS == rc) {
        // I created it, so I must release it.
        pmix_gds_shmem_set_status(
            job, shmem_id, PMIX_GDS_SHMEM_RELEASE
        );
    }
    return rc;
}

static pmix_status_t
module_init(
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_GDS_SHMEM_VOUT_HERE();
    PMIX_CONSTRUCT(&pmix_mca_gds_shmem_component.jobs, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_mca_gds_shmem_component.sessions, pmix_list_t);
    return PMIX_SUCCESS;
}

static void
module_finalize(void)
{
    PMIX_GDS_SHMEM_VOUT_HERE();
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_shmem_component.jobs);
    // Note to developers: the contents of pmix_mca_gds_shmem_component.sessions
    // point to elements in shared-memory, so no need to destruct here since
    // job_destruct took care of it.
}

static pmix_status_t
assign_module(
    pmix_info_t *info,
    size_t ninfo,
    int *priority
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    static const int max_priority = 100;
    *priority = PMIX_GDS_SHMEM_DEFAULT_PRIORITY;
    // The incoming info always overrides anything in the
    // environment as it is set by the application itself.
    bool specified = false;
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GDS_MODULE)) {
            char **options = NULL;
            specified = true; // They specified who they want.
            options = PMIx_Argv_split(info[n].value.data.string, ',');
            for (size_t m = 0; NULL != options[m]; m++) {
                if (0 == strcmp(options[m], PMIX_GDS_SHMEM_NAME)) {
                    // They specifically asked for us.
                    *priority = max_priority;
                    break;
                }
            }
            PMIx_Argv_free(options);
            break;
        }
    }
#if (PMIX_GDS_SHMEM_DISABLE == 1)
    if (true) {
        *priority = 0;
        return PMIX_SUCCESS;
    }
#endif
    // If they don't want us, then disqualify ourselves.
    if (specified && *priority != max_priority) {
        *priority = 0;
    }
    return PMIX_SUCCESS;
}

static pmix_status_t
server_cache_job_info(
    struct pmix_namespace_t *ns,
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_HIDE_UNUSED_PARAMS(ns, info, ninfo);
    PMIX_GDS_SHMEM_VOUT_HERE();
    // We don't support this operation.
    return PMIX_ERR_NOT_SUPPORTED;
}

/**
 *
 */
static pmix_status_t
prepare_shmem_store_for_local_job_data(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_local_job_stats_t *stats
) {
    pmix_status_t rc = PMIX_SUCCESS;
    static const float fluff = 2.5;
    const size_t kvsize = (sizeof(pmix_kval_t) + sizeof(pmix_value_t));
    // Initial hash table size.
    const size_t htsize = stats->hash_table_size;
    // Calculate a rough estimate on the amount of storage required to store the
    // values associated with the pmix_gds_shmem_shared_job_data_t. Err on the
    // side of overestimation.
    size_t seg_size = sizeof(*job->smdata) +
                      sizeof(job->smdata->tma) +
                      sizeof(*job->smdata->session) +
                      sizeof(*job->smdata->jobinfo) +
                      sizeof(*job->smdata->nodeinfo) +
                      sizeof(*job->smdata->appinfo) +
                      sizeof(*job->smdata->local_hashtab);
    // We need to store a hash table in the shared-memory segment, so calculate
    // a rough estimate on the memory required for its storage.
    seg_size += sizeof(pmix_hash_table_t);
    seg_size += htsize * pmix_hash_table_sizeof_hash_element();
    // Add a little extra to compensate for the value storage requirements. Here
    // we add an additional storage space for each entry.
    seg_size += htsize * kvsize;
    // Finally add the data size contribution, plus a little extra.
    seg_size += stats->packed_size;
    // Include some extra fluff that empirically seems reasonable.
    seg_size *= fluff;
    // Adjust (increase or decrease) segment size by the given parameter size.
    seg_size *= pmix_gds_shmem_segment_size_multiplier;
    // Create and attach to the shared-memory segment associated with this job.
    // This will be the backing store for metadata associated with static,
    // read-only data shared between the server and its clients.
    rc = shmem_segment_create_and_attach(
        job, PMIX_GDS_SHMEM_JOB_ID, "jobdata", seg_size
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = job_smdata_construct(job, htsize);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static inline pmix_status_t
pack_shmem_connection_info(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    pmix_peer_t *peer,
    pmix_buffer_t *buffer
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VVOUT(
        "%s:%s for peer (ID=%d) namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        peer->info->peerid, job->nspace_id
    );

    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_by_id(
        job, shmem_id, &shmem
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_kval_t kv;
    do {
        // Pack the namespace name.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM_SEG_NSID_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        kv.value->type = PMIX_STRING;
        kv.value->data.string = strdup(job->nspace_id);
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kv);
        // Pack the shmem ID as string.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM_SEG_SMID_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        kv.value->type = PMIX_STRING;
        int nw = asprintf(&kv.value->data.string, "%zd", (size_t)shmem_id);
        if (PMIX_UNLIKELY(nw == -1)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kv);
        // Pack the backing file path.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM_SEG_PATH_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        kv.value->type = PMIX_STRING;
        kv.value->data.string = strdup(shmem->backing_path);
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kv);
        // Pack attach size to shared-memory segment.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM_SEG_SIZE_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        kv.value->type = PMIX_STRING;
        nw = asprintf(&kv.value->data.string, "%zx", shmem->size);
        if (PMIX_UNLIKELY(nw == -1)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kv);
        // Pack the base address for attaching to shared-memory segment.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM_SEG_ADDR_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        kv.value->type = PMIX_STRING;
        nw = asprintf(
            &kv.value->data.string, "%zx", (size_t)shmem->base_address
        );
        if (PMIX_UNLIKELY(nw == -1)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
    } while (false);
    PMIX_DESTRUCT(&kv);

    return rc;
}

/**
 * Emits the contents of an pmix_gds_shmem_unpacked_seg_blob_t.
 */
static inline void
vout_unpacked_seg_blob(
    pmix_gds_shmem_unpacked_seg_blob_t *usb,
    const char *called_by
) {
#if (PMIX_ENABLE_DEBUG == 0)
    PMIX_HIDE_UNUSED_PARAMS(usb, called_by);
#endif
    PMIX_GDS_SHMEM_VVOUT(
        "%s: "
        SHMEM_SEG_NSID_KEY "=%s "
        SHMEM_SEG_SMID_KEY "=%u "
        SHMEM_SEG_PATH_KEY "=%s "
        SHMEM_SEG_SIZE_KEY "=%zd "
        SHMEM_SEG_ADDR_KEY "=0x%zx",
        called_by, usb->nsid, (unsigned)usb->smid,
        usb->seg_path, usb->seg_size, usb->seg_addr
    );
}

/**
 * Sets shared-memory connection information from a pmix_kval_t by unpacking the
 * blob and saving the values for the caller. If successful, returns relevant
 * data associated with the unpacked data.
 */
static inline pmix_status_t
unpack_shmem_connection_info(
    pmix_kval_t *kvbo,
    pmix_gds_shmem_unpacked_seg_blob_t *usb
) {
    pmix_status_t rc = PMIX_SUCCESS;

    // Make sure this is the expected type.
    if (PMIX_UNLIKELY(PMIX_BYTE_OBJECT != kvbo->value->type)) {
        rc = PMIX_ERR_TYPE_MISMATCH;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_buffer_t buffer;
    PMIX_CONSTRUCT(&buffer, pmix_buffer_t);

    PMIX_LOAD_BUFFER(
        pmix_client_globals.myserver,
        &buffer,
        kvbo->value->data.bo.bytes,
        kvbo->value->data.bo.size
    );

    pmix_kval_t kv;
    while (true) {
        PMIX_CONSTRUCT(&kv, pmix_kval_t);

        int32_t count = 1;
        PMIX_BFROPS_UNPACK(
            rc, pmix_client_globals.myserver,
            &buffer, &kv, &count, PMIX_KVAL
        );
        if (PMIX_SUCCESS != rc) {
            break;
        }

        const char *const val = kv.value->data.string;
        if (PMIX_CHECK_KEY(&kv, SHMEM_SEG_NSID_KEY)) {
            int nw = asprintf(&usb->nsid, "%s", val);
            if (PMIX_UNLIKELY(nw == -1)) {
                rc = PMIX_ERR_NOMEM;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM_SEG_SMID_KEY)) {
            size_t st_shmem_id;
            rc = strtost(val, 10, &st_shmem_id);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
            usb->smid = (pmix_gds_shmem_job_shmem_id_t)st_shmem_id;
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM_SEG_PATH_KEY)) {
            int nw = asprintf(&usb->seg_path, "%s", val);
            if (PMIX_UNLIKELY(nw == -1)) {
                rc = PMIX_ERR_NOMEM;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM_SEG_SIZE_KEY)) {
            rc = strtost(val, 16, &usb->seg_size);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM_SEG_ADDR_KEY)) {
            rc = strtost(val, 16, &usb->seg_addr);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else {
            rc = PMIX_ERR_BAD_PARAM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        // Done with this one.
        PMIX_DESTRUCT(&kv);
    }
    // Catch last kval.
    PMIX_DESTRUCT(&kv);
    PMIX_DESTRUCT(&buffer);

    if (PMIX_UNLIKELY(PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc)) {
        rc = PMIX_ERR_UNPACK_FAILURE;
        PMIX_ERROR_LOG(rc);
    }
    else {
        vout_unpacked_seg_blob(usb, __func__);
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/**
 * Fetches a complete copy of the job-level information.
 */
static pmix_status_t
fetch_local_job_data(
    pmix_namespace_t *ns,
    pmix_cb_t *job_cb
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_proc_t wildcard;
    PMIX_LOAD_PROCID(&wildcard, ns->nspace, PMIX_RANK_WILDCARD);

    job_cb->key = NULL;
    job_cb->proc = &wildcard;
    job_cb->copy = true;
    job_cb->scope = PMIX_LOCAL;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, job_cb);

    return rc;
}

/**
 * Internally the hash table can do some interesting sizing calculations, so we
 * just construct a temporary one with the number of expected elements, then
 * query it for its actual capacity.
 */
static inline size_t
get_actual_hashtab_capacity(
    size_t num_elements
) {
    pmix_hash_table_t tmp;
    PMIX_CONSTRUCT(&tmp, pmix_hash_table_t);
    pmix_hash_table_init(&tmp, num_elements);
    // Grab the actual capacity.
    const size_t result = tmp.ht_capacity;
    PMIX_DESTRUCT(&tmp);

    return result;
}

static inline pmix_status_t
get_local_job_data_stats(
    pmix_peer_t *peer,
    pmix_cb_t *job_cb,
    pmix_gds_shmem_local_job_stats_t *stats
) {
    pmix_status_t rc = PMIX_SUCCESS;
    size_t nhtentries = 0;

    memset(stats, 0, sizeof(*stats));

    pmix_buffer_t data;
    PMIX_CONSTRUCT(&data, pmix_buffer_t);

    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, &job_cb->kvs, pmix_kval_t) {
        // Calculate some statistics so we can make an educated estimate on the
        // size of structures we need for our backing store.
        if (PMIX_DATA_ARRAY == kvi->value->type) {
            // PMIX_PROC_DATA is stored in the hash table.
            if (PMIX_CHECK_KEY(kvi, PMIX_PROC_DATA)) {
                nhtentries += kvi->value->data.darray->size;
            }
        }
        // Just a key/value pair, so they will likely go into the hash table.
        else {
            nhtentries += 1;
        }

        PMIX_BFROPS_PACK(rc, peer, &data, kvi, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            goto out;
        }
    }
    stats->packed_size = data.bytes_used;
    stats->hash_table_size = get_actual_hashtab_capacity(nhtentries);
out:
    PMIX_DESTRUCT(&data);
    return rc;
}

static inline pmix_status_t
pack_shmem_seg_blob(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    struct pmix_peer_t *peer,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;

    // Only pack connection info that is ready for use. Otherwise,
    // it's bogus data that we shouldn't be sharing it with our clients.
    const bool ready_for_use = pmix_gds_shmem_has_status(
        job, shmem_id, PMIX_GDS_SHMEM_READY_FOR_USE
    );
    if (!ready_for_use) {
        return rc;
    }

    pmix_buffer_t buff;
    do {
        PMIX_CONSTRUCT(&buff, pmix_buffer_t);

        rc = pack_shmem_connection_info(
            job, shmem_id, peer, &buff
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }

        pmix_value_t blob = {
            .type = PMIX_BYTE_OBJECT
        };
        pmix_kval_t kv = {
            .key = SHMEM_SEG_BLOB_KEY,
            .value = &blob
        };

        PMIX_UNLOAD_BUFFER(&buff, blob.data.bo.bytes, blob.data.bo.size);
        PMIX_BFROPS_PACK(rc, peer, reply, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_VALUE_DESTRUCT(&blob);
    } while (false);
    PMIX_DESTRUCT(&buff);

    return rc;
}

static pmix_status_t
publish_shmem_connection_info(
    pmix_gds_shmem_job_t *job,
    pmix_peer_t *peer,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_namespace_t *ns = peer->nptr;

    // Pack the payload for delivery. Note that the message we are going to send
    // is simply the shared memory connection information that is shared among
    // clients on a single node.

    // Start with the namespace name.
    PMIX_BFROPS_PACK(rc, peer, reply, &ns->nspace, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Pack the shared-memory segment information.
    rc = pack_shmem_seg_blob(
        job, PMIX_GDS_SHMEM_JOB_ID, peer, reply
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // If we have more than one local client for this nspace,
    // save this packed object so we don't do this again.
    if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) ||
        ns->nlocalprocs > 1) {
        PMIX_RETAIN(reply);
        ns->jobbkt = reply;
    }
    return rc;
}

static pmix_status_t
server_register_new_job_info(
    pmix_peer_t *peer,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_namespace_t *const ns = peer->nptr;

    // Setup a new job tracker for this peer's nspace.
    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(ns->nspace, true, &job);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Ask for a complete copy of the job-level information.
    pmix_cb_t job_cb;
    PMIX_CONSTRUCT(&job_cb, pmix_cb_t);

    rc = fetch_local_job_data(ns, &job_cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Pack the data so we can see how large it is. This will help inform how
    // large to make the shared-memory segment associated with these data.
    pmix_gds_shmem_local_job_stats_t stats;
    rc = get_local_job_data_stats(peer, &job_cb, &stats);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Get the shared-memory segment ready for job data.
    rc = prepare_shmem_store_for_local_job_data(job, &stats);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Store fetched data into a shared-memory segment.
    rc = pmix_gds_shmem_store_local_job_data_in_shmem(job, &job_cb.kvs);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // You guessed it, publish shared-memory connection info.
    rc = publish_shmem_connection_info(job, peer, reply);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
    }
out:
    PMIX_DESTRUCT(&job_cb);
    return rc;
}

/**
 *
 */
static pmix_status_t
server_register_job_info(
    struct pmix_peer_t *peer_struct,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_peer_t *const peer = (pmix_peer_t *)peer_struct;
    pmix_namespace_t *const ns = peer->nptr;

    if (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        // This function is only available on servers.
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    PMIX_GDS_SHMEM_VOUT(
        "%s: %s for peer %s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        PMIX_PEER_PRINT(peer)
    );
    // First see if we already have processed this data for another
    // peer in this nspace so we don't waste time doing it again.
    if (NULL != ns->jobbkt) {
        PMIX_GDS_SHMEM_VOUT(
            "[%s:%d] copying prepacked payload",
            pmix_globals.myid.nspace,
            pmix_globals.myid.rank
        );
        // We have packed this before, so we can just deliver it.
        PMIX_BFROPS_COPY_PAYLOAD(rc, peer, reply, ns->jobbkt);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        // Now see if we have delivered it to
        // all our local clients for this nspace.
        if (!PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) &&
            ns->ndelivered == ns->nlocalprocs) {
            // We have, so let's get rid of the packed copy of the data.
            PMIX_RELEASE(ns->jobbkt);
            ns->jobbkt = NULL;
        }
        return rc;
    }
    // Else we need to actually store and register the job info.
    PMIX_GDS_SHMEM_VOUT(
        "[%s:%d] no cached payload. Registering a new one.",
        pmix_globals.myid.nspace, pmix_globals.myid.rank
    );
    return server_register_new_job_info(peer, reply);
}

static pmix_status_t
unpack_shmem_seg_blob_and_attach_if_necessary(
    pmix_kval_t *kvbo
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_gds_shmem_unpacked_seg_blob_t usb;
    PMIX_CONSTRUCT(&usb, pmix_gds_shmem_unpacked_seg_blob_t);
    do {
        rc = unpack_shmem_connection_info(kvbo, &usb);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        // Get the associated job tracker.
        pmix_gds_shmem_job_t *job;
        rc = pmix_gds_shmem_get_job_tracker(usb.nsid, true, &job);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        // Make sure we aren't already attached to the given shmem.
        if (pmix_gds_shmem_has_status(job, usb.smid, PMIX_GDS_SHMEM_ATTACHED)) {
            break;
        }
        // Looks like we have to attach and initialize it.
        rc = shmem_segment_attach_and_init(job, &usb);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    } while (false);
    PMIX_DESTRUCT(&usb);

    return rc;
}

static pmix_status_t
store_job_info(
    const char *nspace,
    pmix_buffer_t *buff
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT(
        "%s:%s for namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid), nspace
    );

    pmix_kval_t kval;
    while (true) {
        PMIX_CONSTRUCT(&kval, pmix_kval_t);

        int32_t nvals = 1;
        PMIX_BFROPS_UNPACK(
            rc, pmix_client_globals.myserver,
            buff, &kval, &nvals , PMIX_KVAL
        );
        if (PMIX_SUCCESS != rc) {
            break;
        }

        if (PMIX_CHECK_KEY(&kval, SHMEM_SEG_BLOB_KEY)) {
            rc = unpack_shmem_seg_blob_and_attach_if_necessary(&kval);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kval, PMIX_SESSION_INFO_ARRAY) ||
                 PMIX_CHECK_KEY(&kval, PMIX_NODE_INFO_ARRAY) ||
                 PMIX_CHECK_KEY(&kval, PMIX_APP_INFO_ARRAY)) {
            PMIX_GDS_SHMEM_VVOUT("%s:skipping type=%s", __func__, kval.key);
        }
        else {
            PMIX_GDS_SHMEM_VOUT(
                "%s:ERROR unexpected key=%s", __func__, kval.key
            );
            rc = PMIX_ERR_BAD_PARAM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kval);
    };
    // Release the leftover kval.
    PMIX_DESTRUCT(&kval);

    if (PMIX_UNLIKELY(PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc)) {
        rc = PMIX_ERR_UNPACK_FAILURE;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    else {
        rc = PMIX_SUCCESS;
    }
    // Done. Before this point the server should have populated the
    // shared-memory segment with the relevant data.
    return rc;
}

/**
 * This function is only called by the PMIx server when its host has received
 * data from some other peer. It therefore always contains data solely from
 * remote procs, and we shall store it accordingly.
 */
static pmix_status_t
server_store_modex(
    struct pmix_namespace_t *ns,
    pmix_buffer_t *buff,
    void *cbdata
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT_HERE();

    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(
        ((pmix_namespace_t *)(ns))->nspace, false, &job
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    const bool attached = pmix_gds_shmem_has_status(
        job, PMIX_GDS_SHMEM_MODEX_ID, PMIX_GDS_SHMEM_ATTACHED
    );
    if (!attached) {
        static const float fluff = 2.5;
        // TODO(skg) Improve estimate.
        const size_t npeers = job->nspace->nprocs;
        // TODO(skg) We need to calculate this somehow.
        const size_t htsize = 256 * npeers;
        // Estimated size required to store the unpacked modex data.
        size_t seg_size = buff->bytes_used * npeers;
        seg_size += sizeof(pmix_hash_table_t);
        seg_size += htsize * pmix_hash_table_sizeof_hash_element();
        // Include some extra fluff that empirically seems reasonable.
        seg_size *= fluff;
        // Adjust (increase or decrease) segment size by the given parameter size.
        seg_size *= pmix_gds_shmem_segment_size_multiplier;
        // Create and attach to the shared-memory segment that will back these data.
        rc = shmem_segment_create_and_attach(
            job, PMIX_GDS_SHMEM_MODEX_ID, "modexdata", seg_size
        );
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }

        rc = modex_smdata_construct(job, htsize);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    return pmix_gds_base_store_modex(
        ns, buff, NULL, pmix_gds_shmem_store_modex_in_shmem, cbdata
    );
}

static pmix_status_t
server_setup_fork(
    const pmix_proc_t *peer,
    char ***env
) {
    PMIX_HIDE_UNUSED_PARAMS(peer, env);
    PMIX_GDS_SHMEM_VOUT_HERE();
    // Nothing to do here.
    return PMIX_SUCCESS;
}

static pmix_status_t
server_add_nspace(
    const char *nspace,
    uint32_t nlocalprocs,
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_HIDE_UNUSED_PARAMS(nspace, nlocalprocs, info, ninfo);
    PMIX_GDS_SHMEM_VOUT_HERE();
    // Nothing to do here.
    return PMIX_SUCCESS;
}

static pmix_status_t
del_nspace(
    const char *nspace
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    pmix_gds_shmem_job_t *ji;
    pmix_gds_shmem_component_t *const component = &pmix_mca_gds_shmem_component;
    PMIX_LIST_FOREACH (ji, &component->jobs, pmix_gds_shmem_job_t) {
        if (0 == strcmp(nspace, ji->nspace_id)) {
            pmix_list_remove_item(&component->jobs, &ji->super);
            PMIX_RELEASE(ji);
            break;
        }
    }
    return PMIX_SUCCESS;
}

static pmix_status_t
server_mark_modex_complete(
    struct pmix_peer_t *peer,
    pmix_list_t *nslist,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Pack connection info for each ns in nslist.
    pmix_nspace_caddy_t *nsi;
    PMIX_LIST_FOREACH (nsi, nslist, pmix_nspace_caddy_t) {
        // false here because we should already know about the nspace.
        pmix_gds_shmem_job_t *job;
        rc = pmix_gds_shmem_get_job_tracker(
            nsi->ns->nspace, false, &job
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        rc = pack_shmem_seg_blob(
            job, PMIX_GDS_SHMEM_JOB_ID, peer, reply
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        rc = pack_shmem_seg_blob(
            job, PMIX_GDS_SHMEM_MODEX_ID, peer, reply
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    }
    return rc;
}

static pmix_status_t
client_recv_modex_complete(
    pmix_buffer_t *buff
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_kval_t kval;
    while (true) {
        PMIX_CONSTRUCT(&kval, pmix_kval_t);

        int32_t nvals = 1;
        PMIX_BFROPS_UNPACK(
            rc, pmix_client_globals.myserver,
            buff, &kval, &nvals, PMIX_KVAL
        );
        if (PMIX_SUCCESS != rc) {
            break;
        }

        if (PMIX_CHECK_KEY(&kval, SHMEM_SEG_BLOB_KEY)) {
            rc = unpack_shmem_seg_blob_and_attach_if_necessary(&kval);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else {
            PMIX_GDS_SHMEM_VOUT(
                "%s:ERROR unexpected key=%s", __func__, kval.key
            );
            rc = PMIX_ERR_BAD_PARAM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kval);
    };
    // Release the leftover kval.
    PMIX_DESTRUCT(&kval);

    if (PMIX_UNLIKELY(PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc)) {
        rc = PMIX_ERR_UNPACK_FAILURE;
        PMIX_ERROR_LOG(rc);
    }
    else {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

static void set_size(struct pmix_namespace_t *ns, size_t memsize)
{
    PMIX_HIDE_UNUSED_PARAMS(ns, memsize);
    return;
}

pmix_gds_base_module_t pmix_shmem_module = {
    .name = PMIX_GDS_SHMEM_NAME,
    .is_tsafe = false,
    .init = module_init,
    .finalize = module_finalize,
    .assign_module = assign_module,
    .cache_job_info = server_cache_job_info,
    .register_job_info = server_register_job_info,
    .store_job_info = store_job_info,
    .store = NULL,
    .store_modex = server_store_modex,
    .fetch = pmix_gds_shmem_fetch,
    .setup_fork = server_setup_fork,
    .add_nspace = server_add_nspace,
    .del_nspace = del_nspace,
    .assemb_kvs_req = NULL,
    .accept_kvs_resp = NULL,
    .mark_modex_complete = server_mark_modex_complete,
    .recv_modex_complete = client_recv_modex_complete,
    .set_size = set_size
};

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
