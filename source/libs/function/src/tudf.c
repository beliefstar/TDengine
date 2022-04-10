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
#include "uv.h"
#include "os.h"
#include "tlog.h"
#include "tudf.h"
#include "tudfInt.h"
#include "tarray.h"

//TODO: when startup, set thread poll size. add it to cfg 
//TODO: udfd restart when exist or aborts
//TODO: network error processing.
//TODO: add unit test
//TODO: test libuv queue
typedef void *QUEUE[2];

/* Private macros. */
#define QUEUE_NEXT(q)       (*(QUEUE **) &((*(q))[0]))
#define QUEUE_PREV(q)       (*(QUEUE **) &((*(q))[1]))
#define QUEUE_PREV_NEXT(q)  (QUEUE_NEXT(QUEUE_PREV(q)))
#define QUEUE_NEXT_PREV(q)  (QUEUE_PREV(QUEUE_NEXT(q)))

/* Public macros. */
#define QUEUE_DATA(ptr, type, field)                                          \
  ((type *) ((char *) (ptr) - offsetof(type, field)))

/* Important note: mutating the list while QUEUE_FOREACH is
 * iterating over its elements results in undefined behavior.
 */
#define QUEUE_FOREACH(q, h)                                                   \
  for ((q) = QUEUE_NEXT(h); (q) != (h); (q) = QUEUE_NEXT(q))

#define QUEUE_EMPTY(q)                                                        \
  ((const QUEUE *) (q) == (const QUEUE *) QUEUE_NEXT(q))

#define QUEUE_HEAD(q)                                                         \
  (QUEUE_NEXT(q))

#define QUEUE_INIT(q)                                                         \
  do {                                                                        \
    QUEUE_NEXT(q) = (q);                                                      \
    QUEUE_PREV(q) = (q);                                                      \
  }                                                                           \
  while (0)

#define QUEUE_ADD(h, n)                                                       \
  do {                                                                        \
    QUEUE_PREV_NEXT(h) = QUEUE_NEXT(n);                                       \
    QUEUE_NEXT_PREV(n) = QUEUE_PREV(h);                                       \
    QUEUE_PREV(h) = QUEUE_PREV(n);                                            \
    QUEUE_PREV_NEXT(h) = (h);                                                 \
  }                                                                           \
  while (0)

#define QUEUE_SPLIT(h, q, n)                                                  \
  do {                                                                        \
    QUEUE_PREV(n) = QUEUE_PREV(h);                                            \
    QUEUE_PREV_NEXT(n) = (n);                                                 \
    QUEUE_NEXT(n) = (q);                                                      \
    QUEUE_PREV(h) = QUEUE_PREV(q);                                            \
    QUEUE_PREV_NEXT(h) = (h);                                                 \
    QUEUE_PREV(q) = (n);                                                      \
  }                                                                           \
  while (0)

#define QUEUE_MOVE(h, n)                                                      \
  do {                                                                        \
    if (QUEUE_EMPTY(h))                                                       \
      QUEUE_INIT(n);                                                          \
    else {                                                                    \
      QUEUE* q = QUEUE_HEAD(h);                                               \
      QUEUE_SPLIT(h, q, n);                                                   \
    }                                                                         \
  }                                                                           \
  while (0)

#define QUEUE_INSERT_HEAD(h, q)                                               \
  do {                                                                        \
    QUEUE_NEXT(q) = QUEUE_NEXT(h);                                            \
    QUEUE_PREV(q) = (h);                                                      \
    QUEUE_NEXT_PREV(q) = (q);                                                 \
    QUEUE_NEXT(h) = (q);                                                      \
  }                                                                           \
  while (0)

#define QUEUE_INSERT_TAIL(h, q)                                               \
  do {                                                                        \
    QUEUE_NEXT(q) = (h);                                                      \
    QUEUE_PREV(q) = QUEUE_PREV(h);                                            \
    QUEUE_PREV_NEXT(q) = (q);                                                 \
    QUEUE_PREV(h) = (q);                                                      \
  }                                                                           \
  while (0)

#define QUEUE_REMOVE(q)                                                       \
  do {                                                                        \
    QUEUE_PREV_NEXT(q) = QUEUE_NEXT(q);                                       \
    QUEUE_NEXT_PREV(q) = QUEUE_PREV(q);                                       \
  }                                                                           \
  while (0)


enum {
  UV_TASK_CONNECT = 0,
  UV_TASK_REQ_RSP = 1,
  UV_TASK_DISCONNECT = 2
};

typedef struct SUdfUvSession {
  int64_t severHandle;
  uv_pipe_t *udfSvcPipe;
} SUdfUvSession;

