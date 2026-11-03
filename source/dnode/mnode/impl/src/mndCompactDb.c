/*
 * Copyright (c) 2026 TAOS Data, Inc. <jhtao@taosdata.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef TD_ENTERPRISE
#include "mndCompact.h"
#include "mndCompactDetail.h"
#include "mndDb.h"
#include "mndPrivilege.h"
#include "mndTrans.h"
#include "mndUser.h"
#include "mndVgroup.h"

static int32_t mndCompactVgroup(SMnode *mnode, STrans *trans, SDbObj *db, SVgObj *vg,
                               SCompactObj *compact, const SCompactDbReq *req, int32_t *detailId) {
  SCompactVnodeReq vnodeReq = {.dbUid = db->uid,
                               .compactId = compact->compactId,
                               .compactStartTime = compact->startTime,
                               .tw = req->timeRange,
                               .metaOnly = req->metaOnly,
                               .force = req->force};
  tstrncpy(vnodeReq.db, db->name, sizeof(vnodeReq.db));
  int32_t len = tSerializeSCompactVnodeReq(NULL, 0, &vnodeReq);
  if (len < 0) return len;
  int32_t total = len + sizeof(SMsgHead);
  SMsgHead *head = taosMemoryMalloc(total);
  if (head == NULL) return terrno;
  head->contLen = htonl(total);
  head->vgId = htonl(vg->vgId);
  int32_t code = tSerializeSCompactVnodeReq(head + 1, len, &vnodeReq);
  if (code < 0) {
    taosMemoryFree(head);
    return code;
  }
  STransAction action = {.epSet = mndGetVgroupEpset(mnode, vg),
                          .pCont = head, .contLen = total, .msgType = TDMT_VND_COMPACT};
  code = mndTransAppendRedoAction(trans, &action);
  if (code) {
    taosMemoryFree(head);
    return code;
  }
  for (int32_t i = 0; i < vg->replica; ++i) {
    TAOS_CHECK_RETURN(mndAddCompactDetailToTran(mnode, trans, compact, vg, &vg->vnodeGid[i], (*detailId)++));
  }
  return 0;
}

int32_t mndProcessCompactDbReq(SRpcMsg *rpc) {
  SMnode *mnode = rpc->info.node;
  SCompactDbReq req = {0};
  SCompactDbRsp rsp = {0};
  SCompactObj compact = {0};
  SDbObj *db = NULL;
  STrans *trans = NULL;
  int32_t code = 0, lino = 0, detailId = 0;
  void *iter = NULL;

  TAOS_CHECK_EXIT(tDeserializeSCompactDbReq(rpc->pCont, rpc->contLen, &req));
  if (req.timeRange.skey > req.timeRange.ekey) TAOS_CHECK_EXIT(TSDB_CODE_INVALID_PARA);
  db = mndAcquireDb(mnode, req.db);
  if (db == NULL) TAOS_CHECK_EXIT(terrno);
  TAOS_CHECK_EXIT(mndCheckDbPrivilege(mnode, RPC_MSG_USER(rpc), RPC_MSG_TOKEN(rpc), MND_OPER_COMPACT_DB, db));
  if (db->cfg.isMount) TAOS_CHECK_EXIT(TSDB_CODE_MND_MOUNT_OBJ_NOT_SUPPORT);

  // Validate the whole list before adding any logs or actions, including cross-database IDs.
  for (int32_t i = 0; i < taosArrayGetSize(req.vgroupIds); ++i) {
    int64_t id = *(int64_t *)taosArrayGet(req.vgroupIds, i);
    SVgObj *vg = id > 0 && id <= INT32_MAX ? mndAcquireVgroup(mnode, id) : NULL;
    bool valid = vg != NULL && vg->dbUid == db->uid;
    if (vg) mndReleaseVgroup(mnode, vg);
    if (!valid) TAOS_CHECK_EXIT(TSDB_CODE_MND_VGROUP_NOT_EXIST);
  }

  while (true) {
    SCompactObj *running = NULL;
    iter = sdbFetch(mnode->pSdb, SDB_COMPACT, iter, (void **)&running);
    if (iter == NULL) break;
    bool same = running->dbUid == db->uid;
    sdbRelease(mnode->pSdb, running);
    if (same) {
      sdbCancelFetch(mnode->pSdb, iter);
      iter = NULL;
      code = mndDeferCompactReq(mnode, rpc);
      goto _exit;
    }
  }

  trans = mndTransCreate(mnode, TRN_POLICY_RETRY, TRN_CONFLICT_DB, rpc, "compact-db");
  if (trans == NULL) TAOS_CHECK_EXIT(terrno);
  mndTransSetDbName(trans, db->name, NULL);
  TAOS_CHECK_EXIT(mndTransCheckConflict(mnode, trans));
  TAOS_CHECK_EXIT(mndTransCheckConflictWithCompact(mnode, trans));
  TAOS_CHECK_EXIT(mndAddCompactToTran(mnode, trans, &compact, db, &rsp));

  while (true) {
    SVgObj *vg = NULL;
    iter = sdbFetch(mnode->pSdb, SDB_VGROUP, iter, (void **)&vg);
    if (iter == NULL) break;
    bool selected = taosArrayGetSize(req.vgroupIds) == 0;
    for (int32_t i = 0; !selected && i < taosArrayGetSize(req.vgroupIds); ++i) {
      selected = vg->vgId == *(int64_t *)taosArrayGet(req.vgroupIds, i);
    }
    if (vg->dbUid == db->uid && selected) code = mndCompactVgroup(mnode, trans, db, vg, &compact, &req, &detailId);
    sdbRelease(mnode->pSdb, vg);
    if (code) {
      sdbCancelFetch(mnode->pSdb, iter);
      iter = NULL;
      goto _exit;
    }
  }

  rsp.bAccepted = true;
  int32_t len = tSerializeSCompactDbRsp(NULL, 0, &rsp);
  if (len < 0) TAOS_CHECK_EXIT(len);
  void *buf = taosMemoryMalloc(len);
  if (buf == NULL) TAOS_CHECK_EXIT(terrno);
  code = tSerializeSCompactDbRsp(buf, len, &rsp);
  if (code < 0) {
    taosMemoryFree(buf);
    goto _exit;
  }
  code = 0;
  mndTransSetRpcRsp(trans, buf, len);
  TAOS_CHECK_EXIT(mndTransPrepare(mnode, trans));
  code = TSDB_CODE_ACTION_IN_PROGRESS;

_exit:
  if (code && code != TSDB_CODE_ACTION_IN_PROGRESS) {
    mError("db:%s, compact request failed at line %d since %s", req.db, lino, tstrerror(code));
  }
  mndTransDrop(trans);
  mndReleaseDb(mnode, db);
  tFreeSCompactDbReq(&req);
  return code;
}
#endif
