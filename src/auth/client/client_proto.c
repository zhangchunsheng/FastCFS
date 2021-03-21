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

#include <sys/stat.h>
#include <limits.h>
#include "fastcommon/ini_file_reader.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/connection_pool.h"
#include "auth_proto.h"
#include "auth_func.h"
#include "client_global.h"
#include "client_proto.h"

#define check_username(username) \
    check_username_ex(username, true)

static inline int check_username_ex(const string_t *username,
        const bool required)
{
    int min_length;

    min_length = required ? 1 : 0;
    if (username->len < min_length || username->len > NAME_MAX) {
        logError("file: "__FILE__", line: %d, "
                "invalid username length: %d, which <= 0 or > %d",
                __LINE__, username->len, NAME_MAX);
        return EINVAL;
    }

    return 0;
}

static inline int pack_username(const string_t *username,
        FCFSAuthProtoNameInfo *proto_uname)
{
    int result;

    if ((result=check_username(username)) != 0) {
        return result;
    }
    memcpy(proto_uname->str, username->str, username->len);
    proto_uname->len = username->len;
    return 0;
}

static inline int pack_user_passwd_pair(const string_t *username,
        const string_t *passwd, FCFSAuthProtoUserPasswdPair *up_pair)
{
    if (passwd->len != FCFS_AUTH_PASSWD_LEN) {
        logError("file: "__FILE__", line: %d, "
                "invalid password length: %d != %d",
                __LINE__, passwd->len, FCFS_AUTH_PASSWD_LEN);
        return EINVAL;
    }
    memcpy(up_pair->passwd, passwd->str, passwd->len);
    return pack_username(username, &up_pair->username);
}

int fcfs_auth_client_proto_user_login(FCFSAuthClientContext *client_ctx,
        ConnectionInfo *conn, const string_t *username,
        const string_t *passwd)
{
    FCFSAuthProtoHeader *header;
    FCFSAuthProtoUserLoginReq *req;
    char out_buff[sizeof(FCFSAuthProtoHeader) +
        sizeof(FCFSAuthProtoUserLoginReq) +
        NAME_MAX + FCFS_AUTH_PASSWD_LEN];
    SFResponseInfo response;
    FCFSAuthProtoUserLoginResp login_resp;
    int out_bytes;
    int result;

    header = (FCFSAuthProtoHeader *)out_buff;
    req = (FCFSAuthProtoUserLoginReq *)(header + 1);
    out_bytes = sizeof(FCFSAuthProtoHeader) + sizeof(*req) + username->len;
    SF_PROTO_SET_HEADER(header, FCFS_AUTH_SERVICE_PROTO_USER_LOGIN_REQ,
            out_bytes - sizeof(FCFSAuthProtoHeader));
    if ((result=pack_user_passwd_pair(username,
                    passwd, &req->up_pair)) != 0)
    {
        return result;
    }

    response.error.length = 0;
    if ((result=sf_send_and_recv_response(conn, out_buff, out_bytes,
                    &response, client_ctx->common_cfg.network_timeout,
                    FCFS_AUTH_SERVICE_PROTO_USER_LOGIN_RESP,
                    (char *)&login_resp, sizeof(login_resp))) == 0)
    {
        memcpy(client_ctx->session_id, login_resp.session_id,
                FCFS_AUTH_SESSION_ID_LEN);
    } else {
        sf_log_network_error(&response, conn, result);
    }

    return result;
}

int fcfs_auth_client_proto_user_create(FCFSAuthClientContext *client_ctx,
        ConnectionInfo *conn, const FCFSAuthUserInfo *user)
{
    FCFSAuthProtoHeader *header;
    FCFSAuthProtoUserCreateReq *req;
    char out_buff[sizeof(FCFSAuthProtoHeader) +
        sizeof(FCFSAuthProtoUserCreateReq) +
        NAME_MAX + FCFS_AUTH_PASSWD_LEN];
    SFResponseInfo response;
    int out_bytes;
    int result;

    header = (FCFSAuthProtoHeader *)out_buff;
    req = (FCFSAuthProtoUserCreateReq *)(header + 1);
    out_bytes = sizeof(FCFSAuthProtoHeader) + sizeof(*req) + user->name.len;
    SF_PROTO_SET_HEADER(header, FCFS_AUTH_SERVICE_PROTO_USER_CREATE_REQ,
            out_bytes - sizeof(FCFSAuthProtoHeader));
    if ((result=pack_user_passwd_pair(&user->name,
                    &user->passwd, &req->up_pair)) != 0)
    {
        return result;
    }
    long2buff(user->priv, req->priv);

    response.error.length = 0;
    if ((result=sf_send_and_recv_none_body_response(conn, out_buff, out_bytes,
                    &response, client_ctx->common_cfg.network_timeout,
                    FCFS_AUTH_SERVICE_PROTO_USER_CREATE_RESP)) != 0)
    {
        sf_log_network_error(&response, conn, result);
    }

    return result;
}

