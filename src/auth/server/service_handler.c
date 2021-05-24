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

//service_handler.c

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/ioevent_loop.h"
#include "sf/sf_util.h"
#include "sf/sf_func.h"
#include "sf/sf_nio.h"
#include "sf/sf_service.h"
#include "sf/sf_global.h"
#include "common/auth_proto.h"
#include "common/auth_func.h"
#include "db/dao/dao.h"
#include "db/auth_db.h"
#include "server_global.h"
#include "server_func.h"
#include "common_handler.h"
#include "service_handler.h"

int service_handler_init()
{
    g_fcfs_auth_client_vars.need_load_passwd = false;
    return auth_db_init();
}

int service_handler_destroy()
{
    return 0;
}

void service_task_finish_cleanup(struct fast_task_info *task)
{
    switch (SERVER_TASK_TYPE) {
        case AUTH_SERVER_TASK_TYPE_SESSION:
            if (SESSION_ENTRY != NULL) {
                server_session_delete(SESSION_ENTRY->session_id);
                SESSION_ENTRY = NULL;
            }
            SERVER_TASK_TYPE = SF_SERVER_TASK_TYPE_NONE;
            break;
        default:
            break;
    }

    sf_task_finish_clean_up(task);
}

static int check_user_priv(struct fast_task_info *task)
{
    int64_t the_priv;

    switch (REQUEST.header.cmd) {
        case SF_PROTO_ACTIVE_TEST_REQ:
        case FCFS_AUTH_SERVICE_PROTO_USER_LOGIN_REQ:
        case FCFS_AUTH_SERVICE_PROTO_SPOOL_GET_QUOTA_REQ:
            return 0;
        case FCFS_AUTH_SERVICE_PROTO_USER_CREATE_REQ:
        case FCFS_AUTH_SERVICE_PROTO_USER_LIST_REQ:
        case FCFS_AUTH_SERVICE_PROTO_USER_GRANT_REQ:
        case FCFS_AUTH_SERVICE_PROTO_USER_REMOVE_REQ:
            the_priv = FCFS_AUTH_USER_PRIV_USER_MANAGE;
            break;
        case FCFS_AUTH_SERVICE_PROTO_SPOOL_CREATE_REQ:
        case FCFS_AUTH_SERVICE_PROTO_SPOOL_LIST_REQ:
        case FCFS_AUTH_SERVICE_PROTO_SPOOL_REMOVE_REQ:
        case FCFS_AUTH_SERVICE_PROTO_SPOOL_SET_QUOTA_REQ:
        case FCFS_AUTH_SERVICE_PROTO_GPOOL_GRANT_REQ:
        case FCFS_AUTH_SERVICE_PROTO_GPOOL_WITHDRAW_REQ:
            the_priv = FCFS_AUTH_USER_PRIV_CREATE_POOL;
            break;
        case FCFS_AUTH_SERVICE_PROTO_GPOOL_LIST_REQ:
            the_priv = 0;
            break;
        default:
            RESPONSE.error.length = sprintf(RESPONSE.error.message,
                    "unkown cmd: %d", REQUEST.header.cmd);
            return -EINVAL;
    }

    if (SESSION_ENTRY == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "please login first!");
        return EPERM;
    }

    if ((the_priv != 0) && ((SESSION_USER.priv & the_priv) == 0)) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "permission denied");
        return EPERM;
    }

    return 0;
}

