/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef TD_TSDB_COMPACT_H
#define TD_TSDB_COMPACT_H
#include "tsdb.h"

#ifndef TD_ENTERPRISE
int32_t tsdbOpenCompMonitor(STsdb *tsdb);
void    tsdbCloseCompMonitor(STsdb *tsdb);
int32_t tsdbStartCompact(STsdb *tsdb, const SCompactVnodeReq *req);
int32_t tsdbResumeCompact(STsdb *tsdb);
void    tsdbCompactGetProgress(STsdb *tsdb, SQueryCompactProgressRsp *rsp);
int32_t tsdbKillCompact(STsdb *tsdb, int32_t compactId);
bool    tsdbShouldCompact(STFileSet *fset, int64_t metaVersion);
int32_t tsdbPauseCompact(STsdb *tsdb, SArray *tasks);
#endif
#endif
