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

#define _DEFAULT_SOURCE
#include "os.h"
#include "tcache.h"
#include "tconfig.h"
#include "tfs.h"
#include "tnote.h"
#include "tscompression.h"
#include "ttimer.h"
#include "dnodeCfg.h"
#include "dnodeMain.h"
#include "mnode.h"

static int32_t dnodeCreateDir(const char *dir) {
  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    return -1;
  }

  return 0;
}

static void dnodeCheckDataDirOpenned(char *dir) {
  char filepath[256] = {0};
  snprintf(filepath, sizeof(filepath), "%s/.running", dir);

  int32_t fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
  if (fd < 0) {
    dError("failed to open lock file:%s, reason: %s, quit", filepath, strerror(errno));
    exit(0);
  }

  int32_t ret = flock(fd, LOCK_EX | LOCK_NB);
  if (ret != 0) {
    dError("failed to lock file:%s ret:%d since %s, database may be running, quit", filepath, ret, strerror(errno));
    close(fd);
    exit(0);
  }
}

int32_t dnodeInitMain(Dnode *dnode, DnMain **out) {
  DnMain* main = calloc(1, sizeof(DnMain));
  if (main == NULL) return -1;

  main->dnode = dnode;
  main->runStatus = TD_RUN_STAT_STOPPED;
  main->dnodeTimer = taosTmrInit(100, 200, 60000, "DND-TMR");
  if (main->dnodeTimer == NULL) {
    dError("failed to init dnode timer");
    return -1;
  }

  *out = main;

  tscEmbedded = 1;
  taosIgnSIGPIPE();
  taosBlockSIGPIPE();
  taosResolveCRC();
  taosInitGlobalCfg();
  taosReadGlobalLogCfg();
  taosSetCoreDump();

  if (dnodeCreateDir(tsLogDir) < 0) {
   printf("failed to create dir: %s, reason: %s\n", tsLogDir, strerror(errno));
   return -1;
  }

  char temp[TSDB_FILENAME_LEN];
  sprintf(temp, "%s/taosdlog", tsLogDir);
  if (taosInitLog(temp, tsNumOfLogLines, 1) < 0) {
    printf("failed to init log file\n");
  }

  if (!taosReadGlobalCfg()) {
    taosPrintGlobalCfg();
    dError("TDengine read global config failed");
    return -1;
  }

  dInfo("start to initialize TDengine");

  taosInitNotes();

  return taosCheckGlobalCfg();
}

void dnodeCleanupMain(DnMain **out) {
  DnMain *main = *out;
  *out = NULL;

  if (main->dnodeTimer != NULL) {
    taosTmrCleanUp(main->dnodeTimer);
    main->dnodeTimer = NULL;
  }

  taos_cleanup();
  taosCloseLog();
  taosStopCacheRefreshWorker();

  free(main);
}

int32_t dnodeInitStorage(Dnode *dnode, void **m) {
#ifdef TD_TSZ
  // compress module init
  tsCompressInit();
#endif

  // storage module init
  if (tsDiskCfgNum == 1 && dnodeCreateDir(tsDataDir) < 0) {
    dError("failed to create dir:%s since %s", tsDataDir, strerror(errno));
    return -1;
  }

  if (tfsInit(tsDiskCfg, tsDiskCfgNum) < 0) {
    dError("failed to init TFS since %s", tstrerror(terrno));
    return -1;
  }
  strncpy(tsDataDir, TFS_PRIMARY_PATH(), TSDB_FILENAME_LEN);
  sprintf(tsMnodeDir, "%s/mnode", tsDataDir);
  sprintf(tsVnodeDir, "%s/vnode", tsDataDir);
  sprintf(tsDnodeDir, "%s/dnode", tsDataDir);

  if (dnodeCreateDir(tsMnodeDir) < 0) {
    dError("failed to create dir:%s since %s", tsMnodeDir, strerror(errno));
    return -1;
  }

  if (dnodeCreateDir(tsDnodeDir) < 0) {
    dError("failed to create dir:%s since %s", tsDnodeDir, strerror(errno));
    return -1;
  }

  if (tfsMkdir("vnode") < 0) {
    dError("failed to create vnode dir since %s", tstrerror(terrno));
    return -1;
  }

  if (tfsMkdir("vnode_bak") < 0) {
    dError("failed to create vnode_bak dir since %s", tstrerror(terrno));
    return -1;
  }

  TDIR *tdir = tfsOpendir("vnode_bak/.staging");
  bool  stagingNotEmpty = tfsReaddir(tdir) != NULL;
  tfsClosedir(tdir);

  if (stagingNotEmpty) {
    dError("vnode_bak/.staging dir not empty, fix it first.");
    return -1;
  }

  if (tfsMkdir("vnode_bak/.staging") < 0) {
    dError("failed to create vnode_bak/.staging dir since %s", tstrerror(terrno));
    return -1;
  }

  dnodeCheckDataDirOpenned(tsDnodeDir);

  taosGetDisk();
  taosPrintDiskInfo();

  dInfo("dnode storage is initialized at %s", tsDnodeDir);
  return 0;
}