static int service_deal_user_login(struct fast_task_info *task)
{
    FCFSAuthProtoUserLoginReq *req;
    FCFSAuthProtoUserLoginResp *resp;
    ServerSessionFields *fields;
    FCFSAuthProtoNameInfo *proto_poolname;
    string_t username;
    string_t passwd;
    string_t poolname;
    int flags;
    int result;

    if ((result=server_check_min_body_length(sizeof(
                        FCFSAuthProtoUserLoginReq) + 1)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoUserLoginReq *)REQUEST.body;
    FC_SET_STRING_EX(username, req->up_pair.username.str,
            req->up_pair.username.len);
    FC_SET_STRING_EX(passwd, req->up_pair.passwd,
            FCFS_AUTH_PASSWD_LEN);

    proto_poolname = (FCFSAuthProtoNameInfo *)(req->
            up_pair.username.str + username.len);
    FC_SET_STRING_EX(poolname, proto_poolname->str, proto_poolname->len);
    if ((result=server_expect_body_length(
                    sizeof(FCFSAuthProtoUserLoginReq)
                    + username.len + poolname.len)) != 0)
    {
        return result;
    }

    if (SERVER_TASK_TYPE != SF_SERVER_TASK_TYPE_NONE) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "task type: %d != %d", SERVER_TASK_TYPE,
                SF_SERVER_TASK_TYPE_NONE);
        return EEXIST;
    }

    if (SESSION_ENTRY != NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "user already logined");
        return EEXIST;
    }

    fields = (ServerSessionFields *)(SESSION_HOLDER->fields);
    if (!((fields->dbuser=adb_user_get(SERVER_CTX, &username)) != NULL &&
            fc_string_equal(&fields->dbuser->user.passwd, &passwd)))
    {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "user login fail, username or password not correct");
        return EPERM;
    }

    if (poolname.len > 0) {
        if ((fields->dbpool=adb_spool_global_get(&poolname)) == NULL) {
            RESPONSE.error.length = sprintf(RESPONSE.error.message,
                    "pool %.*s not exist", poolname.len, poolname.str);
            return ENOENT;
        }

        if ((result=adb_granted_privs_get(SERVER_CTX, fields->dbuser,
                        fields->dbpool, &fields->pool_privs)) != 0)
        {
            RESPONSE.error.length = sprintf(RESPONSE.error.message,
                    "pool %.*s, not priviledge", poolname.len, poolname.str);
            return EPERM;
        }
    } else {
        fields->dbpool = NULL;
        fields->pool_privs.fdir = 0;
        fields->pool_privs.fstore = 0;
    }

    flags = req->flags;
    fields->publish = (flags & FCFS_AUTH_SESSION_FLAGS_PUBLISH) != 0;
    SESSION_HOLDER->session_id = 0;
    if ((SESSION_ENTRY=server_session_add(SESSION_HOLDER,
                    fields->publish)) == NULL)
    {
        return ENOMEM;
    }
    SERVER_TASK_TYPE = AUTH_SERVER_TASK_TYPE_SESSION;

    /*
    logInfo("session id: %"PRId64", pool: %.*s, pool_privs "
            "{fdir: %d, fstore: %d}", SESSION_ENTRY->session_id,
            poolname.len, poolname.str,
            fields->pool_privs.fdir, fields->pool_privs.fstore);
            */

    resp = (FCFSAuthProtoUserLoginResp *)REQUEST.body;
    long2buff(SESSION_ENTRY->session_id, resp->session_id);
    RESPONSE.header.body_len = sizeof(FCFSAuthProtoUserLoginResp);
    RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_USER_LOGIN_RESP;
    TASK_ARG->context.common.response_done = true;
    return 0;
}

static int service_deal_user_create(struct fast_task_info *task)
{
    FCFSAuthProtoUserCreateReq *req;
    FCFSAuthUserInfo user;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoUserCreateReq)
                    + 1, sizeof(FCFSAuthProtoUserCreateReq) + NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoUserCreateReq *)REQUEST.body;
    FC_SET_STRING_EX(user.name, req->up_pair.username.str,
            req->up_pair.username.len);
    FC_SET_STRING_EX(user.passwd, req->up_pair.passwd,
            FCFS_AUTH_PASSWD_LEN);
    user.priv = buff2long(req->priv);
    if ((result=server_expect_body_length(
                    sizeof(FCFSAuthProtoUserCreateReq)
                    + user.name.len)) != 0)
    {
        return result;
    }

    if ((result=fc_check_filename_ex(&user.name, "username",
                    RESPONSE.error.message, &RESPONSE.error.length,
                    sizeof(RESPONSE.error.message))) != 0)
    {
        return result;
    }

    return adb_user_create(SERVER_CTX, &user);
}

