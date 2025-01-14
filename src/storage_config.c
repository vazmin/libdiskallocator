/*
 * Copyright (c) 2020 YuQing <384681@qq.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "fastcommon/ini_file_reader.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/system_info.h"
#include "sf/sf_global.h"
#include "global.h"
#include "store_path_index.h"
#include "storage_config.h"

static int load_one_path(DAStorageConfig *storage_cfg,
        IniFullContext *ini_ctx, string_t *path)
{
    int result;
    char tmp_filename[PATH_MAX];
    char full_path[PATH_MAX];
    char *path_str;

    path_str = iniGetStrValue(ini_ctx->section_name,
            "path", ini_ctx->context);
    if (path_str == NULL) {
        path_str = DA_DATA_PATH_STR;
    } else if (*path_str == '\0') {
        logError("file: "__FILE__", line: %d, "
                "config file: %s, section: %s, item: path is empty",
                __LINE__, ini_ctx->filename, ini_ctx->section_name);
        return ENOENT;
    } else {
        if (*path_str != '/') {
            snprintf(tmp_filename, sizeof(tmp_filename),
                    "%s/dummy.tmp", DA_DATA_PATH_STR);
            resolve_path(tmp_filename, path_str,
                    full_path, sizeof(full_path));
            path_str = full_path;
        }
    }

    if (access(path_str, F_OK) == 0) {
        if (!isDir(path_str)) {
            logError("file: "__FILE__", line: %d, "
                    "config file: %s, section: %s, item: path, "
                    "%s is NOT a path", __LINE__, ini_ctx->filename,
                    ini_ctx->section_name, path_str);
            return EINVAL;
        }
    } else {
        result = errno != 0 ? errno : EPERM;
        if (result != ENOENT) {
            logError("file: "__FILE__", line: %d, "
                    "config file: %s, section: %s, access path %s fail, "
                    "errno: %d, error info: %s", __LINE__, ini_ctx->filename,
                    ini_ctx->section_name, path_str, result, STRERROR(result));
            return result;
        }

        if (mkdir(path_str, 0775) != 0) {
            result = errno != 0 ? errno : EPERM;
            logError("file: "__FILE__", line: %d, "
                    "mkdir %s fail, errno: %d, error info: %s",
                    __LINE__, path_str, result, STRERROR(result));
            return result;
        }
        
        SF_CHOWN_RETURN_ON_ERROR(path_str, geteuid(), getegid());
    }

    chopPath(path_str);
    path->len = strlen(path_str);
    path->str = (char *)fc_malloc(path->len + 1);
    if (path->str == NULL) {
        return ENOMEM;
    }

    memcpy(path->str, path_str, path->len + 1);
    return 0;
}

static int storage_config_calc_path_spaces(DAStoragePathInfo *path_info)
{
    struct statvfs sbuf;

    if (statvfs(path_info->store.path.str, &sbuf) != 0) {
        logError("file: "__FILE__", line: %d, "
                "statvfs path %s fail, errno: %d, error info: %s.",
                __LINE__, path_info->store.path.str, errno, STRERROR(errno));
        return errno != 0 ? errno : EPERM;
    }

    path_info->space_stat.total = (int64_t)(sbuf.f_blocks) * sbuf.f_frsize;
    path_info->space_stat.avail = (int64_t)(sbuf.f_bavail) * sbuf.f_frsize;
    path_info->reserved_space.value = path_info->space_stat.total *
        path_info->reserved_space.ratio;
    path_info->prealloc_space.value = path_info->space_stat.total *
        path_info->prealloc_space.ratio;
    path_info->prealloc_space.trunk_count = (path_info->prealloc_space.
            value + DA_STORE_CFG.trunk_file_size - 1) / (int64_t)
        DA_STORE_CFG.trunk_file_size;
    if (sbuf.f_blocks > 0) {
        path_info->space_stat.used_ratio = (double)(sbuf.f_blocks -
                sbuf.f_bavail) / (double)sbuf.f_blocks;
    }

    /*
    logInfo("used ratio: %.2f%%, prealloc_space.trunk_count: %d",
            100 * path_info->space_stat.used_ratio,
            path_info->prealloc_space.trunk_count);
            */

    __sync_bool_compare_and_swap(&path_info->space_stat.
            last_stat_time, 0, g_current_time);
    return 0;
}