void dnodeCleanupStorage(void **m) {
  // storage destroy
  tfsDestroy();

 #ifdef TD_TSZ
  // compress destroy
  tsCompressExit();
 #endif
}

void dnodeReportStartup(Dnode *dnode, char *name, char *desc) {
  SStartupStep *startup = &dnode->main->startup;
  tstrncpy(startup->name, name, strlen(startup->name));
  tstrncpy(startup->desc, desc, strlen(startup->desc));
  startup->finished = 0;
}

void dnodeReportStartupFinished(Dnode *dnode, char *name, char *desc) {
  SStartupStep *startup = &dnode->main->startup;
  tstrncpy(startup->name, name, strlen(startup->name));
  tstrncpy(startup->desc, desc, strlen(startup->desc));
  startup->finished = 1;
}

void dnodeProcessStartupReq(Dnode *dnode, SRpcMsg *pMsg) {
  dInfo("startup msg is received, cont:%s", (char *)pMsg->pCont);

  SStartupStep *pStep = rpcMallocCont(sizeof(SStartupStep));
  memcpy(pStep, &dnode->main->startup, sizeof(SStartupStep));

  dDebug("startup msg is sent, step:%s desc:%s finished:%d", pStep->name, pStep->desc, pStep->finished);

  SRpcMsg rpcRsp = {.handle = pMsg->handle, .pCont = pStep, .contLen = sizeof(SStartupStep)};
  rpcSendResponse(&rpcRsp);
  rpcFreeCont(pMsg->pCont);
}

static int32_t dnodeStartMnode(Dnode *dnode, SRpcMsg *pMsg) {
  SCreateMnodeMsg *pCfg = pMsg->pCont;
  pCfg->dnodeId = htonl(pCfg->dnodeId);
  if (pCfg->dnodeId != dnodeGetDnodeId(dnode->cfg)) {
    dDebug("dnode:%d, in create meps msg is not equal with saved dnodeId:%d", pCfg->dnodeId,
           dnodeGetDnodeId(dnode->cfg));
    return TSDB_CODE_MND_DNODE_ID_NOT_CONFIGURED;
  }

  if (strcmp(pCfg->dnodeEp, tsLocalEp) != 0) {
    dDebug("dnodeEp:%s, in create meps msg is not equal with saved dnodeEp:%s", pCfg->dnodeEp, tsLocalEp);
    return TSDB_CODE_MND_DNODE_EP_NOT_CONFIGURED;
  }

  dDebug("dnode:%d, create meps msg is received from mnodes, numOfMnodes:%d", pCfg->dnodeId, pCfg->mnodes.mnodeNum);
  for (int32_t i = 0; i < pCfg->mnodes.mnodeNum; ++i) {
    pCfg->mnodes.mnodeInfos[i].mnodeId = htonl(pCfg->mnodes.mnodeInfos[i].mnodeId);
    dDebug("meps index:%d, meps:%d:%s", i, pCfg->mnodes.mnodeInfos[i].mnodeId, pCfg->mnodes.mnodeInfos[i].mnodeEp);
  }

  if (mnodeIsServing(dnode->mnode)) return 0;

  return mnodeDeploy(dnode->mnode, &pCfg->mnodes);
}

void dnodeProcessCreateMnodeReq(Dnode *dnode, SRpcMsg *pMsg) {
  int32_t code = dnodeStartMnode(dnode, pMsg);

  SRpcMsg rspMsg = {.handle = pMsg->handle, .pCont = NULL, .contLen = 0, .code = code};

  rpcSendResponse(&rspMsg);
  rpcFreeCont(pMsg->pCont);
}

void dnodeProcessConfigDnodeReq(Dnode *dnode, SRpcMsg *pMsg) {
  SCfgDnodeMsg *pCfg = pMsg->pCont;

  int32_t code = taosCfgDynamicOptions(pCfg->config);

  SRpcMsg rspMsg = {.handle = pMsg->handle, .pCont = NULL, .contLen = 0, .code = code};

  rpcSendResponse(&rspMsg);
  rpcFreeCont(pMsg->pCont);
}