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

#ifndef _TD_CATALOG_INT_H_
#define _TD_CATALOG_INT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "catalog.h"
#include "tcommon.h"
#include "query.h"

#define CTG_DEFAULT_CACHE_CLUSTER_NUMBER 6
#define CTG_DEFAULT_CACHE_VGROUP_NUMBER 100
#define CTG_DEFAULT_CACHE_DB_NUMBER 20
#define CTG_DEFAULT_CACHE_TBLMETA_NUMBER 1000
#define CTG_DEFAULT_RENT_SECOND 10
#define CTG_DEFAULT_RENT_SLOT_SIZE 10
#define CTG_DEFAULT_MAX_RETRY_TIMES 3

#define CTG_RENT_SLOT_SECOND 1.5

#define CTG_DEFAULT_INVALID_VERSION (-1)

#define CTG_ERR_CODE_TABLE_NOT_EXIST TSDB_CODE_PAR_TABLE_NOT_EXIST

enum {
  CTG_READ = 1,
  CTG_WRITE,
};

enum {
  CTG_RENT_DB = 1,
  CTG_RENT_STABLE,
};

enum {
  CTG_OP_UPDATE_VGROUP = 0,
  CTG_OP_UPDATE_TB_META,
  CTG_OP_DROP_DB_CACHE,
  CTG_OP_DROP_DB_VGROUP,
  CTG_OP_DROP_STB_META,
  CTG_OP_DROP_TB_META,
  CTG_OP_UPDATE_USER,
  CTG_OP_UPDATE_VG_EPSET,
  CTG_OP_UPDATE_TB_INDEX,
  CTG_OP_DROP_TB_INDEX,
  CTG_OP_CLEAR_CACHE,
  CTG_OP_MAX
};

typedef enum {
  CTG_TASK_GET_QNODE = 0,
  CTG_TASK_GET_DB_VGROUP,
  CTG_TASK_GET_DB_CFG,
  CTG_TASK_GET_DB_INFO,
  CTG_TASK_GET_TB_META,
  CTG_TASK_GET_TB_HASH,
  CTG_TASK_GET_TB_INDEX,
  CTG_TASK_GET_INDEX,
  CTG_TASK_GET_UDF,
  CTG_TASK_GET_USER,
} CTG_TASK_TYPE;

typedef struct SCtgDebug {
  bool     lockEnable;
  bool     cacheEnable;
  bool     apiEnable;
  bool     metaEnable;
  uint32_t showCachePeriodSec;
} SCtgDebug;

typedef struct SCtgTbCacheInfo {
  bool     inCache;
  uint64_t dbId;
  uint64_t suid;
  int32_t  tbType;
} SCtgTbCacheInfo;

typedef struct SCtgTbMetaCtx {
  SCtgTbCacheInfo tbInfo;
  int32_t vgId;
  SName* pName;
  int32_t flag;
} SCtgTbMetaCtx;

typedef struct SCtgTbIndexCtx {
  SName* pName;
} SCtgTbIndexCtx;

typedef struct SCtgDbVgCtx {
  char dbFName[TSDB_DB_FNAME_LEN];
} SCtgDbVgCtx;

typedef struct SCtgDbCfgCtx {
  char dbFName[TSDB_DB_FNAME_LEN];
} SCtgDbCfgCtx;

typedef struct SCtgDbInfoCtx {
  char dbFName[TSDB_DB_FNAME_LEN];
} SCtgDbInfoCtx;

typedef struct SCtgTbHashCtx {
  char dbFName[TSDB_DB_FNAME_LEN];
  SName* pName;
} SCtgTbHashCtx;

typedef struct SCtgIndexCtx {
  char indexFName[TSDB_INDEX_FNAME_LEN];
} SCtgIndexCtx;

typedef struct SCtgUdfCtx {
  char udfName[TSDB_FUNC_NAME_LEN];
} SCtgUdfCtx;

typedef struct SCtgUserCtx {
  SUserAuthInfo user;
} SCtgUserCtx;

typedef STableIndexRsp STableIndex;

typedef struct SCtgTbCache {
  SRWLatch     metaLock;
  STableMeta  *pMeta;
  SRWLatch     indexLock;
  STableIndex *pIndex;
} SCtgTbCache;

typedef struct SCtgVgCache {
  SRWLatch         vgLock;
  SDBVgInfo       *vgInfo;  
} SCtgVgCache;

