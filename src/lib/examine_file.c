/*
 * Examine - a set of tools for memory leak detection on Windows and
 * PE file reader
 *
 * Copyright (C) 2015 Vincent Torri.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <windows.h>
# undef WIN32_LEAN_AND_MEAN
#endif

#include "examine_log.h"
#include "examine_list.h"
#include "examine_file.h"
#ifndef _WIN32
# include "examine_pe_unix.h"
#endif


/*============================================================================*
 *                                  Local                                     *
 *============================================================================*/


static Exm_List *_exm_file_path = NULL;

#ifndef _WIN32
static int
_exm_file_name_strcmp(const void *d1, const void *d2)
{
    return strcmp((const char *)d1, (const char *)d2);
}
#endif

static int
_exm_file_name_strcasecmp(const void *d1, const void *d2)
{
    return _stricmp((const char *)d1, (const char *)d2);
}

static void
_exm_file_base_dir_name_get(const char *filename, char **dir_name, char **base_name)
{
    char *idx;
    char *res;

    if (dir_name) *dir_name = NULL;
    if (base_name) *base_name = NULL;

    if (!filename)
        return;

#ifdef _WIN32
    idx = strrchr(filename, '\\');
#else
    idx = strrchr(filename, '/');
#endif
    if (idx)
    {
        res = malloc((idx - filename + 2) * sizeof(char));
        if (res)
        {
            /* we copy also the last path separator */
            memcpy(res, filename, idx - filename + 1);
            res[idx - filename + 1] = '\0';

            if (dir_name) *dir_name = res;
        }

        if (base_name) *base_name = strdup(idx + 1);
    }
    else
      EXM_LOG_ERR("file %s has not an absolute path", filename);
}

static char *
_exm_file_concat(const char *path, const char *filename)
{
    char *res;
    size_t l1;
    size_t l2;

    l1 = strlen(path);
    l2 = strlen(filename);
    res = (char *)malloc((l1 + l2 + 1) * sizeof(char));
    if (!res)
        return NULL;

    memcpy(res, path, l1);
    memcpy(res + l1, filename, l2);
    res[l1 + l2] = '\0';

    return res;
}

/**
 * @brief Check if the given path is absolute or not.
 *
 * @param[in] filename The file name.
 * @return 1 if the  path is absolute, 0 otherwise.
 *
 * This function checks if @filename has an absolute path or relative
 * path by looking at thefirst three characters. It returnd 1 if the
 * path is absolute, 0 otherwise.
 *
 * @internal
 */
static int
_exm_file_path_is_absolute(const char *filename)
{
#ifdef _WIN32
    if (!filename)
        return 0;

    if (strlen(filename) < 3)
        return 0;

    if ((((*filename >= 'a') && (*filename <= 'z')) ||
         ((*filename >= 'A') && (*filename <= 'Z'))) &&
        (filename[1] == ':') &&
        ((filename[2] == '/') || (filename[2] == '\\')))
        return 1;
#else
    if (*filename == '/')
        return 1;
#endif

    return 0;
}

static int
_exm_file_exists(const char *path, const char *filename)
{
    char *tmp;
#ifdef _WIN32
    struct _stati64 buf;

    tmp = _exm_file_concat(path, filename);
    if (_stati64(tmp, &buf) != 0)
        return 0;
#else
    struct stat buf;

    tmp = _exm_file_concat(path, filename);
    if (stat(tmp, &buf) != 0)
        return 0;
#endif

    return 1;
}

static void
_exm_file_backslash_final_set(char *filename)
{
#ifdef _WIN32
    size_t length;

    length = strlen(filename);
    if (filename[length - 1] != '\\')
    {
        filename[length] = '\\';
        filename[length + 1] = '\0';
    }
#endif
}


/*============================================================================*
 *                                 Global                                     *
 *============================================================================*/


/*============================================================================*
 *                                   API                                      *
 *============================================================================*/


