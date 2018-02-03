#include <node.h>
#include <v8.h>
#include <v8-profiler.h>
#include <uv.h>
#include <string.h>
#include <unistd.h>

namespace ClinicAgent {
static v8::Isolate* main_isolate = nullptr;
static uv_loop_t* main_loop = nullptr;
static std::vector< uint64_t > statistics_timeline;
static const uint8_t statistics_timeline_max_size = 120;

struct AgentWorker {
  uv_thread_t thread;
  uv_loop_t loop;
  uv_timer_t timer;
};
static AgentWorker* agent_worker = nullptr;

struct SnapshotMetaData {
  int snapshot_number = 0;
  std::string destination_directory = "./";
};
static SnapshotMetaData agent_snapshot_data;

void NOOP(uv_fs_t *req) {}

class FileOutputStream : public v8::OutputStream {
  uv_file fd;
  int64_t offset;
  public:
  FileOutputStream(std::string path) {
    uv_fs_t open_req;
    fd = uv_fs_open(nullptr, &open_req, path.c_str(), UV_FS_O_CREAT | UV_FS_O_SEQUENTIAL | UV_FS_O_TRUNC | UV_FS_O_WRONLY, 0644, nullptr);
    if (open_req.result < 0) {
    }
  }
  virtual void EndOfStream() {
    uv_fs_t close_req;
    uv_fs_close(nullptr, &close_req, fd, nullptr);
  }

  virtual WriteResult WriteAsciiChunk(char* data, int size) {
    uv_fs_t write_req;
    uv_buf_t buf = {
      .base = data,
      .len = static_cast<size_t>(size)
    };
    uv_fs_write(nullptr, &write_req, fd, &buf, 1, offset, nullptr);
    while (write_req.result < size) {
      if (write_req.result < 0) {
        return WriteResult::kAbort;
      }
      offset += write_req.result;
      size -= write_req.result;
      buf.base += write_req.result;
      buf.len -= write_req.result;
      uv_fs_write(nullptr, &write_req, fd, &buf, 1, offset, nullptr);
    }
    offset += write_req.result;
    return WriteResult::kContinue;
  }
};

void HeapSnapshot(v8::Isolate* isolate, SnapshotMetaData* state) {
  printf("taking snapshot\n");
  auto profiler = isolate->GetHeapProfiler();
  auto snapshot = profiler->TakeHeapSnapshot();
  auto id = state->snapshot_number;
  state->snapshot_number += 1;
  {
    auto heapsnapshot_path_std = state->destination_directory + std::to_string(uv_os_getpid()) + "." + std::to_string(id) + ".heapsnapshot";
    FileOutputStream stream(heapsnapshot_path_std);
    snapshot->Serialize(&stream);
    const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
  }
}

struct SnapshotRequestData {
  bool need_action = true;
  unsigned int pending = 0;
  uv_work_t req;

  SnapshotRequestData(unsigned int todo) : pending(todo) {}