typedef struct SCtgDBCache {
  SRWLatch         dbLock;          // RC between destroy tbCache/stbCache and all reads
  uint64_t         dbId;
  int8_t           deleted;
  SCtgVgCache      vgCache;
  SHashObj        *tbCache;         // key:tbname, value:SCtgTbCache
  SHashObj        *stbCache;        // key:suid, value:STableMeta*
} SCtgDBCache;

typedef struct SCtgRentSlot {
  SRWLatch lock;
  bool     needSort;
  SArray  *meta;  // element is SDbVgVersion or SSTableVersion
} SCtgRentSlot;

typedef struct SCtgRentMgmt {
  int8_t         type;
  uint16_t       slotNum;
  uint16_t       slotRIdx;
  int64_t        lastReadMsec;
  SCtgRentSlot  *slots;
} SCtgRentMgmt;

typedef struct SCtgUserAuth {
  int32_t   version;
  SRWLatch  lock;
  bool      superUser;
  SHashObj *createdDbs;
  SHashObj *readDbs;
  SHashObj *writeDbs;
} SCtgUserAuth;

typedef struct SCatalog {
  uint64_t         clusterId;  
  SHashObj        *userCache;    //key:user, value:SCtgUserAuth
  SHashObj        *dbCache;      //key:dbname, value:SCtgDBCache
  SCtgRentMgmt     dbRent;
  SCtgRentMgmt     stbRent;
} SCatalog;

typedef struct SCtgJob {
  int64_t           refId;
  SArray*           pTasks;
  int32_t           taskDone;
  SMetaData         jobRes;

  uint64_t          queryId;
  SCatalog*         pCtg; 
  SRequestConnInfo  conn;
  void*             userParam;
  catalogCallback   userFp;
  int32_t           tbMetaNum;
  int32_t           tbHashNum;
  int32_t           dbVgNum;
  int32_t           udfNum;
  int32_t           qnodeNum;
  int32_t           dbCfgNum;
  int32_t           indexNum;
  int32_t           userNum;
  int32_t           dbInfoNum;
  int32_t           tbIndexNum;
} SCtgJob;

typedef struct SCtgMsgCtx {
  int32_t reqType;
  void* lastOut;
  void* out;
  char* target;
} SCtgMsgCtx;

typedef struct SCtgTask {
  CTG_TASK_TYPE type;
  int32_t       taskId;
  SCtgJob*      pJob;
  void*         taskCtx;
  SCtgMsgCtx    msgCtx;
  int32_t       code;
  void*         res;
} SCtgTask;

typedef int32_t (*ctgLanchTaskFp)(SCtgTask*);
typedef int32_t (*ctgHandleTaskMsgRspFp)(SCtgTask*, int32_t, const SDataBuf *, int32_t);
typedef int32_t (*ctgDumpTaskResFp)(SCtgTask*);

typedef struct SCtgAsyncFps {
  ctgLanchTaskFp launchFp;
  ctgHandleTaskMsgRspFp handleRspFp;
  ctgDumpTaskResFp dumpResFp;
} SCtgAsyncFps;

typedef struct SCtgApiStat {

#ifdef WINDOWS
  size_t avoidCompilationErrors;
#endif

} SCtgApiStat;

typedef struct SCtgRuntimeStat {
  uint64_t qNum;
  uint64_t qDoneNum;
} SCtgRuntimeStat;

typedef struct SCtgCacheStat {
  uint64_t clusterNum;
  uint64_t dbNum;
  uint64_t tblNum;
  uint64_t stblNum;
  uint64_t userNum;
  uint64_t vgHitNum;
  uint64_t vgMissNum;
  uint64_t tbMetaHitNum;
  uint64_t tbMetaMissNum;
  uint64_t tbIndexHitNum;
  uint64_t tbIndexMissNum;  
  uint64_t userHitNum;
  uint64_t userMissNum;
} SCtgCacheStat;

typedef struct SCatalogStat {
  SCtgApiStat      api;
  SCtgRuntimeStat  runtime;
  SCtgCacheStat    cache;
} SCatalogStat;

typedef struct SCtgUpdateMsgHeader {
  SCatalog* pCtg;
} SCtgUpdateMsgHeader;

typedef struct SCtgUpdateVgMsg {
  SCatalog* pCtg;
  char  dbFName[TSDB_DB_FNAME_LEN];
  uint64_t dbId;
  SDBVgInfo* dbInfo;
} SCtgUpdateVgMsg;