typedef struct SClientUvTaskNode {
  int8_t type;
  int errCode;

  uv_pipe_t *pipe;

  int64_t seqNum;
  uv_buf_t reqBuf;

  uv_sem_t taskSem;
  uv_buf_t rspBuf;

  QUEUE recvTaskQueue;
  QUEUE procTaskQueue;
  QUEUE connTaskQueue;
} SClientUvTaskNode;

typedef struct SClientUdfTask {
  int8_t type;

  SUdfUvSession *session;

  int32_t errCode;

  union {
    struct {
      SUdfSetupRequest req;
      SUdfSetupResponse rsp;
    } _setup;
    struct {
      SUdfCallRequest req;
      SUdfCallResponse rsp;
    } _call;
    struct {
      SUdfTeardownRequest req;
      SUdfTeardownResponse rsp;
    } _teardown;
  };


} SClientUdfTask;

typedef struct SClientConnBuf {
  char *buf;
  int32_t len;
  int32_t cap;
  int32_t total;
} SClientConnBuf;

typedef struct SClientUvConn {
  uv_pipe_t *pipe;
  QUEUE taskQueue;
  SClientConnBuf readBuf;
} SClientUvConn;

uv_process_t gUdfdProcess;

uv_barrier_t gUdfInitBarrier;

uv_loop_t gUdfdLoop;
uv_thread_t gUdfLoopThread;
uv_async_t gUdfLoopTaskAync;

uv_async_t gUdfLoopStopAsync;

uv_mutex_t gUdfTaskQueueMutex;
int64_t gUdfTaskSeqNum = 0;

enum {
  UDFC_STATE_INITAL = 0, // initial state
  UDFC_STATE_STARTNG, // starting after startUdfService
  UDFC_STATE_READY, // started and begin to receive quests
  UDFC_STATE_RESTARTING, // udfd abnormal exit. cleaning up and restart.
  UDFC_STATE_STOPPING, // stopping after stopUdfService
  UDFC_STATUS_FINAL, // stopped
};
int8_t gUdfcState = UDFC_STATE_INITAL;

//double circular linked list

QUEUE gUdfTaskQueue = {0};

//TODO: deal with uv task that has been started and then udfd core dumped
QUEUE gUvProcTaskQueue = {0};

int32_t encodeRequest(char **pBuf, int32_t *pBufLen, SUdfRequest *request) {
  debugPrint("%s", "encoding request");

  int len = sizeof(SUdfRequest) - sizeof(void *);
  switch (request->type) {
    case UDF_TASK_SETUP: {
      SUdfSetupRequest *setup = (SUdfSetupRequest *) (request->subReq);
      len += sizeof(SUdfSetupRequest) - 1 * sizeof(char *) + setup->pathSize;
      break;
    }
    case UDF_TASK_CALL: {
      SUdfCallRequest *call = (SUdfCallRequest *) (request->subReq);
      len += sizeof(SUdfCallRequest) - 2 * sizeof(char *) + call->inputBytes + call->stateBytes;
      break;
    }
    case UDF_TASK_TEARDOWN: {
      SUdfTeardownRequest *teardown = (SUdfTeardownRequest *) (request->subReq);
      len += sizeof(SUdfTeardownRequest);
      break;
    }
    default:
      break;
  }

  char *bufBegin = taosMemoryMalloc(len);
  char *buf = bufBegin;

  //skip msgLen first
  buf += sizeof(int32_t);

  *(int64_t *) buf = request->seqNum;
  buf += sizeof(int64_t);
  *(int8_t *) buf = request->type;
  buf += sizeof(int8_t);

  switch (request->type) {
    case UDF_TASK_SETUP: {
      SUdfSetupRequest *setup = (SUdfSetupRequest *) (request->subReq);
      memcpy(buf, setup->udfName, 16);
      buf += 16;
      *(int8_t *) buf = setup->scriptType;
      buf += sizeof(int8_t);
      *(int8_t *) buf = setup->udfType;
      buf += sizeof(int8_t);
      *(int16_t *) buf = setup->pathSize;
      buf += sizeof(int16_t);
      memcpy(buf, setup->path, setup->pathSize);
      buf += setup->pathSize;
      break;
    }

    case UDF_TASK_CALL: {
      SUdfCallRequest *call = (SUdfCallRequest *) (request->subReq);
      *(int64_t *) buf = call->udfHandle;
      buf += sizeof(int64_t);
      *(int8_t *) buf = call->step;
      buf += sizeof(int8_t);
      *(int32_t *) buf = call->inputBytes;
      buf += sizeof(int32_t);
      memcpy(buf, call->input, call->inputBytes);
      buf += call->inputBytes;
      *(int32_t *) buf = call->stateBytes;
      buf += sizeof(int32_t);
      memcpy(buf, call->state, call->stateBytes);
      buf += call->stateBytes;
      break;
    }

    case UDF_TASK_TEARDOWN: {
      SUdfTeardownRequest *teardown = (SUdfTeardownRequest *) (request->subReq);
      *(int64_t *) buf = teardown->udfHandle;
      buf += sizeof(int64_t);
      break;
    }
    default:
      break;
  }

  request->msgLen = buf - bufBegin;
  *(int32_t *) bufBegin = request->msgLen;
  *pBuf = bufBegin;
  *pBufLen = request->msgLen;
  debugPrint("\tLen: estimate: %d, actual:%d", len, *pBufLen);
  return 0;
}