static int service_deal_user_list(struct fast_task_info *task)
{
    FCFSAuthUserArray array;
    const DBUserInfo *dbuser;
    const FCFSAuthUserInfo *user;
    const FCFSAuthUserInfo *end;
    string_t username;
    FCFSAuthProtoUserListReq *req;
    FCFSAuthProtoListRespHeader *resp_header;
    FCFSAuthProtoUserListRespBodyPart *body_part;
    char *p;
    int result;

    if ((result=server_check_body_length(0, NAME_MAX)) != 0) {
        return result;
    }

    fcfs_auth_user_init_array(&array);
    if (REQUEST.header.body_len > 0) {
        req = (FCFSAuthProtoUserListReq *)REQUEST.body;
        username.len = REQUEST.header.body_len;
        username.str = req->username;
        if ((dbuser=adb_user_get(SERVER_CTX, &username)) == NULL) {
            RESPONSE.error.length = sprintf(RESPONSE.error.message,
                    "username:%.*s not exist", username.len, username.str);
            fcfs_auth_user_free_array(&array);
            return ENOENT;
        }
        array.users[0] = dbuser->user;
        array.count = 1;
    } else if ((result=adb_user_list(SERVER_CTX, &array)) != 0) {
        fcfs_auth_user_free_array(&array);
        return result;
    }

    resp_header = (FCFSAuthProtoListRespHeader *)REQUEST.body;
    p = (char *)(resp_header + 1);
    end = array.users + array.count;
    for (user=array.users; user<end; user++) {
        body_part = (FCFSAuthProtoUserListRespBodyPart *)p;
        long2buff(user->priv, body_part->priv);
        body_part->username.len = user->name.len;
        memcpy(body_part->username.str, user->name.str, user->name.len);
        p += sizeof(FCFSAuthProtoUserListRespBodyPart) + user->name.len;
    }
    int2buff(array.count, resp_header->count);
    RESPONSE.header.body_len = p - REQUEST.body;
    RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_USER_LIST_RESP;
    TASK_ARG->context.common.response_done = true;

    fcfs_auth_user_free_array(&array);
    return 0;
}

static int service_deal_user_grant(struct fast_task_info *task)
{
    FCFSAuthProtoUserGrantReq *req;
    string_t username;
    int64_t priv;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoUserGrantReq)
                    + 1, sizeof(FCFSAuthProtoUserGrantReq) + NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoUserGrantReq *)REQUEST.body;
    FC_SET_STRING_EX(username, req->username.str, req->username.len);
    priv = buff2long(req->priv);
    if ((result=server_expect_body_length(
                    sizeof(FCFSAuthProtoUserGrantReq)
                    + username.len)) != 0)
    {
        return result;
    }

    return adb_user_update_priv(SERVER_CTX, &username, priv);
}

static int service_deal_user_remove(struct fast_task_info *task)
{
    FCFSAuthProtoUserRemoveReq *req;
    string_t username;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoUserRemoveReq)
                    + 1, sizeof(FCFSAuthProtoUserRemoveReq) + NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoUserRemoveReq *)REQUEST.body;
    FC_SET_STRING_EX(username, req->username.str, req->username.len);
    if ((result=server_expect_body_length(
                    sizeof(FCFSAuthProtoUserRemoveReq)
                    + username.len)) != 0)
    {
        return result;
    }

    return adb_user_remove(SERVER_CTX, &username);
}

static int spool_create_by_template(struct fast_task_info *task,
        const string_t *username, const string_t *pool_name,
        FCFSAuthStoragePoolInfo *spool, const bool dryrun)
{
    int result;
    int name_size;
    struct {
        char buff[32];
        string_t tag;
        struct {
            int64_t n;
            string_t s;
        } value;
    } auto_id;

    name_size = spool->name.len;
    FC_SET_STRING_EX(auto_id.tag, FCFS_AUTH_AUTO_ID_TAG_STR,
            FCFS_AUTH_AUTO_ID_TAG_LEN);
    auto_id.value.n = adb_spool_get_auto_id(SERVER_CTX);
    auto_id.value.s.str = auto_id.buff;
    do {
        auto_id.value.s.len = sprintf(auto_id.value.s.str,
                "%"PRId64, auto_id.value.n);
        if ((result=str_replace(pool_name, &auto_id.tag, &auto_id.value.s,
                        &spool->name, name_size)) != 0)
        {
            RESPONSE.error.length = sprintf(RESPONSE.error.message,
                    "invalid pool name: %.*s\n",
                    pool_name->len, pool_name->str);
            return result;
        }

        if (dryrun) {
            result = adb_spool_access(SERVER_CTX, &spool->name);
            if (result == 0) {  //pool exist
                result = adb_spool_next_auto_id(SERVER_CTX, &auto_id.value.n);
                continue;
            } else if (result == ENOENT) {
                result = 0;
            }
            break;
        }

        result = adb_spool_create(SERVER_CTX, username, spool);
        if (result == 0) {
            result = adb_spool_inc_auto_id(SERVER_CTX);
            break;
        } else if (result == EEXIST) {
            result = adb_spool_next_auto_id(SERVER_CTX, &auto_id.value.n);
        } else {
            break;
        }
    } while (result == 0);

    return result;
}