int storage_config_calc_path_avail_space(DAStoragePathInfo *path_info)
{
    struct statvfs sbuf;
    time_t last_stat_time;

    last_stat_time = __sync_add_and_fetch(&path_info->
            space_stat.last_stat_time, 0);
    if (last_stat_time == g_current_time) {
        return 0;
    }
    __sync_bool_compare_and_swap(&path_info->space_stat.
            last_stat_time, last_stat_time, g_current_time);

    if (statvfs(path_info->store.path.str, &sbuf) != 0) {
        logError("file: "__FILE__", line: %d, "
                "statvfs path %s fail, errno: %d, error info: %s.",
                __LINE__, path_info->store.path.str, errno, STRERROR(errno));
        return errno != 0 ? errno : EPERM;
    }

    path_info->space_stat.avail = (int64_t)(sbuf.f_bavail) * sbuf.f_frsize;
    if (sbuf.f_blocks > 0) {
        path_info->space_stat.used_ratio = (double)(sbuf.f_blocks -
                sbuf.f_bavail) / (double)sbuf.f_blocks;
    }

    return 0;
}

void storage_config_stat_path_spaces(SFSpaceStat *ss)
{
    DAStoragePathInfo **pp;
    DAStoragePathInfo **end;
    SFSpaceStat stat;
    int64_t disk_avail;

    stat.total = stat.used = stat.avail = 0;
    end = DA_STORE_CFG.paths_by_index.paths + DA_STORE_CFG.paths_by_index.count;
    for (pp=DA_STORE_CFG.paths_by_index.paths; pp<end; pp++) {
        if (*pp == NULL) {
            continue;
        }

        storage_config_calc_path_avail_space(*pp);
        disk_avail = (*pp)->space_stat.avail  - (*pp)->reserved_space.value;
        if (disk_avail < 0) {
            disk_avail = 0;
        }
        stat.total += (*pp)->trunk_stat.total + disk_avail;
        stat.avail += (*pp)->trunk_stat.avail + disk_avail;
        stat.used += (*pp)->trunk_stat.used;

        /*
        logInfo("trunk {total: %"PRId64" MB, avail: %"PRId64" MB, "
                "used: %"PRId64" MB, reserved: %"PRId64" MB}, "
                "disk_avail: %"PRId64" MB, sum {total: %"PRId64" MB, "
                "avail: %"PRId64" MB, used: %"PRId64" MB}",
                (*pp)->trunk_stat.total / (1024 * 1024),
                (*pp)->trunk_stat.avail / (1024 * 1024),
                (*pp)->trunk_stat.used / (1024 * 1024),
                (*pp)->reserved_space.value / (1024 * 1024),
                disk_avail / (1024 * 1024), stat.total / (1024 * 1024),
                stat.avail / (1024 * 1024), stat.used / (1024 * 1024));
                */
    }

    *ss = stat;
}

static int load_paths(DAStorageConfig *storage_cfg, IniFullContext *ini_ctx,
        const char *section_name_prefix, const char *item_name,
        DAStoragePathArray *parray, const bool required)
{
    int result;
    int count;
    int bytes;
    int i;
    char section_name[64];

    count = iniGetIntValue(NULL, item_name, ini_ctx->context, 0);
    if (count <= 0) {
        if (required) {
            logError("file: "__FILE__", line: %d, "
                    "config file: %s, item \"%s\" "
                    "not exist or invalid", __LINE__,
                    ini_ctx->filename, item_name);
            return ENOENT;
        } else {
            parray->count = 0;
            return 0;
        }
    }

    bytes = sizeof(DAStoragePathInfo) * count;
    parray->paths = (DAStoragePathInfo *)fc_malloc(bytes);
    if (parray->paths == NULL) {
        return ENOMEM;
    }
    memset(parray->paths, 0, bytes);

    ini_ctx->section_name = section_name;
    for (i=0; i<count; i++) {
        sprintf(section_name, "%s-%d", section_name_prefix, i + 1);
        if ((result=load_one_path(storage_cfg, ini_ctx,
                        &parray->paths[i].store.path)) != 0)
        {
            return result;
        }

        parray->paths[i].write_thread_count = iniGetIntValue(section_name,
                "write_threads", ini_ctx->context, storage_cfg->
                write_threads_per_path);
        if (parray->paths[i].write_thread_count <= 0) {
            parray->paths[i].write_thread_count = 1;
        }

        parray->paths[i].read_thread_count = iniGetIntValue(section_name,
                "read_threads", ini_ctx->context, storage_cfg->
                read_threads_per_path);
        if (parray->paths[i].read_thread_count <= 0) {
            parray->paths[i].read_thread_count = 1;
        }

        parray->paths[i].read_io_depth = iniGetIntValue(section_name,
                "read_io_depth", ini_ctx->context, storage_cfg->
                io_depth_per_read_thread);
        if (parray->paths[i].read_io_depth <= 0) {
            parray->paths[i].read_io_depth = 64;
        }

        if ((result=iniGetPercentValue(ini_ctx, "prealloc_space",
                        &parray->paths[i].prealloc_space.ratio,
                        storage_cfg->prealloc_space.ratio_per_path)) != 0)
        {
            return result;
        }

        if ((result=iniGetPercentValue(ini_ctx, "reserved_space",
                        &parray->paths[i].reserved_space.ratio,
                        storage_cfg->reserved_space_per_disk)) != 0)
        {
            return result;
        }

        if ((result=storage_config_calc_path_spaces(
                        parray->paths + i)) != 0)
        {
            return result;
        }
    }

    parray->count = count;
    return 0;
}