int32_t decodeRequest(char *bufMsg, int32_t bufLen, SUdfRequest **pRequest) {
  debugPrint("%s", "decoding request");
  if (*(int32_t *) bufMsg != bufLen) {
    debugPrint("%s", "decoding request error");
    return -1;
  }
  char *buf = bufMsg;
  SUdfRequest *request = taosMemoryMalloc(sizeof(SUdfRequest));
  request->subReq = NULL;
  request->msgLen = *(int32_t *) (buf);
  buf += sizeof(int32_t);
  request->seqNum = *(int64_t *) (buf);
  buf += sizeof(int64_t);
  request->type = *(int8_t *) (buf);
  buf += sizeof(int8_t);

  switch (request->type) {
    case UDF_TASK_SETUP: {
      SUdfSetupRequest *setup = taosMemoryMalloc(sizeof(SUdfSetupRequest));

      memcpy(setup->udfName, buf, 16);
      buf += 16;
      setup->scriptType = *(int8_t *) buf;
      buf += sizeof(int8_t);
      setup->udfType = *(int8_t *) buf;
      buf += sizeof(int8_t);
      setup->pathSize = *(int16_t *) buf;
      buf += sizeof(int16_t);
      setup->path = buf;
      buf += setup->pathSize;

      request->subReq = setup;
      break;
    }
    case UDF_TASK_CALL: {
      SUdfCallRequest *call = taosMemoryMalloc(sizeof(SUdfCallRequest));

      call->udfHandle = *(int64_t *) buf;
      buf += sizeof(int64_t);
      call->step = *(int8_t *) buf;
      buf += sizeof(int8_t);
      call->inputBytes = *(int32_t *) buf;
      buf += sizeof(int32_t);
      call->input = buf;
      buf += call->inputBytes;
      call->stateBytes = *(int32_t *) buf;
      buf += sizeof(int32_t);
      call->state = buf;
      buf += call->stateBytes;

      request->subReq = call;
      break;
    }

    case UDF_TASK_TEARDOWN: {
      SUdfTeardownRequest *teardown = taosMemoryMalloc(sizeof(SUdfTeardownRequest));

      teardown->udfHandle = *(int64_t *) buf;
      buf += sizeof(int64_t);

      request->subReq = teardown;
    }

  }
  if (buf - bufMsg != bufLen) {
    debugPrint("%s", "decode request error");
    taosMemoryFree(request->subReq);
    taosMemoryFree(request);
    return -1;
  }
  *pRequest = request;
  return 0;
}

int32_t encodeResponse(char **pBuf, int32_t *pBufLen, SUdfResponse *response) {
  debugPrint("%s", "encoding response");

  int32_t len = sizeof(SUdfResponse) - sizeof(void *);

  switch (response->type) {
    case UDF_TASK_SETUP: {
      len += sizeof(SUdfSetupResponse);
      break;
    }
    case UDF_TASK_CALL: {
      SUdfCallResponse *callResp = (SUdfCallResponse *) (response->subRsp);
      len += sizeof(SUdfCallResponse) - 2 * sizeof(char *) +
             callResp->outputBytes + callResp->newStateBytes;
      break;
    }
    case UDF_TASK_TEARDOWN: {
      len += sizeof(SUdfTeardownResponse);
      break;
    }
  }

  char *bufBegin = taosMemoryMalloc(len);
  char *buf = bufBegin;

  //skip msgLen
  buf += sizeof(int32_t);

  *(int64_t *) buf = response->seqNum;
  buf += sizeof(int64_t);
  *(int8_t *) buf = response->type;
  buf += sizeof(int8_t);
  *(int32_t *) buf = response->code;
  buf += sizeof(int32_t);


  switch (response->type) {
    case UDF_TASK_SETUP: {
      SUdfSetupResponse *setupResp = (SUdfSetupResponse *) (response->subRsp);
      *(int64_t *) buf = setupResp->udfHandle;
      buf += sizeof(int64_t);
      break;
    }
    case UDF_TASK_CALL: {
      SUdfCallResponse *callResp = (SUdfCallResponse *) (response->subRsp);
      *(int32_t *) buf = callResp->outputBytes;
      buf += sizeof(int32_t);
      memcpy(buf, callResp->output, callResp->outputBytes);
      buf += callResp->outputBytes;

      *(int32_t *) buf = callResp->newStateBytes;
      buf += sizeof(int32_t);
      memcpy(buf, callResp->newState, callResp->newStateBytes);
      buf += callResp->newStateBytes;
      break;
    }
    case UDF_TASK_TEARDOWN: {
      SUdfTeardownResponse *teardownResp = (SUdfTeardownResponse *) (response->subRsp);
      break;
    }
    default:
      break;
  }
  response->msgLen = buf - bufBegin;
  *(int32_t *) bufBegin = response->msgLen;
  *pBuf = bufBegin;
  *pBufLen = response->msgLen;
  return 0;
}