static int service_deal_spool_create(struct fast_task_info *task)
{
    FCFSAuthProtoSPoolCreateReq *req;
    FCFSAuthProtoSPoolCreateResp *resp;
    char name_buff[NAME_MAX + 1];
    string_t username;
    string_t pool_name;
    FCFSAuthStoragePoolInfo spool;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoSPoolCreateReq),
                    sizeof(FCFSAuthProtoSPoolCreateReq) + NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoSPoolCreateReq *)REQUEST.body;
    FC_SET_STRING_EX(spool.name, req->poolname.str, req->poolname.len);
    spool.quota = buff2long(req->quota);
    if ((result=server_expect_body_length(
                    sizeof(FCFSAuthProtoSPoolCreateReq)
                    + spool.name.len)) != 0)
    {
        return result;
    }

    if (spool.name.len == 0) {
        spool.name = POOL_NAME_TEMPLATE;
    } else if ((result=fc_check_filename_ex(&spool.name, "pool name",
                    RESPONSE.error.message, &RESPONSE.error.length,
                    sizeof(RESPONSE.error.message))) != 0)
    {
        return result;
    }

    username = SESSION_USER.name;
    if (spool.name.len >= FCFS_AUTH_AUTO_ID_TAG_LEN &&
            strstr(spool.name.str, FCFS_AUTH_AUTO_ID_TAG_STR) != NULL)
    {
        pool_name = spool.name;
        spool.name.str = name_buff;
        spool.name.len = sizeof(name_buff);
        result = spool_create_by_template(task, &username,
                &pool_name, &spool, req->dryrun);
    } else {
        if (req->dryrun) {
            result = adb_spool_access(SERVER_CTX, &spool.name);
            if (result == 0) {
                result = EEXIST;
            } else if (result == ENOENT) {
                result = 0;
            }
        } else {
            result = adb_spool_create(SERVER_CTX, &username, &spool);
        }
    }

    if (result == 0) {
        resp = (FCFSAuthProtoSPoolCreateResp *)REQUEST.body;
        resp->poolname.len = spool.name.len;
        memcpy(resp->poolname.str, spool.name.str, spool.name.len);
        RESPONSE.header.body_len = sizeof(*resp) + spool.name.len;
        RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_SPOOL_CREATE_RESP;
        TASK_ARG->context.common.response_done = true;
    } else if (result == EEXIST) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "pool %.*s already exist", spool.name.len, spool.name.str);
    }
    return result;
}