typedef struct SCtgUpdateTbMetaMsg {
  SCatalog*         pCtg;
  STableMetaOutput* pMeta;
} SCtgUpdateTbMetaMsg;

typedef struct SCtgDropDBMsg {
  SCatalog* pCtg;
  char  dbFName[TSDB_DB_FNAME_LEN];
  uint64_t dbId;
} SCtgDropDBMsg;

typedef struct SCtgDropDbVgroupMsg {
  SCatalog* pCtg;
  char  dbFName[TSDB_DB_FNAME_LEN];
} SCtgDropDbVgroupMsg;


typedef struct SCtgDropStbMetaMsg {
  SCatalog* pCtg;
  char  dbFName[TSDB_DB_FNAME_LEN];
  char  stbName[TSDB_TABLE_NAME_LEN];
  uint64_t dbId;
  uint64_t suid;
} SCtgDropStbMetaMsg;

typedef struct SCtgDropTblMetaMsg {
  SCatalog* pCtg;
  char  dbFName[TSDB_DB_FNAME_LEN];
  char  tbName[TSDB_TABLE_NAME_LEN];
  uint64_t dbId;
} SCtgDropTblMetaMsg;

typedef struct SCtgUpdateUserMsg {
  SCatalog* pCtg;
  SGetUserAuthRsp userAuth;
} SCtgUpdateUserMsg;

typedef struct SCtgUpdateTbIndexMsg {
  SCatalog*    pCtg;
  STableIndex* pIndex;
} SCtgUpdateTbIndexMsg;

typedef struct SCtgDropTbIndexMsg {
  SCatalog*    pCtg;
  char         dbFName[TSDB_DB_FNAME_LEN];
  char         tbName[TSDB_TABLE_NAME_LEN];
} SCtgDropTbIndexMsg;

typedef struct SCtgClearCacheMsg {
  SCatalog*    pCtg;
} SCtgClearCacheMsg;

typedef struct SCtgUpdateEpsetMsg {
  SCatalog* pCtg;
  char  dbFName[TSDB_DB_FNAME_LEN];
  int32_t vgId;
  SEpSet  epSet;
} SCtgUpdateEpsetMsg;

typedef struct SCtgCacheOperation {
  int32_t  opId;
  void    *data;
  bool     syncOp;
  tsem_t   rspSem;  
} SCtgCacheOperation;

typedef struct SCtgQNode {
  SCtgCacheOperation    *op;
  struct SCtgQNode      *next;
} SCtgQNode;

typedef struct SCtgQueue {
  SRWLatch              qlock;
  SCtgQNode            *head;
  SCtgQNode            *tail;
  tsem_t                reqSem;  
  uint64_t              qRemainNum;
} SCtgQueue;

typedef struct SCatalogMgmt {
  bool                  exit;
  int32_t               jobPool;
  SRWLatch              lock;
  SCtgQueue             queue;
  TdThread              updateThread;  
  SHashObj             *pCluster;     //key: clusterId, value: SCatalog*
  SCatalogStat          stat;
  SCatalogCfg           cfg;
} SCatalogMgmt;

typedef uint32_t (*tableNameHashFp)(const char *, uint32_t);
typedef int32_t (*ctgOpFunc)(SCtgCacheOperation *);

typedef struct SCtgOperation {
  int32_t    opId;
  char       name[32];
  ctgOpFunc func;
} SCtgOperation;

#define CTG_QUEUE_INC() atomic_add_fetch_64(&gCtgMgmt.queue.qRemainNum, 1)
#define CTG_QUEUE_DEC() atomic_sub_fetch_64(&gCtgMgmt.queue.qRemainNum, 1)

#define CTG_STAT_INC(_item, _n) atomic_add_fetch_64(&(_item), _n)
#define CTG_STAT_DEC(_item, _n) atomic_sub_fetch_64(&(_item), _n)
#define CTG_STAT_GET(_item) atomic_load_64(&(_item))

#define CTG_RT_STAT_INC(item, n) (CTG_STAT_INC(gCtgMgmt.stat.runtime.item, n))
#define CTG_CACHE_STAT_INC(item, n) (CTG_STAT_INC(gCtgMgmt.stat.cache.item, n))
#define CTG_CACHE_STAT_DEC(item, n) (CTG_STAT_DEC(gCtgMgmt.stat.cache.item, n))