int32_t decodeResponse(char *bufMsg, int32_t bufLen, SUdfResponse **pResponse) {
  debugPrint("%s", "decoding response");

  if (*(int32_t *) bufMsg != bufLen) {
    debugPrint("%s", "can not decode response");
    return -1;
  }
  char *buf = bufMsg;
  SUdfResponse *rsp = taosMemoryMalloc(sizeof(SUdfResponse));
  rsp->msgLen = *(int32_t *) buf;
  buf += sizeof(int32_t);
  rsp->seqNum = *(int64_t *) buf;
  buf += sizeof(int64_t);
  rsp->type = *(int8_t *) buf;
  buf += sizeof(int8_t);
  rsp->code = *(int32_t *) buf;
  buf += sizeof(int32_t);

  switch (rsp->type) {
    case UDF_TASK_SETUP: {
      SUdfSetupResponse *setupRsp = (SUdfSetupResponse *) taosMemoryMalloc(sizeof(SUdfSetupResponse));
      setupRsp->udfHandle = *(int64_t *) buf;
      buf += sizeof(int64_t);
      rsp->subRsp = (char *) setupRsp;
      break;
    }
    case UDF_TASK_CALL: {
      SUdfCallResponse *callRsp = (SUdfCallResponse *) taosMemoryMalloc(sizeof(SUdfCallResponse));
      callRsp->outputBytes = *(int32_t *) buf;
      buf += sizeof(int32_t);

      callRsp->output = buf;
      buf += callRsp->outputBytes;

      callRsp->newStateBytes = *(int32_t *) buf;
      buf += sizeof(int32_t);

      callRsp->newState = buf;
      buf += callRsp->newStateBytes;

      rsp->subRsp = callRsp;
      break;
    }
    case UDF_TASK_TEARDOWN: {
      SUdfTeardownResponse *teardownRsp = (SUdfTeardownResponse *) taosMemoryMalloc(sizeof(SUdfTeardownResponse));
      rsp->subRsp = teardownRsp;
      break;
    }
    default:
      break;
  }
  if (buf - bufMsg != bufLen) {
    debugPrint("%s", "can not decode response");
    taosMemoryFree(rsp->subRsp);
    taosMemoryFree(rsp);
    return -1;
  }
  *pResponse = rsp;
  return 0;
}

void onUdfcPipeClose(uv_handle_t *handle) {
  SClientUvConn *conn = handle->data;
  if (!QUEUE_EMPTY(&conn->taskQueue)) {
    QUEUE* h = QUEUE_HEAD(&conn->taskQueue);
    SClientUvTaskNode *task = QUEUE_DATA(h, SClientUvTaskNode, connTaskQueue);
    task->errCode = 0;
    uv_sem_post(&task->taskSem);
    QUEUE_REMOVE(&task->procTaskQueue);
  }

  taosMemoryFree(conn->readBuf.buf);
  taosMemoryFree(conn);
  taosMemoryFree((uv_pipe_t *) handle);

}

int32_t udfcGetUvTaskResponseResult(SClientUdfTask *task, SClientUvTaskNode *uvTask) {
  debugPrint("%s", "get uv task result");
  if (uvTask->type == UV_TASK_REQ_RSP) {
    if (uvTask->rspBuf.base != NULL) {
      SUdfResponse *rsp;
      decodeResponse(uvTask->rspBuf.base, uvTask->rspBuf.len, &rsp);
      task->errCode = rsp->code;

      switch (task->type) {
        case UDF_TASK_SETUP: {
          //TODO: copy or not
          task->_setup.rsp = *(SUdfSetupResponse *) (rsp->subRsp);
          break;
        }
        case UDF_TASK_CALL: {
          task->_call.rsp = *(SUdfCallResponse *) (rsp->subRsp);
          //TODO: copy or not
          break;
        }
        case UDF_TASK_TEARDOWN: {
          task->_teardown.rsp = *(SUdfTeardownResponse *) (rsp->subRsp);
          //TODO: copy or not?
          break;
        }
        default: {
          break;
        }
      }

      // TODO: the call buffer is setup and freed by udf invocation
      taosMemoryFree(uvTask->rspBuf.base);
      taosMemoryFree(rsp->subRsp);
      taosMemoryFree(rsp);
    } else {
      task->errCode = uvTask->errCode;
    }
  } else if (uvTask->type == UV_TASK_CONNECT) {
    task->errCode = uvTask->errCode;
  } else if (uvTask->type == UV_TASK_DISCONNECT) {
    task->errCode = uvTask->errCode;
  }
  return 0;
}