static int service_deal_spool_list(struct fast_task_info *task)
{
    FCFSAuthStoragePoolArray array;
    const FCFSAuthStoragePoolInfo *spool;
    const FCFSAuthStoragePoolInfo *end;
    string_t username;
    string_t poolname;
    FCFSAuthProtoSPoolListReq *req;
    FCFSAuthProtoListRespHeader *resp_header;
    FCFSAuthProtoSPoolListRespBodyPart *body_part;
    char *p;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoSPoolListReq),
                    sizeof(FCFSAuthProtoSPoolListReq) + NAME_MAX * 2)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoSPoolListReq *)REQUEST.body;
    fcfs_auth_parse_user_pool_pair(&req->up_pair, &username, &poolname);
    if ((result=server_expect_body_length(sizeof(FCFSAuthProtoSPoolListReq)
                    + username.len + poolname.len)) != 0)
    {
        return result;
    }

    if (username.len == 0) {
        username = SESSION_USER.name;
    } else {
        if ((SESSION_USER.priv & FCFS_AUTH_USER_PRIV_USER_MANAGE) == 0) {
            RESPONSE.error.length = sprintf(
                    RESPONSE.error.message,
                    "permission denied");
            return EPERM;
        }
    }

    fcfs_auth_spool_init_array(&array);
    if (poolname.len > 0) {
        if ((spool=adb_spool_get(SERVER_CTX, &username, &poolname)) == NULL) {
            RESPONSE.error.length = sprintf(RESPONSE.error.message,
                    "user: %.*s, pool: %.*s not exist", username.len,
                    username.str, poolname.len, poolname.str);
            fcfs_auth_spool_free_array(&array);
            return ENOENT;
        }

        array.spools[0] = *spool;
        array.count = 1;
    } else if ((result=adb_spool_list(SERVER_CTX, &username, &array)) != 0) {
        fcfs_auth_spool_free_array(&array);
        return result;
    }

    resp_header = (FCFSAuthProtoListRespHeader *)REQUEST.body;
    p = (char *)(resp_header + 1);
    end = array.spools + array.count;
    for (spool=array.spools; spool<end; spool++) {
        body_part = (FCFSAuthProtoSPoolListRespBodyPart *)p;
        long2buff(spool->quota, body_part->quota);
        body_part->poolname.len = spool->name.len;
        memcpy(body_part->poolname.str, spool->name.str, spool->name.len);
        p += sizeof(FCFSAuthProtoSPoolListRespBodyPart) + spool->name.len;
    }
    int2buff(array.count, resp_header->count);
    RESPONSE.header.body_len = p - REQUEST.body;
    RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_SPOOL_LIST_RESP;
    TASK_ARG->context.common.response_done = true;

    fcfs_auth_spool_free_array(&array);
    return 0;
}

static int service_deal_spool_remove(struct fast_task_info *task)
{
    FCFSAuthProtoSPoolRemoveReq *req;
    string_t username;
    string_t poolname;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoSPoolRemoveReq)
                    + 1, sizeof(FCFSAuthProtoSPoolRemoveReq) + NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoSPoolRemoveReq *)REQUEST.body;
    FC_SET_STRING_EX(poolname, req->poolname.str, req->poolname.len);
    if ((result=server_expect_body_length(
                    sizeof(FCFSAuthProtoSPoolRemoveReq)
                    + poolname.len)) != 0)
    {
        return result;
    }

    username = SESSION_USER.name;
    return adb_spool_remove(SERVER_CTX, &username, &poolname);
}

static int service_deal_spool_set_quota(struct fast_task_info *task)
{
    FCFSAuthProtoSPoolSetQuotaReq *req;
    string_t username;
    string_t poolname;
    int64_t quota;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoSPoolSetQuotaReq)
                    + 1, sizeof(FCFSAuthProtoSPoolSetQuotaReq) +
                    NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoSPoolSetQuotaReq *)REQUEST.body;
    FC_SET_STRING_EX(poolname, req->poolname.str, req->poolname.len);
    if ((result=server_expect_body_length(
                    sizeof(FCFSAuthProtoSPoolSetQuotaReq)
                    + poolname.len)) != 0)
    {
        return result;
    }

    quota = buff2long(req->quota);
    username = SESSION_USER.name;
    return adb_spool_set_quota(SERVER_CTX, &username, &poolname, quota);
}

static int service_deal_spool_get_quota(struct fast_task_info *task)
{
    FCFSAuthProtoSPoolGetQuotaReq *req;
    FCFSAuthProtoSPoolGetQuotaResp *resp;
    string_t poolname;
    int64_t quota;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoSPoolGetQuotaReq)
                    + 1, sizeof(FCFSAuthProtoSPoolGetQuotaReq)
                    + NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoSPoolGetQuotaReq *)REQUEST.body;
    FC_SET_STRING_EX(poolname, req->poolname.str, req->poolname.len);
    if ((result=server_expect_body_length(
                    sizeof(FCFSAuthProtoSPoolGetQuotaReq)
                    + poolname.len)) != 0)
    {
        return result;
    }

    if ((result=adb_spool_get_quota(SERVER_CTX, &poolname, &quota)) == 0) {
        resp = (FCFSAuthProtoSPoolGetQuotaResp *)REQUEST.body;
        long2buff(quota, resp->quota);
        RESPONSE.header.body_len = sizeof(*resp);
        TASK_ARG->context.common.response_done = true;
    }

    return result;
}