#define CTG_IS_META_NULL(type) ((type) == META_TYPE_NULL_TABLE)
#define CTG_IS_META_CTABLE(type) ((type) == META_TYPE_CTABLE)
#define CTG_IS_META_TABLE(type) ((type) == META_TYPE_TABLE)
#define CTG_IS_META_BOTH(type) ((type) == META_TYPE_BOTH_TABLE)

#define CTG_FLAG_STB          0x1
#define CTG_FLAG_NOT_STB      0x2
#define CTG_FLAG_UNKNOWN_STB  0x4
#define CTG_FLAG_SYS_DB       0x8
#define CTG_FLAG_FORCE_UPDATE 0x10

#define CTG_FLAG_SET(_flag, _v) ((_flag) |= (_v))

#define CTG_FLAG_IS_STB(_flag) ((_flag) & CTG_FLAG_STB)
#define CTG_FLAG_IS_NOT_STB(_flag) ((_flag) & CTG_FLAG_NOT_STB)
#define CTG_FLAG_IS_UNKNOWN_STB(_flag) ((_flag) & CTG_FLAG_UNKNOWN_STB)
#define CTG_FLAG_IS_SYS_DB(_flag) ((_flag) & CTG_FLAG_SYS_DB)
#define CTG_FLAG_IS_FORCE_UPDATE(_flag) ((_flag) & CTG_FLAG_FORCE_UPDATE)
#define CTG_FLAG_SET_SYS_DB(_flag) ((_flag) |= CTG_FLAG_SYS_DB)
#define CTG_FLAG_SET_STB(_flag, tbType) do { (_flag) |= ((tbType) == TSDB_SUPER_TABLE) ? CTG_FLAG_STB : ((tbType) > TSDB_SUPER_TABLE ? CTG_FLAG_NOT_STB : CTG_FLAG_UNKNOWN_STB); } while (0)
#define CTG_FLAG_MAKE_STB(_isStb) (((_isStb) == 1) ? CTG_FLAG_STB : ((_isStb) == 0 ? CTG_FLAG_NOT_STB : CTG_FLAG_UNKNOWN_STB))
#define CTG_FLAG_MATCH_STB(_flag, tbType) (CTG_FLAG_IS_UNKNOWN_STB(_flag) || (CTG_FLAG_IS_STB(_flag) && (tbType) == TSDB_SUPER_TABLE) || (CTG_FLAG_IS_NOT_STB(_flag) && (tbType) != TSDB_SUPER_TABLE))

#define CTG_IS_SYS_DBNAME(_dbname) (((*(_dbname) == 'i') && (0 == strcmp(_dbname, TSDB_INFORMATION_SCHEMA_DB))) || ((*(_dbname) == 'p') && (0 == strcmp(_dbname, TSDB_PERFORMANCE_SCHEMA_DB))))

#define CTG_META_SIZE(pMeta) (sizeof(STableMeta) + ((pMeta)->tableInfo.numOfTags + (pMeta)->tableInfo.numOfColumns) * sizeof(SSchema))

#define CTG_TABLE_NOT_EXIST(code) (code == CTG_ERR_CODE_TABLE_NOT_EXIST) 
#define CTG_DB_NOT_EXIST(code) (code == TSDB_CODE_MND_DB_NOT_EXIST) 

#define ctgFatal(param, ...)  qFatal("CTG:%p " param, pCtg, __VA_ARGS__)
#define ctgError(param, ...)  qError("CTG:%p " param, pCtg, __VA_ARGS__)
#define ctgWarn(param, ...)   qWarn("CTG:%p " param, pCtg, __VA_ARGS__)
#define ctgInfo(param, ...)   qInfo("CTG:%p " param, pCtg, __VA_ARGS__)
#define ctgDebug(param, ...)  qDebug("CTG:%p " param, pCtg, __VA_ARGS__)
#define ctgTrace(param, ...)  qTrace("CTG:%p " param, pCtg, __VA_ARGS__)

#define CTG_LOCK_DEBUG(...) do { if (gCTGDebug.lockEnable) { qDebug(__VA_ARGS__); } } while (0)
#define CTG_CACHE_DEBUG(...) do { if (gCTGDebug.cacheEnable) { qDebug(__VA_ARGS__); } } while (0)
#define CTG_API_DEBUG(...) do { if (gCTGDebug.apiEnable) { qDebug(__VA_ARGS__); } } while (0)