int fcfs_auth_client_proto_user_list(FCFSAuthClientContext *client_ctx,
        ConnectionInfo *conn, const string_t *username,
        SFProtoRecvBuffer *buffer, FCFSAuthUserArray *array)
{
    FCFSAuthProtoHeader *header;
    char out_buff[sizeof(FCFSAuthProtoHeader) + NAME_MAX];
    SFResponseInfo response;
    FCFSAuthProtoUserListRespHeader *resp_header;
    FCFSAuthProtoUserListRespBodyPart *body_part;
    FCFSAuthUserInfo *user;
    FCFSAuthUserInfo *end;
    char *p;
    int out_bytes;
    int count;
    int result;

    if ((result=check_username_ex(username, false)) != 0) {
        return result;
    }

    header = (FCFSAuthProtoHeader *)out_buff;
    out_bytes = sizeof(FCFSAuthProtoHeader) + username->len;
    SF_PROTO_SET_HEADER(header, FCFS_AUTH_SERVICE_PROTO_USER_LIST_REQ,
            out_bytes - sizeof(FCFSAuthProtoHeader));
    if (username->len > 0) {
        FCFSAuthProtoUserListReq *req;
        req = (FCFSAuthProtoUserListReq *)(header + 1);
        memcpy(req->username, username->str, username->len);
    }

    response.error.length = 0;
    if ((result=sf_send_and_recv_vary_response(conn, out_buff, out_bytes,
                    &response, client_ctx->common_cfg.network_timeout,
                    FCFS_AUTH_SERVICE_PROTO_USER_LIST_RESP, buffer,
                    sizeof(FCFSAuthProtoUserListRespHeader))) != 0)
    {
        sf_log_network_error(&response, conn, result);
        return result;
    }

    resp_header = (FCFSAuthProtoUserListRespHeader *)buffer->buff;
    count = buff2int(resp_header->count);
    if ((result=fcfs_auth_user_check_realloc_array(array, count)) != 0) {
        return result;
    }

    p = (char *)(resp_header + 1);
    end = array->users + count;
    for (user=array->users; user<end; user++) {
        body_part = (FCFSAuthProtoUserListRespBodyPart *)p;
        user->priv = buff2long(body_part->priv);
        user->name.len = body_part->username.len;
        user->name.str = body_part->username.str;
        p += sizeof(FCFSAuthProtoUserListRespBodyPart) + user->name.len;
    }

    if (response.header.body_len != p - buffer->buff) {
        logError("file: "__FILE__", line: %d, "
                "response body length: %d != expect: %d",
                __LINE__, response.header.body_len,
                (int)(p - buffer->buff));
        return EINVAL;
    }

    array->count = count;
    return result;
}

int fcfs_auth_client_proto_user_grant(FCFSAuthClientContext *client_ctx,
        ConnectionInfo *conn, const string_t *username, const int64_t priv)
{
    FCFSAuthProtoHeader *header;
    FCFSAuthProtoUserGrantReq *req;
    char out_buff[sizeof(FCFSAuthProtoHeader) +
        sizeof(FCFSAuthProtoUserGrantReq) + NAME_MAX];
    SFResponseInfo response;
    int out_bytes;
    int result;

    header = (FCFSAuthProtoHeader *)out_buff;
    req = (FCFSAuthProtoUserGrantReq *)(header + 1);
    out_bytes = sizeof(FCFSAuthProtoHeader) + sizeof(*req) + username->len;
    SF_PROTO_SET_HEADER(header, FCFS_AUTH_SERVICE_PROTO_USER_GRANT_REQ,
            out_bytes - sizeof(FCFSAuthProtoHeader));
    if ((result=pack_username(username, &req->username)) != 0) {
        return result;
    }
    long2buff(priv, req->priv);

    response.error.length = 0;
    if ((result=sf_send_and_recv_none_body_response(conn, out_buff, out_bytes,
                    &response, client_ctx->common_cfg.network_timeout,
                    FCFS_AUTH_SERVICE_PROTO_USER_GRANT_RESP)) != 0)
    {
        sf_log_network_error(&response, conn, result);
    }

    return result;
}

int fcfs_auth_client_proto_user_remove(FCFSAuthClientContext *client_ctx,
        ConnectionInfo *conn, const string_t *username)
{
    FCFSAuthProtoHeader *header;
    FCFSAuthProtoUserRemoveReq *req;
    char out_buff[sizeof(FCFSAuthProtoHeader) +
        sizeof(FCFSAuthProtoUserRemoveReq) + NAME_MAX];
    SFResponseInfo response;
    int out_bytes;
    int result;

    header = (FCFSAuthProtoHeader *)out_buff;
    req = (FCFSAuthProtoUserRemoveReq *)(header + 1);
    out_bytes = sizeof(FCFSAuthProtoHeader) + sizeof(*req) + username->len;
    SF_PROTO_SET_HEADER(header, FCFS_AUTH_SERVICE_PROTO_USER_REMOVE_REQ,
            out_bytes - sizeof(FCFSAuthProtoHeader));
    if ((result=pack_username(username, &req->username)) != 0) {
        return result;
    }

    response.error.length = 0;
    if ((result=sf_send_and_recv_none_body_response(conn, out_buff, out_bytes,
                    &response, client_ctx->common_cfg.network_timeout,
                    FCFS_AUTH_SERVICE_PROTO_USER_REMOVE_RESP)) != 0)
    {
        sf_log_network_error(&response, conn, result);
    }

    return result;
}