#ifdef OS_LINUX
static int load_aio_read_buffer_params(DAStorageConfig *storage_cfg,
        IniFullContext *ini_ctx)
{
    int result;
    int64_t total_memory;

    ini_ctx->section_name = "aio-read-buffer";
    if ((result=iniGetPercentValue(ini_ctx, "memory_watermark_low",
                    &storage_cfg->aio_read_buffer.memory_watermark_low.
                    ratio, 0.01)) != 0)
    {
        return result;
    }

    if ((result=iniGetPercentValue(ini_ctx, "memory_watermark_high",
                    &storage_cfg->aio_read_buffer.memory_watermark_high.
                    ratio, 0.10)) != 0)
    {
        return result;
    }

    if ((result=get_sys_total_mem_size(&total_memory)) != 0) {
        return result;
    }

    storage_cfg->aio_read_buffer.memory_watermark_low.value =
        (int64_t)(total_memory * storage_cfg->aio_read_buffer.
                memory_watermark_low.ratio);
    storage_cfg->aio_read_buffer.memory_watermark_high.value =
        (int64_t)(total_memory * storage_cfg->aio_read_buffer.
                memory_watermark_high.ratio);

    storage_cfg->aio_read_buffer.max_idle_time = iniGetIntValue(
            ini_ctx->section_name, "max_idle_time",
            ini_ctx->context, 300);
    if (storage_cfg->aio_read_buffer.max_idle_time <= 0) {
        storage_cfg->aio_read_buffer.max_idle_time = 300;
    }

    storage_cfg->aio_read_buffer.reclaim_interval = iniGetIntValue(
            ini_ctx->section_name, "reclaim_interval",
            ini_ctx->context, 60);
    if (storage_cfg->aio_read_buffer.reclaim_interval <= 0) {
        storage_cfg->aio_read_buffer.reclaim_interval = 60;
    }

    return 0;
}
#endif

