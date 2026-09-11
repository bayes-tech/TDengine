/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
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
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _TD_VNODE_TSDB_INT_H_
#define _TD_VNODE_TSDB_INT_H_

#include "../tsdb/tsdbDataFileRW.h"
#include "../tsdb/tsdbFS2.h"
#include "../tsdb/tsdbFSetRW.h"
#include "../tsdb/tsdbIter.h"
#include "../tsdb/tsdbSttFileRW.h"

#ifdef __cplusplus
extern "C" {
#endif

// operation type of a tsdb background task, only TSDB_OPTR_NORMAL is used by the
// community data-compact implementation.
typedef enum {
  TSDB_OPTR_NORMAL = 0,
  TSDB_OPTR_ROLLUP,
  TSDB_OPTR_SSMIGRATE,
} ETsdbOpType;

// compactor context shared by the per-fileset compact steps
typedef struct {
  STsdb  *tsdb;
  int32_t szPage;
  int32_t minRow;
  int32_t maxRow;
  int8_t  cmprAlg;
  int8_t  optrType;  // ETsdbOpType
  int64_t cid;
  int64_t compactVersion;

  STFileSet   *fset;
  TFileOpArray fopArr[1];

  struct {
    int32_t expLevel;

    // reader
    SDataFileReader     *dataReader;
    TSttFileReaderArray  sttReaderArr[1];

    // iter & merger
    TTsdbIterArray dataIterArr[1];
    SIterMerger   *dataIterMerger;
    TTsdbIterArray tombIterArr[1];
    SIterMerger   *tombIterMerger;

    // writer
    SFSetWriter *writer;

    TABLEID   tbid[1];
    SHashObj *pKeepHashObj;  // SHashObj<suid, keep>

    // skyline of delete ranges
    SArray  *aSkyLine;
    int32_t  iSkyLine;
    TSDBKEY *pDKey;
    TSDBKEY  dKey;
  } ctx[1];
} SCompactor2;

typedef struct {
  STsdb      *tsdb;
  int32_t     fid;
  ETsdbOpType type;
  bool        force;
  SVATaskID   taskid;
} SCompactArg;

// tsdbCompact.c
int32_t tsdbAsyncCompact(STsdb *tsdb, const STimeWindow *tw, ETsdbOpType type, bool force);
bool    tsdbShouldCompact(const STFileSet *fset, int32_t vgId);
void    tsdbAlterNumCompactThreads(void);

// tsdbCompactMonitor.c
bool    tsdbCompMonHasTask(STsdb *tsdb);
int32_t tsdbOpenCompMonitor(STsdb *tsdb);
void    tsdbCloseCompMonitor(STsdb *tsdb);
int32_t tsdbAddCompMonitorTask(STsdb *tsdb, int32_t fid, SVATaskID *taskId, int64_t compactSize);
int32_t tsdbUpdateCompMonitorTask(STsdb *tsdb, int32_t fid, SVATaskID *taskId, int64_t compactSize);
void    tsdbStopAllCompTask(STsdb *tsdb);
int32_t tsdbCompMonitorGetInfo(STsdb *tsdb, SQueryCompactProgressRsp *rsp);
int32_t tsdbCompMonitorGetKilled(STsdb *tsdb);
void    tsdbRemoveCompMonitorTask(STsdb *tsdb, SVATaskID *taskId);

#ifdef __cplusplus
}
#endif

#endif /*_TD_VNODE_TSDB_INT_H_*/