#define TD_RWLATCH_WRITE_FLAG_COPY 0x40000000

#define CTG_IS_LOCKED(_lock) atomic_load_32((_lock))

#define CTG_LOCK(type, _lock) do {   \
  if (CTG_READ == (type)) {          \
    assert(atomic_load_32((_lock)) >= 0);  \
    CTG_LOCK_DEBUG("CTG RLOCK%p:%d, %s:%d B", (_lock), atomic_load_32(_lock), __FILE__, __LINE__); \
    taosRLockLatch(_lock);           \
    CTG_LOCK_DEBUG("CTG RLOCK%p:%d, %s:%d E", (_lock), atomic_load_32(_lock), __FILE__, __LINE__); \
    assert(atomic_load_32((_lock)) > 0);  \
  } else {                                                \
    assert(atomic_load_32((_lock)) >= 0);  \
    CTG_LOCK_DEBUG("CTG WLOCK%p:%d, %s:%d B", (_lock), atomic_load_32(_lock), __FILE__, __LINE__);  \
    taosWLockLatch(_lock);                                \
    CTG_LOCK_DEBUG("CTG WLOCK%p:%d, %s:%d E", (_lock), atomic_load_32(_lock), __FILE__, __LINE__);  \
    assert(atomic_load_32((_lock)) == TD_RWLATCH_WRITE_FLAG_COPY);  \
  }                                                       \
} while (0)

#define CTG_UNLOCK(type, _lock) do {                       \
  if (CTG_READ == (type)) {                                \
    assert(atomic_load_32((_lock)) > 0);  \
    CTG_LOCK_DEBUG("CTG RULOCK%p:%d, %s:%d B", (_lock), atomic_load_32(_lock), __FILE__, __LINE__); \
    taosRUnLockLatch(_lock);                              \
    CTG_LOCK_DEBUG("CTG RULOCK%p:%d, %s:%d E", (_lock), atomic_load_32(_lock), __FILE__, __LINE__); \
    assert(atomic_load_32((_lock)) >= 0);  \
  } else {                                                \
    assert(atomic_load_32((_lock)) == TD_RWLATCH_WRITE_FLAG_COPY);  \
    CTG_LOCK_DEBUG("CTG WULOCK%p:%d, %s:%d B", (_lock), atomic_load_32(_lock), __FILE__, __LINE__); \
    taosWUnLockLatch(_lock);                              \
    CTG_LOCK_DEBUG("CTG WULOCK%p:%d, %s:%d E", (_lock), atomic_load_32(_lock), __FILE__, __LINE__); \
    assert(atomic_load_32((_lock)) >= 0);  \
  }                                                       \
} while (0)

  
#define CTG_ERR_RET(c) do { int32_t _code = c; if (_code != TSDB_CODE_SUCCESS) { terrno = _code; return _code; } } while (0)
#define CTG_RET(c) do { int32_t _code = c; if (_code != TSDB_CODE_SUCCESS) { terrno = _code; } return _code; } while (0)
#define CTG_ERR_JRET(c) do { code = c; if (code != TSDB_CODE_SUCCESS) { terrno = code; goto _return; } } while (0)

#define CTG_API_LEAVE(c) do { int32_t __code = c; CTG_UNLOCK(CTG_READ, &gCtgMgmt.lock); CTG_API_DEBUG("CTG API leave %s", __FUNCTION__); CTG_RET(__code); } while (0)
#define CTG_API_ENTER() do { CTG_API_DEBUG("CTG API enter %s", __FUNCTION__); CTG_LOCK(CTG_READ, &gCtgMgmt.lock); if (atomic_load_8((int8_t*)&gCtgMgmt.exit)) { CTG_API_LEAVE(TSDB_CODE_CTG_OUT_OF_SERVICE); }  } while (0)

void    ctgdShowTableMeta(SCatalog* pCtg, const char *tbName, STableMeta* p);
void    ctgdShowClusterCache(SCatalog* pCtg);
int32_t ctgdShowCacheInfo(void);

int32_t ctgRemoveTbMetaFromCache(SCatalog* pCtg, SName* pTableName, bool syncReq);
int32_t ctgGetTbMetaFromCache(SCatalog* pCtg, SRequestConnInfo *pConn, SCtgTbMetaCtx* ctx, STableMeta** pTableMeta);