  bool consume() {
    bool need_action = this->need_action;
    this->need_action = false;
    this->pending--;
    if (this->pending == 0) delete this;
    return need_action;
  }
};
static SnapshotRequestData* current_snapshot_req = nullptr;
void work() {
  v8::HandleScope scope(main_isolate);
  v8::Isolate::DisallowJavascriptExecutionScope no_js(main_isolate, v8::Isolate::DisallowJavascriptExecutionScope::OnFailure::CRASH_ON_FAILURE);
  v8::Isolate::SuppressMicrotaskExecutionScope no_tasks(main_isolate);
  v8::HeapStatistics stats;
  main_isolate->GetHeapStatistics(&stats);
  statistics_timeline.insert(statistics_timeline.begin(), stats.used_heap_size());
  if (statistics_timeline.size() > statistics_timeline_max_size) {
    statistics_timeline.resize(statistics_timeline_max_size);
  }
  uint64_t sum = 0;
  for (auto used_heap_size : statistics_timeline) {
    sum += used_heap_size;
  }
  uint64_t mean = sum / statistics_timeline.size();
  printf("size %d available %d percent %d\n", stats.used_heap_size(), stats.heap_size_limit(), (stats.used_heap_size() * 100) / stats.heap_size_limit());
  HeapSnapshot(main_isolate, &agent_snapshot_data);
  current_snapshot_req = nullptr;
}
void INTERRUPT_HeapSnapshot(v8::Isolate* isolate, void* data) {
  auto snap_req = static_cast<SnapshotRequestData*>(data);
  if (snap_req->consume()) {
    work();
  }
}
void NO_WORK(uv_work_t* req) {}
void AFTER_WORK_HeapSnapshot(uv_work_t* req, int status) {
  auto snap_req = static_cast<SnapshotRequestData*>(req->data);
  if (status != UV_ECANCELED) {
    if (snap_req->consume()) {
      work();
    }
  }
}
void agent_callback(void* data) {
  AgentWorker* worker = static_cast<AgentWorker*>(data);
  uv_run(&worker->loop, UV_RUN_DEFAULT);
  uv_loop_close(&worker->loop);
  delete worker;
}
void wake_agent(uv_timer_t* handle) {
  if (current_snapshot_req == nullptr) {
    SnapshotRequestData* snap_req = new SnapshotRequestData(2);
    current_snapshot_req = snap_req;
    snap_req->req.data = snap_req;
    uv_queue_work(main_loop, &snap_req->req, NO_WORK, AFTER_WORK_HeapSnapshot);
    main_isolate->RequestInterrupt(INTERRUPT_HeapSnapshot, snap_req);
  } else {
    printf("still waiting\n");
  }
}
void Enable(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (agent_worker != nullptr) return; 
  auto isolate = args.GetIsolate();
  auto worker = agent_worker = new AgentWorker();
  {
    auto result = uv_loop_init(&worker->loop);
    if (result >= 0) {
      auto result = uv_timer_init(&worker->loop, &worker->timer);
      if (result >= 0) {
        // every 10 seconds
        uv_timer_start(&worker->timer, wake_agent, 0, 2000);
        {
          auto result = uv_thread_create(&worker->thread, agent_callback, worker);
          if (result >= 0) return;
        }
      }
    }
  }
  isolate->ThrowException(
    v8::Exception::Error(
      v8::String::NewFromOneByte(
        isolate,
        reinterpret_cast<const uint8_t*>("cannot allocate agent polling timer"),
        v8::NewStringType::kNormal
      ).ToLocalChecked()
    )
  );
}

void Disable(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (agent_worker == nullptr) return; 
  auto result = uv_timer_stop(&agent_worker->timer);
  if (result >= 0) {
    agent_worker = nullptr;
    return;
  }
  auto isolate = args.GetIsolate();
  isolate->ThrowException(
    v8::Exception::Error(
      v8::String::NewFromOneByte(
        isolate,
        reinterpret_cast<const uint8_t*>("cannot deallocate agent polling timer"),
        v8::NewStringType::kNormal
      ).ToLocalChecked()
    )
  );
}

void GetObjectBySnapshotId(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto isolate = args.GetIsolate();
  auto profiler = isolate->GetHeapProfiler();
  int id = 0;
  if (args[0]->IsNumber()) {
    id = args[0]->ToInteger()->Value();
  }
  if (id <= 0) {
    isolate->ThrowException(
      v8::Exception::Error(
        v8::String::NewFromOneByte(
          isolate,
          reinterpret_cast<const uint8_t*>("expected positive integer id"),
          v8::NewStringType::kNormal
        ).ToLocalChecked()
      )
    );
  }
  auto obj = profiler->FindObjectById(id);
  if (obj.IsEmpty()) {
    isolate->ThrowException(
      v8::Exception::Error(
        v8::String::NewFromOneByte(
          isolate,
          reinterpret_cast<const uint8_t*>("no object found, have you taken a snapshot?"),
          v8::NewStringType::kNormal
        ).ToLocalChecked()
      )
    );
    return;
  }
  args.GetReturnValue().Set(obj);
}

void GetSnapshotIdOfObject(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto isolate = args.GetIsolate();
  auto profiler = isolate->GetHeapProfiler();
  auto id = profiler->GetObjectId(args[0]);
  if (id == 0) {
    isolate->ThrowException(
      v8::Exception::Error(
        v8::String::NewFromOneByte(
          isolate,
          reinterpret_cast<const uint8_t*>("no snapshots found, take one first"),
          v8::NewStringType::kNormal
        ).ToLocalChecked()
      )
    );
  }
  args.GetReturnValue().Set(v8::Integer::New(isolate, id));
}

void init(v8::Local<v8::Object> exports) {
  main_loop = uv_default_loop();
  auto ctx = exports->CreationContext();
  auto isolate = ctx->GetIsolate();
  main_isolate = isolate;
  NODE_SET_METHOD(exports, "enable" , ClinicAgent::Enable);
  NODE_SET_METHOD(exports, "getObjectBySnapshotId" , ClinicAgent::GetObjectBySnapshotId);
  NODE_SET_METHOD(exports, "getSnapshotIdOfObject" , ClinicAgent::GetSnapshotIdOfObject);
  NODE_SET_METHOD(exports, "disable" , ClinicAgent::Disable);
}

NODE_MODULE(NODE_GYP_MODULE_NAME, init)
}