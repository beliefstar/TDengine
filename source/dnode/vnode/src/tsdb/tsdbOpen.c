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

#include "tsdb.h"
#include "tsdbFS2.h"

int32_t tsdbSetKeepCfg(STsdb *pTsdb, STsdbCfg *pCfg) {
  STsdbKeepCfg *pKeepCfg = &pTsdb->keepCfg;
  pKeepCfg->precision = pCfg->precision;
  pKeepCfg->days = pCfg->days;
  pKeepCfg->keep0 = pCfg->keep0;
  pKeepCfg->keep1 = pCfg->keep1;
  pKeepCfg->keep2 = pCfg->keep2;
  pKeepCfg->keepTimeOffset = pCfg->keepTimeOffset;
  return 0;
}

/**
 * @brief
 *
 * @param pVnode
 * @param ppTsdb
 * @param dir
 * @return int
 */
int tsdbOpen(SVnode *pVnode, STsdb **ppTsdb, const char *dir, STsdbKeepCfg *pKeepCfg, int8_t rollback) {
  STsdb *pTsdb = NULL;
  int    slen = 0;

  *ppTsdb = NULL;
  slen = strlen(pVnode->path) + strlen(dir) + 2;

  // create handle
  pTsdb = (STsdb *)taosMemoryCalloc(1, sizeof(*pTsdb) + slen);
  if (pTsdb == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  pTsdb->path = (char *)&pTsdb[1];
  snprintf(pTsdb->path, TD_PATH_MAX, "%s%s%s", pVnode->path, TD_DIRSEP, dir);
  // taosRealPath(pTsdb->path, NULL, slen);
  pTsdb->pVnode = pVnode;
  taosThreadRwlockInit(&pTsdb->rwLock, NULL);
  if (!pKeepCfg) {
    tsdbSetKeepCfg(pTsdb, &pVnode->config.tsdbCfg);
  } else {
    memcpy(&pTsdb->keepCfg, pKeepCfg, sizeof(STsdbKeepCfg));
  }

  // create dir
  if (pVnode->pTfs) {
    tfsMkdir(pVnode->pTfs, pTsdb->path);
  } else {
    taosMkDir(pTsdb->path);
  }

  // open tsdb
  if (tsdbOpenFS(pTsdb, &pTsdb->pFS, rollback) < 0) {
    goto _err;
  }

  if (tsdbOpenCache(pTsdb) < 0) {
    goto _err;
  }

  tsdbDebug("vgId:%d, tsdb is opened at %s, days:%d, keep:%d,%d,%d, keepTimeoffset:%d", TD_VID(pVnode), pTsdb->path,
            pTsdb->keepCfg.days, pTsdb->keepCfg.keep0, pTsdb->keepCfg.keep1, pTsdb->keepCfg.keep2,
            pTsdb->keepCfg.keepTimeOffset);

  *ppTsdb = pTsdb;
  return 0;

_err:
  taosMemoryFree(pTsdb);
  return -1;
}

int tsdbClose(STsdb **pTsdb) {
  if (*pTsdb) {
    STsdb *pdb = *pTsdb;
    tsdbDebug("vgId:%d, tsdb is close at %s, days:%d, keep:%d,%d,%d, keepTimeOffset:%d", TD_VID(pdb->pVnode), pdb->path,
              pdb->keepCfg.days, pdb->keepCfg.keep0, pdb->keepCfg.keep1, pdb->keepCfg.keep2,
              pdb->keepCfg.keepTimeOffset);
    taosThreadRwlockWrlock(&(*pTsdb)->rwLock);
    tsdbMemTableDestroy((*pTsdb)->mem, true);
    (*pTsdb)->mem = NULL;
    taosThreadRwlockUnlock(&(*pTsdb)->rwLock);

    taosThreadRwlockDestroy(&(*pTsdb)->rwLock);

    tsdbCloseFS(&(*pTsdb)->pFS);
    tsdbCloseCache(*pTsdb);
    taosMemoryFreeClear(*pTsdb);
  }
  return 0;
}