int32_t ctgOpUpdateVgroup(SCtgCacheOperation *action);
int32_t ctgOpUpdateTbMeta(SCtgCacheOperation *action);
int32_t ctgOpDropDbCache(SCtgCacheOperation *action);
int32_t ctgOpDropDbVgroup(SCtgCacheOperation *action);
int32_t ctgOpDropStbMeta(SCtgCacheOperation *action);
int32_t ctgOpDropTbMeta(SCtgCacheOperation *action);
int32_t ctgOpUpdateUser(SCtgCacheOperation *action);
int32_t ctgOpUpdateEpset(SCtgCacheOperation *operation);
int32_t ctgAcquireVgInfoFromCache(SCatalog* pCtg, const char *dbFName, SCtgDBCache **pCache);
void    ctgReleaseDBCache(SCatalog *pCtg, SCtgDBCache *dbCache);
void    ctgRUnlockVgInfo(SCtgDBCache *dbCache);
int32_t ctgTbMetaExistInCache(SCatalog* pCtg, char *dbFName, char* tbName, int32_t *exist);
int32_t ctgReadTbMetaFromCache(SCatalog* pCtg, SCtgTbMetaCtx* ctx, STableMeta** pTableMeta);
int32_t ctgReadTbVerFromCache(SCatalog *pCtg, SName *pTableName, int32_t *sver, int32_t *tver, int32_t *tbType, uint64_t *suid, char *stbName);
int32_t ctgChkAuthFromCache(SCatalog* pCtg, char* user, char* dbFName, AUTH_TYPE type, bool *inCache, bool *pass);
int32_t ctgDropDbCacheEnqueue(SCatalog* pCtg, const char *dbFName, int64_t dbId);
int32_t ctgDropDbVgroupEnqueue(SCatalog* pCtg, const char *dbFName, bool syncReq);
int32_t ctgDropStbMetaEnqueue(SCatalog* pCtg, const char *dbFName, int64_t dbId, const char *stbName, uint64_t suid, bool syncReq);
int32_t ctgDropTbMetaEnqueue(SCatalog* pCtg, const char *dbFName, int64_t dbId, const char *tbName, bool syncReq);
int32_t ctgUpdateVgroupEnqueue(SCatalog* pCtg, const char *dbFName, int64_t dbId, SDBVgInfo* dbInfo, bool syncReq);
int32_t ctgUpdateTbMetaEnqueue(SCatalog* pCtg, STableMetaOutput *output, bool syncReq);
int32_t ctgUpdateUserEnqueue(SCatalog* pCtg, SGetUserAuthRsp *pAuth, bool syncReq);
int32_t ctgUpdateVgEpsetEnqueue(SCatalog* pCtg, char *dbFName, int32_t vgId, SEpSet* pEpSet);
int32_t ctgUpdateTbIndexEnqueue(SCatalog* pCtg, STableIndex **pIndex, bool syncOp);
int32_t ctgClearCacheEnqueue(SCatalog* pCtg, bool syncOp);
int32_t ctgMetaRentInit(SCtgRentMgmt *mgmt, uint32_t rentSec, int8_t type);
int32_t ctgMetaRentAdd(SCtgRentMgmt *mgmt, void *meta, int64_t id, int32_t size);
int32_t ctgMetaRentGet(SCtgRentMgmt *mgmt, void **res, uint32_t *num, int32_t size);
int32_t ctgUpdateTbMetaToCache(SCatalog* pCtg, STableMetaOutput* pOut, bool syncReq);
int32_t ctgStartUpdateThread();
int32_t ctgRelaunchGetTbMetaTask(SCtgTask *pTask);
void    ctgReleaseVgInfoToCache(SCatalog* pCtg, SCtgDBCache *dbCache);
int32_t ctgReadTbIndexFromCache(SCatalog* pCtg, SName* pTableName, SArray** pRes);
int32_t ctgDropTbIndexEnqueue(SCatalog* pCtg, SName* pName, bool syncOp);
int32_t ctgOpDropTbIndex(SCtgCacheOperation *operation);
int32_t ctgOpUpdateTbIndex(SCtgCacheOperation *operation);
int32_t ctgOpClearCache(SCtgCacheOperation *operation);
int32_t ctgReadTbTypeFromCache(SCatalog* pCtg, char* dbFName, char *tableName, int32_t *tbType);




