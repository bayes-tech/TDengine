/*
 * Copyright (c) 2026 TAOS Data, Inc. <jhtao@taosdata.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef TD_ENTERPRISE
#include "vnd.h"
#include "../tsdb/tsdbCompact.h"
#include "meta.h"

extern int32_t metaCompact(SMeta *meta);

int32_t vnodeAsyncCompact(SVnode *vnode, int64_t ver, void *buf, int32_t len, SRpcMsg *rsp) {
  SCompactVnodeReq req = {0};
  TAOS_CHECK_RETURN(tDeserializeSCompactVnodeReq(buf, len, &req));
  if (req.dbUid != vnode->config.dbId || req.tw.skey > req.tw.ekey) return TSDB_CODE_INVALID_MSG;
  // A flush request acknowledges scheduling, so wait for the preceding commit
  // and flush the current memtable before taking the compact input snapshot.
  TAOS_CHECK_RETURN(vnodeSyncCommit(vnode));
  TAOS_CHECK_RETURN(vnodeBegin(vnode));
  if (req.metaOnly) {
    // The replicated write queue serializes DDL and commits. Readers take the
    // metadata lock; reclaiming historical entries cannot race a metadata reader.
    TAOS_CHECK_RETURN(metaCompact(vnode->pMeta));
    TAOS_CHECK_RETURN(vnodeSyncCommit(vnode));
    TAOS_CHECK_RETURN(vnodeBegin(vnode));
  }
  return tsdbStartCompact(vnode->pTsdb, &req);
}

int32_t vnodeGetCompactProgress(SVnode *vnode, int32_t compactId, SQueryCompactProgressRsp *rsp) {
  rsp->compactId = compactId;
  tsdbCompactGetProgress(vnode->pTsdb, rsp);
  return 0;
}

int32_t vnodeQueryCompactProgress(SVnode *vnode, SRpcMsg *msg) {
  SQueryCompactProgressReq req = {0};
  SQueryCompactProgressRsp rsp = {0};
  SRpcMsg reply = {.info = msg->info, .msgType = TDMT_VND_QUERY_COMPACT_PROGRESS_RSP};
  int32_t code = tDeserializeSQueryCompactProgressReq(msg->pCont, msg->contLen, &req);
  if (code == 0) {
    rsp.compactId = req.compactId;
    rsp.dnodeId = req.dnodeId;
    tsdbCompactGetProgress(vnode->pTsdb, &rsp);
    int32_t size = tSerializeSQueryCompactProgressRsp(NULL, 0, &rsp);
    if (size < 0) code = size;
    else if ((reply.pCont = rpcMallocCont(size)) == NULL) code = terrno;
    else {
      code = tSerializeSQueryCompactProgressRsp(reply.pCont, size, &rsp);
      if (code >= 0) { reply.contLen = size; code = 0; }
    }
  }
  reply.code = code;
  tmsgSendRsp(&reply);
  return 0;
}

int32_t vnodeProcessKillCompactReq(SVnode *vnode, int64_t ver, void *buf, int32_t len, SRpcMsg *rsp) {
  SVKillCompactReq req = {0};
  TAOS_CHECK_RETURN(tDeserializeSVKillCompactReq(buf, len, &req));
  TAOS_CHECK_RETURN(tsdbKillCompact(vnode->pTsdb, req.compactId));
  rsp->msgType = TDMT_VND_KILL_COMPACT_RSP;
  rsp->code = 0;
  return 0;
}
#endif