void udfcAllocateBuffer(uv_handle_t *handle, size_t suggestedSize, uv_buf_t *buf) {
  debugPrint("%s", "client allocate buffer to receive from pipe");
  SClientUvConn *conn = handle->data;
  SClientConnBuf *connBuf = &conn->readBuf;

  int32_t msgHeadSize = sizeof(int32_t) + sizeof(int64_t);
  if (connBuf->cap == 0) {
    connBuf->buf = taosMemoryMalloc(msgHeadSize);
    if (connBuf->buf) {
      connBuf->len = 0;
      connBuf->cap = msgHeadSize;
      connBuf->total = -1;

      buf->base = connBuf->buf;
      buf->len = connBuf->cap;
    } else {
      //TODO: log error
      buf->base = NULL;
      buf->len = 0;
    }
  } else {
    connBuf->cap = connBuf->total > connBuf->cap ? connBuf->total : connBuf->cap;
    void *resultBuf = taosMemoryRealloc(connBuf->buf, connBuf->cap);
    if (resultBuf) {
      connBuf->buf = resultBuf;
      buf->base = connBuf->buf + connBuf->len;
      buf->len = connBuf->cap - connBuf->len;
    } else {
      //TODO: log error free connBuf->buf
      buf->base = NULL;
      buf->len = 0;
    }
  }

  debugPrint("\tconn buf cap - len - total : %d - %d - %d", connBuf->cap, connBuf->len, connBuf->total);

}

bool isUdfcUvMsgComplete(SClientConnBuf *connBuf) {
  if (connBuf->total == -1 && connBuf->len >= sizeof(int32_t)) {
    connBuf->total = *(int32_t *) (connBuf->buf);
  }
  if (connBuf->len == connBuf->cap && connBuf->total == connBuf->cap) {
    return true;
  }
  return false;
}

void udfcUvHandleRsp(SClientUvConn *conn) {
  SClientConnBuf *connBuf = &conn->readBuf;
  int64_t seqNum = *(int64_t *) (connBuf->buf + sizeof(int32_t)); // msglen int32_t then seqnum

  if (QUEUE_EMPTY(&conn->taskQueue)) {
    //LOG error
    return;
  }
  bool found = false;
  SClientUvTaskNode *taskFound = NULL;
  QUEUE* h = QUEUE_NEXT(&conn->taskQueue);
  SClientUvTaskNode *task = QUEUE_DATA(h, SClientUvTaskNode, connTaskQueue);

  while (h != &conn->taskQueue) {
    if (task->seqNum == seqNum) {
      if (found == false) {
        found = true;
        taskFound = task;
      } else {
        //LOG error;
        continue;
      }
    }
    h = QUEUE_NEXT(h);
    task = QUEUE_DATA(h, SClientUvTaskNode, connTaskQueue);
  }

  if (taskFound) {
    taskFound->rspBuf = uv_buf_init(connBuf->buf, connBuf->len);
    QUEUE_REMOVE(&taskFound->connTaskQueue);
    uv_sem_post(&taskFound->taskSem);
    QUEUE_REMOVE(&taskFound->procTaskQueue);
  } else {
    //TODO: LOG error
  }
  connBuf->buf = NULL;
  connBuf->total = -1;
  connBuf->len = 0;
  connBuf->cap = 0;
}

void udfcUvHandleError(SClientUvConn *conn) {
  uv_close((uv_handle_t *) conn->pipe, onUdfcPipeClose);
}

void onUdfcRead(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
  debugPrint("%s, nread: %zd", "client read from pipe", nread);
  if (nread == 0) return;

  SClientUvConn *conn = client->data;
  SClientConnBuf *connBuf = &conn->readBuf;
  if (nread > 0) {
    connBuf->len += nread;
    if (isUdfcUvMsgComplete(connBuf)) {
      udfcUvHandleRsp(conn);
    }

  }
  if (nread < 0) {
    debugPrint("\tclient read error: %s", uv_strerror(nread));
    if (nread == UV_EOF) {
      //TODO:
    }
    udfcUvHandleError(conn);
  }

}

