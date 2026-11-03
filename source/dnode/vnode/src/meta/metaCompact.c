/*
 * Copyright (c) 2026 TAOS Data, Inc. <jhtao@taosdata.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef TD_ENTERPRISE
#include "meta.h"

// Reclaim obsolete table entries through the normal TDB transaction/journal.
// Schema history is deliberately retained: persisted TSDB rows can still refer
// to older schema versions. Freed B-tree pages are available for subsequent DDL.
int32_t metaCompact(SMeta *meta) {
  int32_t code = 0;
  TBC *cursor = NULL;
  SArray *keys = taosArrayInit(1024, sizeof(STbDbKey));
  STbDbKey resume = {0};
  bool haveResume = false, done = false;
  if (keys == NULL) return terrno;
  metaWLock(meta);
  if (metaHasPendingTxnEntries(meta)) { code = TSDB_CODE_NEED_RETRY; goto _exit; }
  while (!done) {
    code = tdbTbcOpen(meta->pTbDb, &cursor, NULL);
    if (code) goto _exit;
    if (haveResume) {
      int cmp = 0;
      code = tdbTbcMoveTo(cursor, &resume, sizeof(resume), &cmp);
      if (code == 0 && cmp >= 0 && tdbTbcIsValid(cursor)) code = tdbTbcMoveToNext(cursor);
    } else {
      code = tdbTbcMoveToFirst(cursor);
    }
    if (code == TSDB_CODE_NOT_FOUND) { code = 0; done = true; }
    if (code) goto _exit;
    taosArrayClear(keys);
    while (!done && taosArrayGetSize(keys) < 1024) {
      if (!tdbTbcIsValid(cursor)) { done = true; break; }
      const void *key = NULL, *value = NULL;
      int klen = 0, vlen = 0;
      code = tdbTbcGet(cursor, &key, &klen, &value, &vlen);
      if (code == TSDB_CODE_NOT_FOUND) { code = 0; done = true; break; }
      if (code) goto _exit;
      if (klen != sizeof(STbDbKey)) { code = TSDB_CODE_FILE_CORRUPTED; goto _exit; }
      memcpy(&resume, key, sizeof(resume));
      haveResume = true;
      void *current = NULL;
      int len = 0;
      int32_t ret = tdbTbGet(meta->pUidIdx, &resume.uid, sizeof(resume.uid), &current, &len);
      bool obsolete = ret == TSDB_CODE_NOT_FOUND;
      if (ret == 0) {
        if (len != sizeof(SUidIdxVal)) { tdbFree(current); code = TSDB_CODE_FILE_CORRUPTED; goto _exit; }
        obsolete = ((SUidIdxVal *)current)->version != resume.version;
      } else if (!obsolete) { tdbFree(current); code = ret; goto _exit; }
      tdbFree(current);
      if (obsolete && taosArrayPush(keys, &resume) == NULL) { code = terrno; goto _exit; }
      code = tdbTbcMoveToNext(cursor);
      if (code == TSDB_CODE_NOT_FOUND) { code = 0; done = true; }
      if (code) goto _exit;
    }
    tdbTbcClose(cursor);
    cursor = NULL;
    for (int32_t i = 0; i < taosArrayGetSize(keys); ++i) {
      code = tdbTbDelete(meta->pTbDb, taosArrayGet(keys, i), sizeof(STbDbKey), meta->txn);
      if (code) goto _exit;
      meta->changed = true;
    }
  }
_exit:
  if (cursor) tdbTbcClose(cursor);
  metaULock(meta);
  taosArrayDestroy(keys);
  return code;
}
#endif