static int load_global_items(DAStorageConfig *storage_cfg,
        IniFullContext *ini_ctx)
{
    int result;

    storage_cfg->fd_cache_capacity_per_read_thread = iniGetIntValue(NULL,
            "fd_cache_capacity_per_read_thread", ini_ctx->context, 256);
    if (storage_cfg->fd_cache_capacity_per_read_thread <= 0) {
        storage_cfg->fd_cache_capacity_per_read_thread = 256;
    }

    storage_cfg->fd_cache_capacity_per_write_thread = iniGetIntValue(NULL,
            "fd_cache_capacity_per_write_thread", ini_ctx->context, 256);
    if (storage_cfg->fd_cache_capacity_per_write_thread <= 0) {
        storage_cfg->fd_cache_capacity_per_write_thread = 256;
    }

    storage_cfg->write_threads_per_path = iniGetIntValue(NULL,
            "write_threads_per_path", ini_ctx->context, 1);
    if (storage_cfg->write_threads_per_path <= 0) {
        storage_cfg->write_threads_per_path = 1;
    }

    storage_cfg->read_threads_per_path = iniGetIntValue(NULL,
            "read_threads_per_path", ini_ctx->context, 1);
    if (storage_cfg->read_threads_per_path <= 0) {
        storage_cfg->read_threads_per_path = 1;
    }

    storage_cfg->io_depth_per_read_thread = iniGetIntValue(NULL,
            "io_depth_per_read_thread", ini_ctx->context, 64);
    if (storage_cfg->io_depth_per_read_thread <= 0) {
        storage_cfg->io_depth_per_read_thread = 64;
    }

    if ((result=iniGetPercentValue(ini_ctx, "prealloc_space_per_path",
                    &storage_cfg->prealloc_space.ratio_per_path, 0.05)) != 0)
    {
        return result;
    }

    if ((result=get_time_item_from_conf(ini_ctx->context,
                    "prealloc_space_start_time", &storage_cfg->
                    prealloc_space.start_time, 0, 0)) != 0)
    {
        return result;
    }
    if ((result=get_time_item_from_conf(ini_ctx->context,
                    "prealloc_space_end_time", &storage_cfg->
                    prealloc_space.end_time, 0, 0)) != 0)
    {
        return result;
    }

    storage_cfg->trunk_prealloc_threads = iniGetIntValue(NULL,
            "trunk_prealloc_threads", ini_ctx->context, 1);
    if (storage_cfg->trunk_prealloc_threads <= 0) {
        storage_cfg->trunk_prealloc_threads = 1;
    }

    storage_cfg->max_trunk_files_per_subdir = iniGetIntValue(NULL,
            "max_trunk_files_per_subdir", ini_ctx->context, 100);
    if (storage_cfg->max_trunk_files_per_subdir <= 0) {
        storage_cfg->max_trunk_files_per_subdir = 100;
    }

    storage_cfg->trunk_file_size = iniGetInt64CorrectValue(ini_ctx,
            "trunk_file_size", DA_DEFAULT_TRUNK_FILE_SIZE,
            DA_TRUNK_FILE_MIN_SIZE, DA_TRUNK_FILE_MAX_SIZE);
    if (storage_cfg->trunk_file_size <= DA_FILE_BLOCK_SIZE) {
        logError("file: "__FILE__", line: %d, "
                "trunk_file_size: %u is too small, "
                "<= block size %d", __LINE__, storage_cfg->
                trunk_file_size, DA_FILE_BLOCK_SIZE);
        return EINVAL;
    }

    storage_cfg->discard_remain_space_size = iniGetInt64CorrectValue(
            ini_ctx, "discard_remain_space_size",
            DA_DEFAULT_DISCARD_REMAIN_SPACE_SIZE,
            DA_DISCARD_REMAIN_SPACE_MIN_SIZE,
            DA_DISCARD_REMAIN_SPACE_MAX_SIZE);

    if ((result=iniGetPercentValue(ini_ctx, "reserved_space_per_disk",
                    &storage_cfg->reserved_space_per_disk, 0.10)) != 0)
    {
        return result;
    }

    if ((result=iniGetPercentValue(ini_ctx, "write_cache_to_hd_on_usage",
                    &storage_cfg->write_cache_to_hd.on_usage, 1.00 -
                    storage_cfg->reserved_space_per_disk)) != 0)
    {
        return result;
    }

    if ((result=get_time_item_from_conf(ini_ctx->context,
                    "write_cache_to_hd_start_time", &storage_cfg->
                    write_cache_to_hd.start_time, 0, 0)) != 0)
    {
        return result;
    }
    if ((result=get_time_item_from_conf(ini_ctx->context,
                    "write_cache_to_hd_end_time", &storage_cfg->
                    write_cache_to_hd.end_time, 0, 0)) != 0)
    {
        return result;
    }

    if ((result=iniGetPercentValue(ini_ctx, "reclaim_trunks_on_path_usage",
                    &storage_cfg->reclaim_trunks_on_path_usage, 0.50)) != 0)
    {
        return result;
    }

    if ((result=iniGetPercentValue(ini_ctx, "never_reclaim_on_trunk_usage",
                    &storage_cfg->never_reclaim_on_trunk_usage, 0.90)) != 0)
    {
        return result;
    }

    return 0;
}

static int load_from_config_file(DAStorageConfig *storage_cfg,
        IniFullContext *ini_ctx)
{
    int result;
    if ((result=load_global_items(storage_cfg, ini_ctx)) != 0) {
        return result;
    }
  
#ifdef OS_LINUX
    if ((result=load_aio_read_buffer_params(storage_cfg, ini_ctx)) != 0) {
        return result;
    }
#endif

    if ((result=load_paths(storage_cfg, ini_ctx,
                    "store-path", "store_path_count",
                    &storage_cfg->store_path, true)) != 0)
    {
        return result;
    }

    if ((result=load_paths(storage_cfg, ini_ctx,
                    "write-cache-path", "write_cache_path_count",
                    &storage_cfg->write_cache, false)) != 0)
    {
        return result;
    }

    return 0;
}

