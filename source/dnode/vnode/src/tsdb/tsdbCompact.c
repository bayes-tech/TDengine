/*
 * Copyright (c) 2026 TAOS Data, Inc. <jhtao@taosdata.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef TD_ENTERPRISE
#include "tsdbCompact.h"
#include "tsdbInt.h"
#include "meta.h"

typedef struct {
  int32_t fid;
  SVATaskID task;
  int64_t size;
} SCompactTask;

struct SCompMonitor {
  SCompactVnodeReq req;
  SArray *tasks;
  int32_t killed;
  int32_t total;
  int32_t finished;
  int64_t totalSize;
  int64_t finishedSize;
  int64_t started;
  bool resume;
};

static bool compactCanceled(STsdb *tsdb) { return atomic_load_32(&tsdb->pCompMonitor->killed) != 0; }

static void compactStatePath(STsdb *tsdb, char *path) {
  vnodeGetPrimaryPath(tsdb->pVnode, false, path, TSDB_FILENAME_LEN);
  size_t offset = strlen(path);
  snprintf(path + offset, TSDB_FILENAME_LEN - offset, "%s%s-compact.state", TD_DIRSEP, tsdb->name);
}

// Store the request before acknowledging it. A restarted vnode retries an
// unfinished range after WAL recovery; rewriting a completed file set is safe.
// A terminal record retains the request identity for idempotent RPC retries.
static int32_t compactSaveState(STsdb *tsdb, bool pending) {
  SCompMonitor *m = tsdb->pCompMonitor;
  int32_t len = tSerializeSCompactVnodeReq(NULL, 0, &m->req);
  if (len < 0) return len;
  uint8_t *buf = taosMemoryMalloc(len + 1);
  if (buf == NULL) return terrno;
  buf[0] = pending ? 1 : 0;
  int32_t code = tSerializeSCompactVnodeReq(buf + 1, len, &m->req);
  char path[TSDB_FILENAME_LEN], tmp[TSDB_FILENAME_LEN];
  compactStatePath(tsdb, path);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  TdFilePtr fd = NULL;
  if (code < 0) goto _exit;
  code = 0;
  fd = taosOpenFile(tmp, TD_FILE_WRITE | TD_FILE_CREATE | TD_FILE_TRUNC);
  if (fd == NULL) { code = terrno; goto _exit; }
  if (taosWriteFile(fd, buf, len + 1) != len + 1) { code = TAOS_SYSTEM_ERROR(ERRNO); goto _exit; }
  code = taosFsyncFile(fd);
  if (code) goto _exit;
  code = taosCloseFile(&fd);
  if (code) goto _exit;
  code = taosRenameFile(tmp, path);
_exit:
  if (fd) (void)taosCloseFile(&fd);
  taosMemoryFree(buf);
  return code;
}

static int32_t compactLoadState(STsdb *tsdb) {
  char path[TSDB_FILENAME_LEN];
  compactStatePath(tsdb, path);
  if (!taosCheckExistFile(path)) return 0;
  TdFilePtr fd = taosOpenFile(path, TD_FILE_READ);
  if (fd == NULL) return terrno;
  uint8_t buf[1024];
  int64_t size = 0;
  int32_t code = taosFStatFile(fd, &size, NULL);
  if (code == 0 && (size < 2 || size > sizeof(buf))) code = TSDB_CODE_FILE_CORRUPTED;
  if (code == 0 && taosReadFile(fd, buf, size) != size) code = TSDB_CODE_FILE_CORRUPTED;
  (void)taosCloseFile(&fd);
  if (code) return code;
  if (buf[0] > 1) return TSDB_CODE_FILE_CORRUPTED;
  code = tDeserializeSCompactVnodeReq(buf + 1, size - 1, &tsdb->pCompMonitor->req);
  if (code) return code;
  if (tsdb->pCompMonitor->req.dbUid != tsdb->pVnode->config.dbId) return TSDB_CODE_FILE_CORRUPTED;
  tsdb->pCompMonitor->resume = buf[0] != 0;
  // Do not report a recovered pending task as complete before scheduling it.
  if (tsdb->pCompMonitor->resume) tsdb->pCompMonitor->total = 1;
  return 0;
}

bool tsdbShouldCompact(STFileSet *fset, int32_t vgId, int32_t expLevel, ETsdbOpType type) {
  // A dropped table changes metadata without committing a TSDB file. Always allow
  // the first compact; FORCE covers subsequent metadata-only changes.
  if (fset->lastCompact == 0 || fset->lastCompactCid == 0) return true;
  // Commits can finish during the rewrite. Their STT generation differs from
  // this rewrite even when their commit timestamp precedes lastCompact.
  for (int32_t i = 0; i < TSDB_FTYPE_MAX; ++i) {
    if (fset->farr[i] && fset->farr[i]->f->cid != fset->lastCompactCid) return true;
  }
  SSttLvl *lvl;
  TARRAY2_FOREACH(fset->lvlArr, lvl) {
    STFileObj *obj;
    TARRAY2_FOREACH(lvl->fobjArr, obj) {
      if (obj->f->cid != fset->lastCompactCid) return true;
    }
  }
  return false;
}

static int32_t compactAddIter(SCompactor2 *c, EIterType type, SSttFileReader *stt) {
  STsdbIterConfig config = {.type = type};
  if (stt) config.sttReader = stt;
  else config.dataReader = c->ctx->dataReader;
  STsdbIter *iter = NULL;
  TAOS_CHECK_RETURN(tsdbIterOpen(&config, &iter));
  bool tomb = type == TSDB_ITER_TYPE_DATA_TOMB || type == TSDB_ITER_TYPE_STT_TOMB;
  int32_t code = tomb ? TARRAY2_APPEND(c->ctx->tombIterArr, iter) : TARRAY2_APPEND(c->ctx->dataIterArr, iter);
  if (code) tsdbIterClose(&iter);
  return code;
}

static int32_t compactOpen(SCompactor2 *c) {
  SDataFileReaderConfig dataConfig = {.tsdb = c->tsdb, .szPage = c->szPage};
  for (int32_t i = 0; i < TSDB_FTYPE_MAX; ++i) {
    STFileObj *obj = c->fset->farr[i];
    if (obj == NULL) continue;
    dataConfig.files[i].exist = true;
    dataConfig.files[i].file = obj->f[0];
    STFileOp op = {.optype = TSDB_FOP_REMOVE, .fid = c->fset->fid, .of = obj->f[0]};
    TAOS_CHECK_RETURN(TARRAY2_APPEND(c->fopArr, op));
  }
  TAOS_CHECK_RETURN(tsdbDataFileReaderOpen(NULL, &dataConfig, &c->ctx->dataReader));
  TAOS_CHECK_RETURN(compactAddIter(c, TSDB_ITER_TYPE_DATA, NULL));
  TAOS_CHECK_RETURN(compactAddIter(c, TSDB_ITER_TYPE_DATA_TOMB, NULL));

  SSttLvl *lvl;
  TARRAY2_FOREACH(c->fset->lvlArr, lvl) {
    STFileObj *obj;
    TARRAY2_FOREACH(lvl->fobjArr, obj) {
      SSttFileReaderConfig config = {.tsdb = c->tsdb, .szPage = c->szPage, .file[0] = obj->f[0]};
      SSttFileReader *reader = NULL;
      TAOS_CHECK_RETURN(tsdbSttFileReaderOpen(obj->fname, &config, &reader));
      int32_t code = TARRAY2_APPEND(c->ctx->sttReaderArr, reader);
      if (code) {
        tsdbSttFileReaderClose(&reader);
        return code;
      }
      TAOS_CHECK_RETURN(compactAddIter(c, TSDB_ITER_TYPE_STT, reader));
      TAOS_CHECK_RETURN(compactAddIter(c, TSDB_ITER_TYPE_STT_TOMB, reader));
      STFileOp op = {.optype = TSDB_FOP_REMOVE, .fid = c->fset->fid, .of = obj->f[0]};
      TAOS_CHECK_RETURN(TARRAY2_APPEND(c->fopArr, op));
    }
  }
  TAOS_CHECK_RETURN(tsdbIterMergerOpen(c->ctx->dataIterArr, &c->ctx->dataIterMerger, false));
  TAOS_CHECK_RETURN(tsdbIterMergerOpen(c->ctx->tombIterArr, &c->ctx->tombIterMerger, true));

  SVnodeCfg *cfg = &c->tsdb->pVnode->config;
  SFSetWriterConfig config = {.tsdb = c->tsdb, .compactVersion = INT64_MAX,
                              .minRow = cfg->tsdbCfg.minRows, .maxRow = cfg->tsdbCfg.maxRows,
                              .szPage = c->szPage, .cmprAlg = cfg->tsdbCfg.compression,
                              .fid = c->fset->fid, .cid = c->cid, .expLevel = c->ctx->expLevel,
                              // A nonzero writer option marks new data as compacted (file lcn=0).
                              .lcn = 1, .throttle = tsdbCompactThrottle()};
  return tsdbFSetWriterOpen(&config, &c->ctx->writer);
}

static int32_t compactTableCompare(const TABLEID *a, const TABLEID *b) {
  if (a->suid != b->suid) return a->suid < b->suid ? -1 : 1;
  return a->uid < b->uid ? -1 : a->uid > b->uid;
}

// Keep tombstones in the output: commits into new STT files may run concurrently
// and may contain older versions not present in this input snapshot.
static int32_t compactTombs(SCompactor2 *c, const TABLEID *table, SArray *deletes) {
  STombRecord *tomb;
  while ((tomb = tsdbIterMergerGetTombRecord(c->ctx->tombIterMerger)) != NULL) {
    TABLEID id = {.suid = tomb->suid, .uid = tomb->uid};
    int32_t cmp = table ? compactTableCompare(&id, table) : -1;
    if (cmp > 0) break;
    SMetaInfo info;
    int32_t metaCode = metaGetInfo(c->tsdb->pVnode->pMeta, id.uid, &info, NULL);
    if (metaCode != 0 && metaCode != TSDB_CODE_NOT_FOUND) return metaCode;
    if (metaCode == 0) {
      TAOS_CHECK_RETURN(tsdbFSetWriteTombRecord(c->ctx->writer, tomb));
      if (cmp == 0) {
        SDelData del = {.version = tomb->version, .sKey = tomb->skey, .eKey = tomb->ekey};
        if (taosArrayPush(deletes, &del) == NULL) return terrno;
      }
    }
    if (compactCanceled(c->tsdb)) return TSDB_CODE_TSC_QUERY_CANCELLED;
    TAOS_CHECK_RETURN(tsdbIterMergerNext(c->ctx->tombIterMerger));
  }
  return 0;
}

static bool compactRowDeleted(SCompactor2 *c, const TSDBROW *row) {
  // Skyline ranges have inclusive endpoints. A boundary can be covered by both
  // neighbours, so inspect both at equality (also handles repeated timestamps/PKs).
  TSDBKEY key = TSDBROW_KEY(row);
  int32_t n = taosArrayGetSize(c->ctx->aSkyLine);
  while (c->ctx->iSkyLine + 1 < n &&
         ((TSDBKEY *)taosArrayGet(c->ctx->aSkyLine, c->ctx->iSkyLine + 1))->ts < key.ts) {
    ++c->ctx->iSkyLine;
  }
  for (int32_t i = c->ctx->iSkyLine; i + 1 < n; ++i) {
    TSDBKEY *a = taosArrayGet(c->ctx->aSkyLine, i);
    TSDBKEY *b = taosArrayGet(c->ctx->aSkyLine, i + 1);
    if (a->ts > key.ts) break;
    if (key.ts <= b->ts && key.version <= a->version) return true;
  }
  return false;
}

static int32_t compactRows(SCompactor2 *c) {
  int32_t code = 0;
  SArray *deletes = taosArrayInit(16, sizeof(SDelData));
  c->ctx->aSkyLine = taosArrayInit(16, sizeof(TSDBKEY));
  if (deletes == NULL || c->ctx->aSkyLine == NULL) {
    code = terrno;
    goto _exit;
  }
  SRowInfo *row;
  while ((row = tsdbIterMergerGetData(c->ctx->dataIterMerger)) != NULL) {
    if (compactCanceled(c->tsdb)) { code = TSDB_CODE_TSC_QUERY_CANCELLED; goto _exit; }
    if (row->uid != c->ctx->tbid->uid || row->suid != c->ctx->tbid->suid) {
      *c->ctx->tbid = (TABLEID){.suid = row->suid, .uid = row->uid};
      SMetaInfo info;
      int32_t metaCode = metaGetInfo(c->tsdb->pVnode->pMeta, row->uid, &info, NULL);
      if (metaCode != 0 && metaCode != TSDB_CODE_NOT_FOUND) { code = metaCode; goto _exit; }
      if (metaCode == TSDB_CODE_NOT_FOUND) {
        code = tsdbIterMergerSkipTableData(c->ctx->dataIterMerger, c->ctx->tbid);
        if (code) goto _exit;
        c->ctx->tbid->uid = 0;
        continue;
      }
      taosArrayClear(deletes);
      taosArrayClear(c->ctx->aSkyLine);
      c->ctx->iSkyLine = 0;
      code = compactTombs(c, c->ctx->tbid, deletes);
      if (code) goto _exit;
      if (taosArrayGetSize(deletes)) {
        code = tsdbBuildDeleteSkyline(deletes, 0, taosArrayGetSize(deletes) - 1, c->ctx->aSkyLine);
        if (code) goto _exit;
      }
    }
    if (!compactRowDeleted(c, &row->row)) {
      code = tsdbFSetWriteRow(c->ctx->writer, row);
      if (code) goto _exit;
    }
    code = tsdbIterMergerNext(c->ctx->dataIterMerger);
    if (code) goto _exit;
  }
  code = compactTombs(c, NULL, deletes);
_exit:
  taosArrayDestroy(deletes);
  return code;
}

static void compactCloseReaders(SCompactor2 *c) {
  tsdbIterMergerClose(&c->ctx->dataIterMerger);
  tsdbIterMergerClose(&c->ctx->tombIterMerger);
  TARRAY2_DESTROY(c->ctx->dataIterArr, tsdbIterClose);
  TARRAY2_DESTROY(c->ctx->tombIterArr, tsdbIterClose);
  TARRAY2_DESTROY(c->ctx->sttReaderArr, tsdbSttFileReaderClose);
  tsdbDataFileReaderClose(&c->ctx->dataReader);
}

static void compactCleanup(SCompactor2 *c) {
  if (c->ctx->writer) (void)tsdbFSetWriterClose(&c->ctx->writer, true, NULL);
  compactCloseReaders(c);
  taosArrayDestroy(c->ctx->aSkyLine);
  TARRAY2_DESTROY(c->fopArr, NULL);
  tsdbTFileSetClear(&c->fset);
}

static int32_t compactFileSet(SCompactArg *arg) {
  STsdb *tsdb = arg->tsdb;
  SCompactor2 c = {.tsdb = tsdb, .szPage = tsdb->pVnode->config.tsdbPageSize};
  STFileSet *fset = NULL;
  bool acquired = false;
  int32_t code = 0;
  (void)taosThreadMutexLock(&tsdb->mutex);
  if (!tsdb->bgTaskDisabled && !compactCanceled(tsdb)) {
    tsdbBeginTaskOnFileSet(tsdb, arg->fid, EVA_TASK_COMPACT, &fset);
    acquired = fset != NULL;
    if (fset && (arg->force || tsdbShouldCompact(fset, TD_VID(tsdb->pVnode), 0, arg->type))) {
      code = tsdbTFileSetInitCopy(tsdb, fset, &c.fset);
    }
  }
  (void)taosThreadMutexUnlock(&tsdb->mutex);
  if (code || c.fset == NULL) goto _exit;
  c.cid = tsdbFSAllocEid(tsdb->pFS);
  c.ctx->expLevel = TMAX(0, tsdbFidLevel(arg->fid, &tsdb->keepCfg, taosGetTimestampSec()));
  code = compactOpen(&c);
  if (code) goto _exit;
  code = compactRows(&c);
  if (code) goto _exit;
  code = tsdbFSetWriterClose(&c.ctx->writer, false, c.fopArr);
  if (code) goto _exit;
  compactCloseReaders(&c);
  STFileOp *op;
  TARRAY2_FOREACH_PTR(c.fopArr, op) { op->compactCid = c.cid; }
  if (compactCanceled(tsdb)) { code = TSDB_CODE_TSC_QUERY_CANCELLED; goto _exit; }
  code = tsdbFSEditBegin(tsdb->pFS, c.fopArr, TSDB_FEDIT_COMPACT);
  if (code) goto _exit;
  (void)taosThreadMutexLock(&tsdb->mutex);
  code = tsdbFSEditCommit(tsdb->pFS);
  (void)taosThreadMutexUnlock(&tsdb->mutex);
  if (code) {
    // CURRENT may already have been renamed; follow the existing commit/merge
    // recovery policy rather than continuing with potentially stale handles.
    tsdbFatal("vgId:%d, compact commit failed since %s", TD_VID(tsdb->pVnode), tstrerror(code));
    exit(EXIT_FAILURE);
  }
_exit:
  if (code) {
    // Closed output files have not entered CURRENT on this path. Remove them
    // as well as files still owned by an aborted writer.
    STFileOp *op;
    TARRAY2_FOREACH_PTR(c.fopArr, op) {
      if (op->optype == TSDB_FOP_CREATE && op->nf.cid == c.cid) {
        char path[TSDB_FILENAME_LEN];
        tsdbTFileName(tsdb, &op->nf, path);
        tsdbRemoveFile(path);
      }
    }
  }
  if (acquired) {
    (void)taosThreadMutexLock(&tsdb->mutex);
    tsdbFinishTaskOnFileSet(tsdb, arg->fid, EVA_TASK_COMPACT);
    (void)taosThreadMutexUnlock(&tsdb->mutex);
  }
  compactCleanup(&c);
  if (code && code != TSDB_CODE_TSC_QUERY_CANCELLED) {
    tsdbError("vgId:%d compact fid:%d failed since %s", TD_VID(tsdb->pVnode), arg->fid, tstrerror(code));
  }
  return code;
}

static void compactTaskDone(SCompactArg *arg, bool finished) {
  STsdb *tsdb = arg->tsdb;
  (void)taosThreadMutexLock(&tsdb->mutex);
  SCompMonitor *m = tsdb->pCompMonitor;
  if (!finished && !compactCanceled(tsdb)) m->resume = true;
  for (int32_t i = 0; i < taosArrayGetSize(m->tasks); ++i) {
    SCompactTask *t = taosArrayGet(m->tasks, i);
    if (t->task.id == arg->taskid.id && t->task.async == arg->taskid.async) {
      if (finished) { ++m->finished; m->finishedSize += t->size; }
      taosArrayRemove(m->tasks, i);
      break;
    }
  }
  if (m->finished == m->total && !m->resume && !tsdb->bgTaskDisabled) {
    int32_t code = compactSaveState(tsdb, false);
    if (code) tsdbError("vgId:%d, failed to save compact completion since %s", TD_VID(tsdb->pVnode), tstrerror(code));
  }
  (void)taosThreadMutexUnlock(&tsdb->mutex);
  taosMemoryFree(arg);
}

// Cancellation callbacks run outside the async scheduler's mutex.
static void compactTaskCancel(void *arg) { compactTaskDone(arg, true); }
static int32_t compactTaskRun(void *ptr) {
  SCompactArg *arg = ptr;
  int32_t code = compactFileSet(arg);
  compactTaskDone(arg, code == 0 || code == TSDB_CODE_TSC_QUERY_CANCELLED);
  return code;
}

static int32_t compactSchedule(STsdb *tsdb, const STimeWindow *tw, ETsdbOpType type, bool force) {
  SCompMonitor *m = tsdb->pCompMonitor;
  int32_t min = tsdbKeyFid(tw->skey, tsdb->keepCfg.days, tsdb->keepCfg.precision);
  int32_t max = tsdbKeyFid(tw->ekey, tsdb->keepCfg.days, tsdb->keepCfg.precision);
  STFileSet *fset;
  TARRAY2_FOREACH(tsdb->pFS->fSetArr, fset) {
    if (fset->fid < min || fset->fid > max) continue;
    SCompactArg *arg = taosMemoryCalloc(1, sizeof(*arg));
    if (arg == NULL) return terrno;
    arg->tsdb = tsdb;
    arg->fid = fset->fid;
    arg->type = type;
    arg->force = force;
    // Reserve monitor space before exposing the task to worker threads.
    SCompactTask task = {.fid = fset->fid, .size = tsdbTFileSetGetDataSize(fset)};
    if (taosArrayPush(m->tasks, &task) == NULL) { taosMemoryFree(arg); return terrno; }
    int32_t code = vnodeAsync(COMPACT_TASK_ASYNC, EVA_PRIORITY_LOW, compactTaskRun, compactTaskCancel, arg, &arg->taskid);
    if (code) {
      taosArrayRemove(m->tasks, taosArrayGetSize(m->tasks) - 1);
      taosMemoryFree(arg);
      return code;
    }
    fset->compactTask = arg->taskid;
    ((SCompactTask *)taosArrayGetLast(m->tasks))->task = arg->taskid;
    ++m->total;
    m->totalSize += task.size;
  }
  return 0;
}

int32_t tsdbOpenCompMonitor(STsdb *tsdb) {
  tsdb->pCompMonitor = taosMemoryCalloc(1, sizeof(SCompMonitor));
  if (tsdb->pCompMonitor == NULL) return terrno;
  tsdb->pCompMonitor->tasks = taosArrayInit(16, sizeof(SCompactTask));
  if (tsdb->pCompMonitor->tasks == NULL) return terrno;
  return compactLoadState(tsdb);
}

void tsdbCloseCompMonitor(STsdb *tsdb) {
  if (tsdb->pCompMonitor) {
    taosArrayDestroy(tsdb->pCompMonitor->tasks);
    taosMemoryFreeClear(tsdb->pCompMonitor);
  }
}

int32_t tsdbStartCompact(STsdb *tsdb, const SCompactVnodeReq *req) {
  (void)taosThreadMutexLock(&tsdb->mutex);
  SCompMonitor *m = tsdb->pCompMonitor;
  int32_t code = 0;
  bool same = m->req.compactId == req->compactId && m->req.compactStartTime == req->compactStartTime;
  if (same && !m->resume) goto _exit;
  if (taosArrayGetSize(m->tasks) || tsdb->bgTaskDisabled) { code = TSDB_CODE_NEED_RETRY; goto _exit; }
  m->req = *req;
  m->total = m->finished = 0;
  m->totalSize = m->finishedSize = 0;
  m->started = taosGetTimestampSec();
  atomic_store_32(&m->killed, 0);
  code = compactSaveState(tsdb, true);
  if (code) { m->resume = true; goto _exit; }
  code = req->metaOnly ? 0 : compactSchedule(tsdb, &req->tw, TSDB_OPTR_NORMAL, req->force);
  m->resume = code != 0;
  if (code == 0 && m->total == 0) code = compactSaveState(tsdb, false);
_exit:
  (void)taosThreadMutexUnlock(&tsdb->mutex);
  return code;
}

int32_t tsdbAsyncCompact(STsdb *tsdb, const STimeWindow *tw, ETsdbOpType type, bool force) {
  SCompactVnodeReq req = {.dbUid = tsdb->pVnode->config.dbId,
                          .tw = *tw, .force = force, .compactStartTime = taosGetTimestampMs()};
  return tsdbStartCompact(tsdb, &req);
}

int32_t tsdbResumeCompact(STsdb *tsdb) {
  if (!tsdb->pVnode->restored) return 0;
  SCompactVnodeReq req;
  (void)taosThreadMutexLock(&tsdb->mutex);
  bool resume = tsdb->pCompMonitor->resume;
  req = tsdb->pCompMonitor->req;
  (void)taosThreadMutexUnlock(&tsdb->mutex);
  return resume ? tsdbStartCompact(tsdb, &req) : 0;
}

void tsdbCompactGetProgress(STsdb *tsdb, SQueryCompactProgressRsp *rsp) {
  // Progress polling also retries work that could not be scheduled or hit an
  // I/O/allocation error. It remains unfinished while retrying.
  (void)tsdbResumeCompact(tsdb);
  (void)taosThreadMutexLock(&tsdb->mutex);
  SCompMonitor *m = tsdb->pCompMonitor;
  if (rsp->compactId != 0 && rsp->compactId != m->req.compactId) {
    rsp->compactId = 0;
    (void)taosThreadMutexUnlock(&tsdb->mutex);
    return;
  }
  rsp->compactId = m->req.compactId;
  rsp->vgId = TD_VID(tsdb->pVnode);
  rsp->numberFileset = m->total + (m->resume ? 1 : 0);
  rsp->finished = m->finished;
  rsp->progress = m->finished == rsp->numberFileset ? 100 : (m->totalSize ? m->finishedSize * 100 / m->totalSize : 0);
  int64_t elapsed = taosGetTimestampSec() - m->started;
  rsp->remainingTime = rsp->progress > 0 ? elapsed * (100 - rsp->progress) / rsp->progress : 0;
  (void)taosThreadMutexUnlock(&tsdb->mutex);
}

int32_t tsdbKillCompact(STsdb *tsdb, int32_t compactId) {
  if (tsdb->pCompMonitor == NULL) return 0;
  int32_t code = 0;
  (void)taosThreadMutexLock(&tsdb->mutex);
  if (compactId == 0 || tsdb->pCompMonitor->req.compactId == compactId) {
    atomic_store_32(&tsdb->pCompMonitor->killed, 1);
    SCompMonitor *m = tsdb->pCompMonitor;
    m->resume = false;
    // An I/O failure can leave an unfinished task with no running workers.
    // KILL must also be able to clean up that state.
    m->total = m->finished + taosArrayGetSize(m->tasks);
    if (!tsdb->bgTaskDisabled) code = compactSaveState(tsdb, false);
  }
  (void)taosThreadMutexUnlock(&tsdb->mutex);
  return code;
}

void tsdbStopAllCompTask(STsdb *tsdb) { (void)tsdbKillCompact(tsdb, 0); }
#endif