void onUdfClientWrite(uv_write_t *write, int status) {
  debugPrint("%s", "after writing to pipe");
  SClientUvTaskNode *uvTask = write->data;
  if (status == 0) {
    uv_pipe_t *pipe = uvTask->pipe;
    SClientUvConn *conn = pipe->data;
    QUEUE_INSERT_TAIL(&conn->taskQueue, &uvTask->connTaskQueue);
  } else {
    //TODO Log error;
  }
  debugPrint("\tlength:%zu", uvTask->reqBuf.len);
  taosMemoryFree(write);
  taosMemoryFree(uvTask->reqBuf.base);
}

void onUdfClientConnect(uv_connect_t *connect, int status) {
  SClientUvTaskNode *uvTask = connect->data;
  uvTask->errCode = status;
  if (status != 0) {
    //TODO: LOG error
  }
  uv_read_start((uv_stream_t *) uvTask->pipe, udfcAllocateBuffer, onUdfcRead);
  taosMemoryFree(connect);
  uv_sem_post(&uvTask->taskSem);
  QUEUE_REMOVE(&uvTask->procTaskQueue);
}

int32_t createUdfcUvTask(SClientUdfTask *task, int8_t uvTaskType, SClientUvTaskNode **pUvTask) {
  SClientUvTaskNode *uvTask = taosMemoryCalloc(1, sizeof(SClientUvTaskNode));
  uvTask->type = uvTaskType;

  if (uvTaskType == UV_TASK_CONNECT) {
  } else if (uvTaskType == UV_TASK_REQ_RSP) {
    uvTask->pipe = task->session->udfSvcPipe;
    SUdfRequest request;
    request.type = task->type;
    request.seqNum = gUdfTaskSeqNum++;

    if (task->type == UDF_TASK_SETUP) {
      request.subReq = &task->_setup.req;
      request.type = UDF_TASK_SETUP;
    } else if (task->type == UDF_TASK_CALL) {
      request.subReq = &task->_call.req;
      request.type = UDF_TASK_CALL;
    } else if (task->type == UDF_TASK_TEARDOWN) {
      request.subReq = &task->_teardown.req;
      request.type = UDF_TASK_TEARDOWN;
    } else {
      //TODO log and return error
    }
    char *buf = NULL;
    int32_t bufLen = 0;
    encodeRequest(&buf, &bufLen, &request);
    uvTask->reqBuf = uv_buf_init(buf, bufLen);
    uvTask->seqNum = request.seqNum;
  } else if (uvTaskType == UV_TASK_DISCONNECT) {
    uvTask->pipe = task->session->udfSvcPipe;
  }
  uv_sem_init(&uvTask->taskSem, 0);

  *pUvTask = uvTask;
  return 0;
}

int32_t queueUvUdfTask(SClientUvTaskNode *uvTask) {
  debugPrint("%s, %d", "queue uv task", uvTask->type);

  uv_mutex_lock(&gUdfTaskQueueMutex);
  QUEUE_INSERT_TAIL(&gUdfTaskQueue, &uvTask->recvTaskQueue);
  uv_mutex_unlock(&gUdfTaskQueueMutex);
  uv_async_send(&gUdfLoopTaskAync);

  uv_sem_wait(&uvTask->taskSem);
  uv_sem_destroy(&uvTask->taskSem);

  return 0;
}

int32_t startUvUdfTask(SClientUvTaskNode *uvTask) {
  debugPrint("%s, type %d", "start uv task ", uvTask->type);
  switch (uvTask->type) {
    case UV_TASK_CONNECT: {
      uv_pipe_t *pipe = taosMemoryMalloc(sizeof(uv_pipe_t));
      uv_pipe_init(&gUdfdLoop, pipe, 0);
      uvTask->pipe = pipe;

      SClientUvConn *conn = taosMemoryMalloc(sizeof(SClientUvConn));
      conn->pipe = pipe;
      conn->readBuf.len = 0;
      conn->readBuf.cap = 0;
      conn->readBuf.buf = 0;
      conn->readBuf.total = -1;
      QUEUE_INIT(&conn->taskQueue);

      pipe->data = conn;

      uv_connect_t *connReq = taosMemoryMalloc(sizeof(uv_connect_t));
      connReq->data = uvTask;

      uv_pipe_connect(connReq, pipe, "udf.sock", onUdfClientConnect);
      break;
    }
    case UV_TASK_REQ_RSP: {
      uv_pipe_t *pipe = uvTask->pipe;
      uv_write_t *write = taosMemoryMalloc(sizeof(uv_write_t));
      write->data = uvTask;
      uv_write(write, (uv_stream_t *) pipe, &uvTask->reqBuf, 1, onUdfClientWrite);
      break;
    }
    case UV_TASK_DISCONNECT: {
      SClientUvConn *conn = uvTask->pipe->data;
      QUEUE_INSERT_TAIL(&conn->taskQueue, &uvTask->connTaskQueue);
      uv_close((uv_handle_t *) uvTask->pipe, onUdfcPipeClose);
      break;
    }
    default: {
      break;
    }
  }

  return 0;
}