static int load_path_indexes(DAStoragePathArray *parray, const char *caption,
        int *change_count)
{
    int result;
    bool regenerated;
    DAStoragePathInfo *p;
    DAStoragePathInfo *end;
    StorePathEntry *pentry;

    end = parray->paths + parray->count;
    for (p=parray->paths; p<end; p++) {
        pentry = store_path_index_get(p->store.path.str);
        if (pentry != NULL) {
            p->store.index = pentry->index;
            if ((result=store_path_check_mark(pentry, &regenerated)) != 0) {
                return result;
            }
            if (regenerated) {
                ++(*change_count);
            }
        } else {
            if ((result=store_path_index_add(p->store.path.str,
                            &p->store.index)) != 0)
            {
                return result;
            }
            ++(*change_count);
        }

#ifdef OS_LINUX
        if ((result=get_path_block_size(p->store.path.str,
                        &p->block_size)) != 0)
        {
            return result;
        }
#endif
    }

    return 0;
}

static void do_set_paths_by_index(DAStorageConfig *storage_cfg,
        DAStoragePathArray *parray)
{
    DAStoragePathInfo *p;
    DAStoragePathInfo *end;

    end = parray->paths + parray->count;
    for (p=parray->paths; p<end; p++) {
        storage_cfg->paths_by_index.paths[p->store.index] = p;
    }
}

static int set_paths_by_index(DAStorageConfig *storage_cfg)
{
    int bytes;

    storage_cfg->paths_by_index.count = storage_cfg->max_store_path_index + 1;
    bytes = sizeof(DAStoragePathInfo *) * storage_cfg->paths_by_index.count;
    storage_cfg->paths_by_index.paths = (DAStoragePathInfo **)fc_malloc(bytes);
    if (storage_cfg->paths_by_index.paths == NULL) {
        return ENOMEM;
    }
    memset(storage_cfg->paths_by_index.paths, 0, bytes);

    do_set_paths_by_index(storage_cfg, &storage_cfg->write_cache);
    do_set_paths_by_index(storage_cfg, &storage_cfg->store_path);
    return 0;
}

static int load_store_path_indexes(DAStorageConfig *storage_cfg,
        const char *storage_filename)
{
    int result;
    int old_count;
    int change_count;

    if ((result=store_path_index_init()) != 0) {
        return result;
    }

    old_count = store_path_index_count();
    change_count = 0;
    do {
        if ((result=load_path_indexes(&storage_cfg->write_cache,
                        "write cache paths", &change_count)) != 0)
        {
            break;
        }
        if ((result=load_path_indexes(&storage_cfg->store_path,
                        "store paths", &change_count)) != 0)
        {
            break;
        }

    } while (0);

    storage_cfg->max_store_path_index = store_path_index_max();
    if (change_count > 0) {
        int r;
        r = store_path_index_save();
        if (result == 0) {
            result = r;
        }
    }
    if (result == 0) {
        result = set_paths_by_index(storage_cfg);
    }

    logDebug("old_count: %d, new_count: %d, change_count: %d, "
            "max_store_path_index: %d", old_count,
            store_path_index_count(), change_count,
            storage_cfg->max_store_path_index);

    store_path_index_destroy();
    return result;
}

int storage_config_load(DAStorageConfig *storage_cfg,
        const char *storage_filename)
{
    IniContext ini_context;
    IniFullContext ini_ctx;
    int result;

    memset(storage_cfg, 0, sizeof(DAStorageConfig));
    if ((result=iniLoadFromFile(storage_filename, &ini_context)) != 0) {
        logError("file: "__FILE__", line: %d, "
                "load conf file \"%s\" fail, ret code: %d",
                __LINE__, storage_filename, result);
        return result;
    }

    FAST_INI_SET_FULL_CTX_EX(ini_ctx, storage_filename, NULL, &ini_context);
    result = load_from_config_file(storage_cfg, &ini_ctx);
    iniFreeContext(&ini_context);
    if (result == 0) {
        result = load_store_path_indexes(storage_cfg, storage_filename);
    }
    return result;
}