static int service_deal_spool_grant(struct fast_task_info *task)
{
    FCFSAuthProtoSPoolGrantReq *req;
    struct {
        string_t my;
        string_t dest;
    } usernames;
    string_t poolname;
    const FCFSAuthStoragePoolInfo *spool;
    FCFSAuthGrantedPoolInfo granted;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoSPoolGrantReq)
                    + 2, sizeof(FCFSAuthProtoSPoolGrantReq)
                    + 2 * NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoSPoolGrantReq *)REQUEST.body;
    fcfs_auth_parse_user_pool_pair(&req->up_pair, &usernames.dest, &poolname);
    if ((result=server_expect_body_length(
                    sizeof(FCFSAuthProtoSPoolGrantReq)
                    + usernames.dest.len + poolname.len)) != 0)
    {
        return result;
    }

    usernames.my = SESSION_USER.name;
    if ((spool=adb_spool_get(SERVER_CTX, &usernames.my, &poolname)) == NULL) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "user: %.*s, pool: %.*s not exist", usernames.my.len,
                usernames.my.str, poolname.len, poolname.str);
        return ENOENT;
    }

    granted.pool_id = spool->id;
    granted.privs.fdir = buff2int(req->privs.fdir);
    granted.privs.fstore = buff2int(req->privs.fstore);
    if ((granted.privs.fdir == FCFS_AUTH_POOL_ACCESS_NONE) &&
            (granted.privs.fstore == FCFS_AUTH_POOL_ACCESS_NONE))
    {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "no access priviledges to grant");
        return EINVAL;
    }

    /*
    logInfo("sizeof(FCFSAuthProtoSPoolGrantReq): %d, dest: %.*s(%d), "
            "poolname: %.*s, pool_id: %"PRId64,
            (int)sizeof(FCFSAuthProtoSPoolGrantReq),
            usernames.dest.len, usernames.dest.str, usernames.dest.len,
            poolname.len, poolname.str, granted.pool_id);
            */

    return adb_granted_create(SERVER_CTX, &usernames.dest, &granted);
}

static int service_deal_spool_withdraw(struct fast_task_info *task)
{
    FCFSAuthProtoSPoolWithdrawReq *req;
    struct {
        string_t my;
        string_t dest;
    } usernames;
    string_t poolname;
    const FCFSAuthStoragePoolInfo *spool;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoSPoolWithdrawReq)
                    + 2, sizeof(FCFSAuthProtoSPoolWithdrawReq)
                    + 2 * NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoSPoolWithdrawReq *)REQUEST.body;
    fcfs_auth_parse_user_pool_pair(&req->up_pair, &usernames.dest, &poolname);
    if ((result=server_expect_body_length(
                    sizeof(FCFSAuthProtoSPoolWithdrawReq)
                    + usernames.dest.len + poolname.len)) != 0)
    {
        return result;
    }

    usernames.my = SESSION_USER.name;
    if ((spool=adb_spool_get(SERVER_CTX, &usernames.my, &poolname)) == NULL) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "user: %.*s, pool: %.*s not exist", usernames.my.len,
                usernames.my.str, poolname.len, poolname.str);
        return ENOENT;
    }

    return adb_granted_remove(SERVER_CTX, &usernames.dest, spool->id);
}