void
exm_file_path_set(void)
{
#ifdef _WIN32
    char buf[32768];

    /* system directory */

    {
        UINT length;

        length = GetSystemDirectory(buf, sizeof(buf));
        if ((length != 0) && (length <= sizeof(buf)))
        {
            _exm_file_backslash_final_set(buf);
            _exm_file_path = exm_list_append(_exm_file_path, strdup(buf));
        }
    }

    /* Windows directory */

    {
        UINT length;

        length = GetWindowsDirectory(buf, sizeof(buf));
        if ((length != 0) && (length <= sizeof(buf)))
        {
            _exm_file_backslash_final_set(buf);
            _exm_file_path = exm_list_append(_exm_file_path, strdup(buf));
        }
    }

    /* PATH directories */

    {
        char *env;
        char *iter;
        char *s;

        /*
         * don't use GetEnvironmentVariable() as MSYS' profile can
         * override $PATH value.
         */
        env = getenv("PATH");
        iter = env;
        while (iter)
        {
            size_t length;

            s = strchr(iter, ';');
            if (!s)
            {
                length = strlen(iter);
                memcpy(buf, iter, length + 1);
                _exm_file_backslash_final_set(buf);
                _exm_file_path = exm_list_append(_exm_file_path, strdup(buf));
                break;
            }

            *s = '\0';
            length = strlen(iter);
            memcpy(buf, iter, length + 1);
            _exm_file_backslash_final_set(buf);
            _exm_file_path = exm_list_append(_exm_file_path, strdup(buf));

            iter = s + 1;
        }
    }
#endif
}

void
exm_file_set(char *filename)
{
#ifdef _WIN32
    char buf[MAX_PATH];
    char *dir_name = NULL;
    char *base_name = NULL;
    char *iter;

    /* change \ separator with // */
    iter = filename;
    while (*iter)
    {
        if (*iter == '/') *iter = '\\';
        iter++;
    }

    /* directory of absolute path in filename */
    EXM_LOG_DBG("Set file %s", filename);
    if (_exm_file_path_is_absolute(filename))
    {
        if (_fullpath(buf, filename, sizeof(buf)))
        {
            _exm_file_base_dir_name_get(buf, &dir_name, &base_name);
            if (!dir_name || !base_name)
            {
                EXM_LOG_ERR("Can not find base dir or base name for %s", filename);
                goto free_base;
            }

            _exm_file_path = exm_list_prepend_if_new(_exm_file_path,
                                                     dir_name,
                                                     _exm_file_name_strcasecmp);
        }
    }
    else
    {
        if (_fullpath(buf, filename, sizeof(buf)))
        {
            _exm_file_base_dir_name_get(buf, &dir_name, &base_name);
            if (!dir_name || !base_name)
            {
                EXM_LOG_ERR("Can not find base dir or base name for %s", filename);
                goto free_base;
            }
            _exm_file_path = exm_list_prepend_if_new(_exm_file_path,
                                                     dir_name,
                                                     _exm_file_name_strcasecmp);
        }
        else
        {
            EXM_LOG_ERR("Can not find the absolute file %s", filename);
        }
    }

  free_base:
    if (base_name)
        free(base_name);
    if (dir_name)
        free(dir_name);
#else
    char buf[PATH_MAX];

    /* directory of absolute path in filename */

    /* FIXME: see what is done on Windows above : dir_name and base_name */
    if (!_exm_file_path_is_absolute(filename))
        filename = realpath(filename, buf);

    _exm_file_path = exm_list_prependif_new(_exm_file_path,
                                            _exm_file_basedir_get(filename),
                                            _exm_file_name_strcmp);
#endif
}

void
exm_file_path_free(void)
{
    exm_list_free(_exm_file_path, free);
}

char *
exm_file_find(const char *filename)
{
#ifdef _WIN32
    char buf[MAX_PATH];
#else
    char buf[PATH_MAX];
#endif
    Exm_List *iter;
    char *file = NULL;
    char *base_name = NULL;

    if (_exm_file_path_is_absolute(filename))
    {
        if (_fullpath(buf, filename, sizeof(buf)))
        {
            _exm_file_base_dir_name_get(buf, NULL, &base_name);
        }
        else
        {
            return NULL;
        }
    }
    else
    {
        if (_fullpath(buf, filename, sizeof(buf)))
        {
            _exm_file_base_dir_name_get(buf, NULL, &base_name);
        }
        else
        {
            base_name = strdup(filename);
        }
    }

    if (!base_name)
    {
        EXM_LOG_ERR("Can not find base name for %s", filename);
        return NULL;
    }

    iter = _exm_file_path;
    while (iter)
    {
        EXM_LOG_DBG("Searching for file %s with base directory %s...", base_name, (const char *)iter->data);
        if (_exm_file_exists((const char *)iter->data, base_name))
        {
            file = _exm_file_concat((const char *)iter->data, base_name);
            EXM_LOG_DBG("Find file %s", file);
            break;
        }
        iter = iter->next;
    }

    free(base_name);
    return file;
}

unsigned long long
exm_file_size_get(const char *filename)
{
#ifdef _WIN32
    struct _stati64 buf;

    if (_stati64(filename, &buf) != 0)
        return 0;

    return buf.st_size;
#else
    struct stat buf;

    if (stat(filename, &buf) != 0)
        return 0;

    return buf.st_size;
#endif
}