int32_t ctgProcessRspMsg(void* out, int32_t reqType, char* msg, int32_t msgSize, int32_t rspCode, char* target);
int32_t ctgGetDBVgInfoFromMnode(SCatalog* pCtg, SRequestConnInfo *pConn, SBuildUseDBInput *input, SUseDbOutput *out, SCtgTask* pTask);
int32_t ctgGetQnodeListFromMnode(SCatalog* pCtg, SRequestConnInfo *pConn, SArray *out, SCtgTask* pTask);
int32_t ctgGetDBCfgFromMnode(SCatalog* pCtg, SRequestConnInfo *pConn, const char *dbFName, SDbCfgInfo *out, SCtgTask* pTask);
int32_t ctgGetIndexInfoFromMnode(SCatalog* pCtg, SRequestConnInfo *pConn, const char *indexName, SIndexInfo *out, SCtgTask* pTask);
int32_t ctgGetTbIndexFromMnode(SCatalog* pCtg, SRequestConnInfo *pConn, SName *name, STableIndex* out, SCtgTask* pTask);
int32_t ctgGetUdfInfoFromMnode(SCatalog* pCtg, SRequestConnInfo *pConn, const char *funcName, SFuncInfo *out, SCtgTask* pTask);
int32_t ctgGetUserDbAuthFromMnode(SCatalog* pCtg, SRequestConnInfo *pConn, const char *user, SGetUserAuthRsp *out, SCtgTask* pTask);
int32_t ctgGetTbMetaFromMnodeImpl(SCatalog* pCtg, SRequestConnInfo *pConn, char *dbFName, char* tbName, STableMetaOutput* out, SCtgTask* pTask);
int32_t ctgGetTbMetaFromMnode(SCatalog* pCtg, SRequestConnInfo *pConn, const SName* pTableName, STableMetaOutput* out, SCtgTask* pTask);
int32_t ctgGetTbMetaFromVnode(SCatalog* pCtg, SRequestConnInfo *pConn, const SName* pTableName, SVgroupInfo *vgroupInfo, STableMetaOutput* out, SCtgTask* pTask);

int32_t ctgInitJob(SCatalog* pCtg, SRequestConnInfo *pConn, SCtgJob** job, uint64_t reqId, const SCatalogReq* pReq, catalogCallback fp, void* param, int32_t* taskNum);
int32_t ctgLaunchJob(SCtgJob *pJob);
int32_t ctgMakeAsyncRes(SCtgJob *pJob);

int32_t ctgCloneVgInfo(SDBVgInfo *src, SDBVgInfo **dst);
int32_t ctgCloneMetaOutput(STableMetaOutput *output, STableMetaOutput **pOutput);
int32_t ctgGenerateVgList(SCatalog *pCtg, SHashObj *vgHash, SArray** pList);
void    ctgFreeJob(void* job);
void    ctgFreeHandle(SCatalog* pCtg);
void    ctgFreeVgInfo(SDBVgInfo *vgInfo);
int32_t ctgGetVgInfoFromHashValue(SCatalog *pCtg, SDBVgInfo *dbInfo, const SName *pTableName, SVgroupInfo *pVgroup);
void    ctgResetTbMetaTask(SCtgTask* pTask);
void    ctgFreeDbCache(SCtgDBCache *dbCache);
int32_t ctgStbVersionSortCompare(const void* key1, const void* key2);
int32_t ctgDbVgVersionSortCompare(const void* key1, const void* key2);
int32_t ctgStbVersionSearchCompare(const void* key1, const void* key2);
int32_t ctgDbVgVersionSearchCompare(const void* key1, const void* key2);
void    ctgFreeSTableMetaOutput(STableMetaOutput* pOutput);
int32_t ctgUpdateMsgCtx(SCtgMsgCtx* pCtx, int32_t reqType, void* out, char* target);
char *  ctgTaskTypeStr(CTG_TASK_TYPE type);
int32_t ctgUpdateSendTargetInfo(SMsgSendInfo *pMsgSendInfo, int32_t msgType, SCtgTask* pTask);
int32_t ctgCloneTableIndex(SArray* pIndex, SArray** pRes);
void    ctgFreeSTableIndex(void *info);


extern SCatalogMgmt gCtgMgmt;
extern SCtgDebug gCTGDebug;
extern SCtgAsyncFps gCtgAsyncFps[];

#ifdef __cplusplus
}
#endif

#endif /*_TD_CATALOG_INT_H_*/