static int service_deal_gpool_list(struct fast_task_info *task)
{
    FCFSAuthGrantedPoolArray array;
    const FCFSAuthGrantedPoolFullInfo *gpool;
    const FCFSAuthGrantedPoolFullInfo *end;
    char pname_buff[NAME_MAX];
    string_t username;
    string_t poolname;
    FCFSAuthProtoGPoolListReq *req;
    FCFSAuthProtoListRespHeader *resp_header;
    FCFSAuthProtoGPoolListRespBodyPart *body_part;
    char *p;
    int count;
    int result;

    if ((result=server_check_body_length(sizeof(FCFSAuthProtoGPoolListReq),
                    sizeof(FCFSAuthProtoGPoolListReq) + NAME_MAX * 2)) != 0)
    {
        return result;
    }

    req = (FCFSAuthProtoGPoolListReq *)REQUEST.body;
    fcfs_auth_parse_user_pool_pair(&req->up_pair, &username, &poolname);
    if ((result=server_expect_body_length(sizeof(FCFSAuthProtoGPoolListReq)
                    + username.len + poolname.len)) != 0)
    {
        return result;
    }

    if (username.len == 0) {
        username = SESSION_USER.name;
    } else {
        if ((SESSION_USER.priv & FCFS_AUTH_USER_PRIV_USER_MANAGE) == 0) {
            RESPONSE.error.length = sprintf(
                    RESPONSE.error.message,
                    "permission denied");
            return EPERM;
        }
    }

    fcfs_auth_granted_init_array(&array);
    if (poolname.len > 0) {
        memcpy(pname_buff, poolname.str, poolname.len);
        poolname.str = pname_buff;
    }

    if ((result=adb_granted_list(SERVER_CTX, &username, &array)) != 0) {
        fcfs_auth_granted_free_array(&array);
        return result;
    }

    count = 0;
    resp_header = (FCFSAuthProtoListRespHeader *)REQUEST.body;
    p = (char *)(resp_header + 1);
    end = array.gpools + array.count;
    for (gpool=array.gpools; gpool<end; gpool++) {
        if (poolname.len == 0 || fc_string_equals(
                    &gpool->pool_name, &poolname))
        {
            body_part = (FCFSAuthProtoGPoolListRespBodyPart *)p;
            int2buff(gpool->granted.privs.fdir, body_part->privs.fdir);
            int2buff(gpool->granted.privs.fstore, body_part->privs.fstore);
            fcfs_auth_pack_user_pool_pair(&gpool->username,
                    &gpool->pool_name, &body_part->up_pair);
            p += sizeof(FCFSAuthProtoGPoolListRespBodyPart) +
                gpool->username.len + gpool->pool_name.len;
            count++;
        }
    }
    int2buff(count, resp_header->count);
    RESPONSE.header.body_len = p - REQUEST.body;
    RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_GPOOL_LIST_RESP;
    TASK_ARG->context.common.response_done = true;

    fcfs_auth_granted_free_array(&array);
    return 0;
}

static int service_process(struct fast_task_info *task)
{
    switch (REQUEST.header.cmd) {
        case SF_PROTO_ACTIVE_TEST_REQ:
            RESPONSE.header.cmd = SF_PROTO_ACTIVE_TEST_RESP;
            return sf_proto_deal_active_test(task, &REQUEST, &RESPONSE);
        case FCFS_AUTH_SERVICE_PROTO_USER_LOGIN_REQ:
            return service_deal_user_login(task);
        case FCFS_AUTH_SERVICE_PROTO_USER_CREATE_REQ:
            RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_USER_CREATE_RESP;
            return service_deal_user_create(task);
        case FCFS_AUTH_SERVICE_PROTO_USER_LIST_REQ:
            return service_deal_user_list(task);
        case FCFS_AUTH_SERVICE_PROTO_USER_GRANT_REQ:
            RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_USER_GRANT_RESP;
            return service_deal_user_grant(task);
        case FCFS_AUTH_SERVICE_PROTO_USER_REMOVE_REQ:
            RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_USER_REMOVE_RESP;
            return service_deal_user_remove(task);
        case FCFS_AUTH_SERVICE_PROTO_SPOOL_CREATE_REQ:
            return service_deal_spool_create(task);
        case FCFS_AUTH_SERVICE_PROTO_SPOOL_LIST_REQ:
            return service_deal_spool_list(task);
        case FCFS_AUTH_SERVICE_PROTO_SPOOL_REMOVE_REQ:
            RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_SPOOL_REMOVE_RESP;
            return service_deal_spool_remove(task);
        case FCFS_AUTH_SERVICE_PROTO_SPOOL_SET_QUOTA_REQ:
            RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_SPOOL_SET_QUOTA_RESP;
            return service_deal_spool_set_quota(task);
        case FCFS_AUTH_SERVICE_PROTO_SPOOL_GET_QUOTA_REQ:
            RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_SPOOL_GET_QUOTA_RESP;
            return service_deal_spool_get_quota(task);
        case FCFS_AUTH_SERVICE_PROTO_GPOOL_GRANT_REQ:
            RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_GPOOL_GRANT_RESP;
            return service_deal_spool_grant(task);
        case FCFS_AUTH_SERVICE_PROTO_GPOOL_WITHDRAW_REQ:
            RESPONSE.header.cmd = FCFS_AUTH_SERVICE_PROTO_GPOOL_WITHDRAW_RESP;
            return service_deal_spool_withdraw(task);
        case FCFS_AUTH_SERVICE_PROTO_GPOOL_LIST_REQ:
            return service_deal_gpool_list(task);
        default:
            RESPONSE.error.length = sprintf(
                    RESPONSE.error.message,
                    "unkown cmd: %d", REQUEST.header.cmd);
            return -EINVAL;
    }
}