static void log_paths(DAStoragePathArray *parray, const char *caption)
{
    DAStoragePathInfo *p;
    DAStoragePathInfo *end;
    char avail_space_buff[32];
    char reserved_space_buff[32];
    char prealloc_space_buff[32];

    if (parray->count == 0) {
        return;
    }

    logInfo("%s count: %d", caption, parray->count);
    end = parray->paths + parray->count;
    for (p=parray->paths; p<end; p++) {
        long_to_comma_str(p->space_stat.avail /
                (1024 * 1024), avail_space_buff);
        long_to_comma_str(p->reserved_space.value /
                (1024 * 1024), reserved_space_buff);
        long_to_comma_str(p->prealloc_space.value /
                (1024 * 1024), prealloc_space_buff);
        logInfo("  path %d: %s, index: %d, write_threads: %d, "
                "read_threads: %d, read_io_depth: %d, "
                "prealloc_space ratio: %.2f%%, "
                "reserved_space ratio: %.2f%%, "
                "avail_space: %s MB, prealloc_space: %s MB, "
#ifdef OS_LINUX
                "reserved_space: %s MB, "
                "device block size: %d",
#else
                "reserved_space: %s MB",
#endif
                (int)(p - parray->paths + 1), p->store.path.str,
                p->store.index, p->write_thread_count,
                p->read_thread_count, p->read_io_depth,
                p->prealloc_space.ratio * 100.00,
                p->reserved_space.ratio * 100.00,
                avail_space_buff, prealloc_space_buff,
#ifdef OS_LINUX
                reserved_space_buff,
                p->block_size
#else
                reserved_space_buff
#endif
                );
    }
}

void storage_config_to_log(DAStorageConfig *storage_cfg)
{
    logInfo("storage config, write_threads_per_path: %d, "
            "read_threads_per_path: %d, "
            "io_depth_per_read_thread: %d, "
            "fd_cache_capacity_per_read_thread: %d, "
            "fd_cache_capacity_per_write_thread: %d, "
            "prealloc_space: {ratio_per_path: %.2f%%, "
            "start_time: %02d:%02d, end_time: %02d:%02d }, "
            "trunk_prealloc_threads: %d, "
            "reserved_space_per_disk: %.2f%%, "
            "trunk_file_size: %u MB, "
            "max_trunk_files_per_subdir: %d, "
            "discard_remain_space_size: %d, "
#if 0
            / * "write_cache_to_hd: { on_usage: %.2f%%, start_time: %02d:%02d, "
            "end_time: %02d:%02d }, "  */
#endif
            "reclaim_trunks_on_path_usage: %.2f%%, "
#ifdef OS_LINUX
            "never_reclaim_on_trunk_usage: %.2f%%, "
            "memory_watermark_low: %.2f%%, "
            "memory_watermark_high: %.2f%%, "
            "max_idle_time: %d, "
            "reclaim_interval: %d",
#else
            "never_reclaim_on_trunk_usage: %.2f%%",
#endif
            storage_cfg->write_threads_per_path,
            storage_cfg->read_threads_per_path,
            storage_cfg->io_depth_per_read_thread,
            storage_cfg->fd_cache_capacity_per_read_thread,
            storage_cfg->fd_cache_capacity_per_write_thread,
            storage_cfg->prealloc_space.ratio_per_path * 100.00,
            storage_cfg->prealloc_space.start_time.hour,
            storage_cfg->prealloc_space.start_time.minute,
            storage_cfg->prealloc_space.end_time.hour,
            storage_cfg->prealloc_space.end_time.minute,
            storage_cfg->trunk_prealloc_threads,
            storage_cfg->reserved_space_per_disk * 100.00,
            storage_cfg->trunk_file_size / (1024 * 1024),
            storage_cfg->max_trunk_files_per_subdir,
            storage_cfg->discard_remain_space_size,
            /*
            storage_cfg->write_cache_to_hd.on_usage * 100.00,
            storage_cfg->write_cache_to_hd.start_time.hour,
            storage_cfg->write_cache_to_hd.start_time.minute,
            storage_cfg->write_cache_to_hd.end_time.hour,
            storage_cfg->write_cache_to_hd.end_time.minute,
            */
            storage_cfg->reclaim_trunks_on_path_usage * 100.00,
#ifdef OS_LINUX
            storage_cfg->never_reclaim_on_trunk_usage * 100.00,
            storage_cfg->aio_read_buffer.memory_watermark_low.ratio * 100.00,
            storage_cfg->aio_read_buffer.memory_watermark_high.ratio * 100.00,
            storage_cfg->aio_read_buffer.max_idle_time,
            storage_cfg->aio_read_buffer.reclaim_interval
#else
            storage_cfg->never_reclaim_on_trunk_usage * 100.00
#endif
            );

    log_paths(&storage_cfg->write_cache, "write cache paths");
    log_paths(&storage_cfg->store_path, "store paths");
}