void udfClientAsyncCb(uv_async_t *async) {
  QUEUE wq;

  uv_mutex_lock(&gUdfTaskQueueMutex);
  QUEUE_MOVE(&gUdfTaskQueue, &wq);
  uv_mutex_unlock(&gUdfTaskQueueMutex);

  while (!QUEUE_EMPTY(&wq)) {
    QUEUE* h = QUEUE_HEAD(&wq);
    QUEUE_REMOVE(h);
    SClientUvTaskNode *task = QUEUE_DATA(h, SClientUvTaskNode, recvTaskQueue);
    startUvUdfTask(task);
    QUEUE_INSERT_TAIL(&gUvProcTaskQueue, &task->procTaskQueue);
  }

}

void cleanUpUvTasks() {
  QUEUE wq;

  uv_mutex_lock(&gUdfTaskQueueMutex);
  QUEUE_MOVE(&gUdfTaskQueue, &wq);
  uv_mutex_unlock(&gUdfTaskQueueMutex);

  while (!QUEUE_EMPTY(&wq)) {
    QUEUE* h = QUEUE_HEAD(&wq);
    QUEUE_REMOVE(h);
    SClientUvTaskNode *task = QUEUE_DATA(h, SClientUvTaskNode, recvTaskQueue);
    if (gUdfcState == UDFC_STATE_STOPPING) {
      task->errCode = UDFC_CODE_STOPPING;
    } else if (gUdfcState == UDFC_STATE_RESTARTING) {
      task->errCode = UDFC_CODE_RESTARTING;
    }
    uv_sem_post(&task->taskSem);
  }

  // TODO: deal with tasks that are waiting result.
  while (!QUEUE_EMPTY(&gUvProcTaskQueue)) {
    QUEUE* h = QUEUE_HEAD(&gUvProcTaskQueue);
    QUEUE_REMOVE(h);
    SClientUvTaskNode *task = QUEUE_DATA(h, SClientUvTaskNode, procTaskQueue);
    if (gUdfcState == UDFC_STATE_STOPPING) {
      task->errCode = UDFC_CODE_STOPPING;
    } else if (gUdfcState == UDFC_STATE_RESTARTING) {
      task->errCode = UDFC_CODE_RESTARTING;
    }
    uv_sem_post(&task->taskSem);
  }
}

void udfStopAsyncCb(uv_async_t *async) {
  cleanUpUvTasks();
  if (gUdfcState == UDFC_STATE_STOPPING) {
    uv_stop(&gUdfdLoop);
  }
}
int32_t startUdfd();
void onUdfdExit(uv_process_t *req, int64_t exit_status, int term_signal) {
  debugPrint("Process exited with status %" PRId64 ", signal %d", exit_status, term_signal);
  uv_close((uv_handle_t *) req, NULL);
  //TODO: restart the udfd process
  if (gUdfcState == UDFC_STATE_STOPPING) {
    if (term_signal != SIGINT) {
      //TODO: log error
    }
  }
  if (gUdfcState == UDFC_STATE_READY) {
    gUdfcState = UDFC_STATE_RESTARTING;
    //TODO: asynchronous without blocking. how to do it
    cleanUpUvTasks();
    startUdfd();
  }

}

int32_t startUdfd() {
  //TODO: path
  uv_process_options_t options = {0};
  static char path[256] = {0};
  size_t cwdSize;
  uv_cwd(path, &cwdSize);
  strcat(path, "/udfd");
  char* args[2] = {path, NULL};
  options.args = args;
  options.file = path;
  options.exit_cb = onUdfdExit;
  options.stdio_count = 3;
  uv_stdio_container_t child_stdio[3];
  child_stdio[0].flags = UV_IGNORE;
  child_stdio[1].flags = UV_INHERIT_FD;
  child_stdio[1].data.fd = 1;
  child_stdio[2].flags = UV_INHERIT_FD;
  child_stdio[2].data.fd = 2;
  options.stdio = child_stdio;
  //TODO spawn error
  int err = uv_spawn(&gUdfdLoop, &gUdfdProcess, &options);
  if (err != 0) {
    debugPrint("can not spawn udfd. path: %s, error: %s", path, uv_strerror(err));
  }
  return err;
}

void constructUdfService(void *argsThread) {
  uv_loop_init(&gUdfdLoop);

  //TODO spawn error
  startUdfd();

  uv_async_init(&gUdfdLoop, &gUdfLoopTaskAync, udfClientAsyncCb);
  uv_async_init(&gUdfdLoop, &gUdfLoopStopAsync, udfStopAsyncCb);
  uv_mutex_init(&gUdfTaskQueueMutex);
  QUEUE_INIT(&gUdfTaskQueue);
  QUEUE_INIT(&gUvProcTaskQueue);
  uv_barrier_wait(&gUdfInitBarrier);
  //TODO return value of uv_run
  uv_run(&gUdfdLoop, UV_RUN_DEFAULT);
  uv_loop_close(&gUdfdLoop);
}


