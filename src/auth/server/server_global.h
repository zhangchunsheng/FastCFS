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


#ifndef _SERVER_GLOBAL_H
#define _SERVER_GLOBAL_H

#include "fastcommon/common_define.h"
#include "sf/sf_global.h"
#include "../common/auth_global.h"
#include "server_types.h"

#define AUTH_ADMIN_GENERATE_MODE_FIRST  'F'
#define AUTH_ADMIN_GENERATE_MODE_ALWAYS 'A'

typedef struct server_global_vars {
    struct {
        SFContext sf_context;  //for cluster communication
    } cluster;

    struct {
        int mode;
        char *buff; //space for username and secret_key_filename
        string_t username;
        string_t secret_key_filename;
    } admin_generate;

    struct {
        int64_t auto_id_initial;
        string_t pool_name_template;
    } pool_generate;

    char *fdir_client_cfg_filename;
    int pool_usage_refresh_interval;

    SFSlowLogContext slow_log;
} AuthServerGlobalVars;

#define CLUSTER_SF_CTX         g_server_global_vars.cluster.sf_context

#define ADMIN_GENERATE               g_server_global_vars.admin_generate
#define ADMIN_GENERATE_MODE          ADMIN_GENERATE.mode
#define ADMIN_GENERATE_BUFF          ADMIN_GENERATE.buff
#define ADMIN_GENERATE_USERNAME      ADMIN_GENERATE.username
#define ADMIN_GENERATE_KEY_FILENAME  ADMIN_GENERATE.secret_key_filename

#define POOL_GENERATE           g_server_global_vars.pool_generate
#define AUTO_ID_INITIAL         POOL_GENERATE.auto_id_initial
#define POOL_NAME_TEMPLATE      POOL_GENERATE.pool_name_template

#define POOL_USAGE_REFRESH_INTERVAL g_server_global_vars. \
    pool_usage_refresh_interval

#define SLOW_LOG                g_server_global_vars.slow_log
#define SLOW_LOG_CFG            SLOW_LOG.cfg
#define SLOW_LOG_CTX            SLOW_LOG.ctx

#ifdef __cplusplus
extern "C" {
#endif

    extern AuthServerGlobalVars g_server_global_vars;

#ifdef __cplusplus
}
#endif

#endif