int service_deal_task(struct fast_task_info *task, const int stage)
{
    int result;

    /*
    logInfo("file: "__FILE__", line: %d, "
            "task: %p, sock: %d, nio stage: %d, continue: %d, "
            "cmd: %d (%s)", __LINE__, task, task->event.fd, stage,
            stage == SF_NIO_STAGE_CONTINUE,
            ((FDIRProtoHeader *)task->data)->cmd,
            fdir_get_cmd_caption(((FDIRProtoHeader *)task->data)->cmd));
            */

    if (stage == SF_NIO_STAGE_CONTINUE) {
        if (task->continue_callback != NULL) {
            result = task->continue_callback(task);
        } else {
            result = RESPONSE_STATUS;
            if (result == TASK_STATUS_CONTINUE) {
                logError("file: "__FILE__", line: %d, "
                        "unexpect status: %d", __LINE__, result);
                result = EBUSY;
            }
        }
    } else {
        sf_proto_init_task_context(task, &TASK_CTX.common);
        if ((result=check_user_priv(task)) == 0) {
            result = service_process(task);
        }
    }

    if (result == TASK_STATUS_CONTINUE) {
        return 0;
    } else {
        RESPONSE_STATUS = result;
        return sf_proto_deal_task_done(task, &TASK_CTX.common);
    }
}

static int create_session_for_access_fdir(ServerSessionEntry
        *session_holder, char *session_id)
{
    ServerSessionFields *fields;
    ServerSessionEntry *session;

    fields = (ServerSessionFields *)(session_holder->fields);
    fields->publish = false;
    fields->pool_privs.fdir = FCFS_AUTH_POOL_ACCESS_ALL;
    fields->pool_privs.fstore = FCFS_AUTH_POOL_ACCESS_ALL;
    session_holder->session_id = 0;
    if ((session=server_session_add(session_holder,
                    fields->publish)) == NULL)
    {
        return ENOMEM;
    }

    long2buff(session->session_id, session_id);
    return 0;
}

void *service_alloc_thread_extra_data(const int thread_index)
{
    short alloc_size;
    short dao_context_size;
    static bool dao_session_inited = false;
    static char dao_session_id[FCFS_AUTH_SESSION_ID_LEN];
    AuthServerContext *server_context;

    dao_context_size = dao_get_context_size();
    alloc_size = sizeof(AuthServerContext) + dao_context_size +
        sizeof(ServerSessionEntry) + sizeof(ServerSessionFields);
    server_context = (AuthServerContext *)fc_malloc(alloc_size);
    if (server_context == NULL) {
        return NULL;
    }

    memset(server_context, 0, alloc_size);
    server_context->dao_ctx = (void *)(server_context + 1);
    server_context->service.session_holder = (ServerSessionEntry *)(((char *)
                server_context->dao_ctx) + dao_context_size);
    server_context->service.session_holder->fields =
        server_context->service.session_holder + 1;
    if (!dao_session_inited) {
        dao_session_inited = true;
        if (create_session_for_access_fdir(server_context->
                    service.session_holder, dao_session_id) != 0)
        {
            return NULL;
        }
    }

    if (dao_init_context(thread_index, server_context->
                dao_ctx, dao_session_id) != 0)
    {
        sf_terminate_myself();
        return NULL;
    }

    return server_context;
}