int32_t startUdfService() {
  gUdfcState = UDFC_STATE_STARTNG;
  uv_barrier_init(&gUdfInitBarrier, 2);
  uv_thread_create(&gUdfLoopThread, constructUdfService, 0);
  uv_barrier_wait(&gUdfInitBarrier);  gUdfcState = UDFC_STATE_READY;
  return 0;
}

int32_t stopUdfService() {
  gUdfcState = UDFC_STATE_STOPPING;
  uv_barrier_destroy(&gUdfInitBarrier);
  if (gUdfcState == UDFC_STATE_STOPPING) {
    uv_process_kill(&gUdfdProcess, SIGINT);
  }
  uv_async_send(&gUdfLoopStopAsync);
  uv_thread_join(&gUdfLoopThread);
  uv_mutex_destroy(&gUdfTaskQueueMutex);
  gUdfcState = UDFC_STATUS_FINAL;
  return 0;
}

int32_t udfcRunUvTask(SClientUdfTask *task, int8_t uvTaskType) {
  SClientUvTaskNode *uvTask = NULL;

  createUdfcUvTask(task, uvTaskType, &uvTask);
  queueUvUdfTask(uvTask);
  udfcGetUvTaskResponseResult(task, uvTask);
  if (uvTaskType == UV_TASK_CONNECT) {
    task->session->udfSvcPipe = uvTask->pipe;
  }
  taosMemoryFree(uvTask);
  uvTask = NULL;
  return task->errCode;
}

int32_t setupUdf(SUdfInfo *udfInfo, UdfHandle *handle) {
  debugPrint("%s", "client setup udf");
  SClientUdfTask *task = taosMemoryMalloc(sizeof(SClientUdfTask));
  task->errCode = 0;
  task->session = taosMemoryMalloc(sizeof(SUdfUvSession));
  task->type = UDF_TASK_SETUP;

  SUdfSetupRequest *req = &task->_setup.req;
  memcpy(req->udfName, udfInfo->udfName, 16);
  req->path = udfInfo->path;
  req->pathSize = strlen(req->path) + 1;
  req->udfType = udfInfo->udfType;
  req->scriptType = udfInfo->scriptType;

  int32_t errCode = udfcRunUvTask(task, UV_TASK_CONNECT);
  if (errCode != 0) {
    //TODO: log error
    return -1;
  }

  udfcRunUvTask(task, UV_TASK_REQ_RSP);

  SUdfSetupResponse *rsp = &task->_setup.rsp;
  task->session->severHandle = rsp->udfHandle;
  *handle = task->session;
  int32_t err = task->errCode;
  taosMemoryFree(task);
  return err;
}

int32_t callUdf(UdfHandle handle, int8_t step, char *state, int32_t stateSize, SUdfDataBlock input, char **newState,
                int32_t *newStateSize, SUdfDataBlock *output) {
  debugPrint("%s", "client call udf");

  SClientUdfTask *task = taosMemoryMalloc(sizeof(SClientUdfTask));
  task->errCode = 0;
  task->session = (SUdfUvSession *) handle;
  task->type = UDF_TASK_CALL;

  SUdfCallRequest *req = &task->_call.req;

  req->state = state;
  req->stateBytes = stateSize;
  req->inputBytes = input.size;
  req->input = input.data;
  req->udfHandle = task->session->severHandle;
  req->step = step;

  udfcRunUvTask(task, UV_TASK_REQ_RSP);

  SUdfCallResponse *rsp = &task->_call.rsp;
  *newState = rsp->newState;
  *newStateSize = rsp->newStateBytes;
  output->size = rsp->outputBytes;
  output->data = rsp->output;
  int32_t err = task->errCode;
  taosMemoryFree(task);
  return err;
}

int32_t teardownUdf(UdfHandle handle) {
  debugPrint("%s", "client teardown udf");

  SClientUdfTask *task = taosMemoryMalloc(sizeof(SClientUdfTask));
  task->errCode = 0;
  task->session = (SUdfUvSession *) handle;
  task->type = UDF_TASK_TEARDOWN;

  SUdfTeardownRequest *req = &task->_teardown.req;
  req->udfHandle = task->session->severHandle;

  udfcRunUvTask(task, UV_TASK_REQ_RSP);


  SUdfTeardownResponse *rsp = &task->_teardown.rsp;

  int32_t err = task->errCode;

  udfcRunUvTask(task, UV_TASK_DISCONNECT);

  taosMemoryFree(task->session);
  taosMemoryFree(task);

  return err;
}
