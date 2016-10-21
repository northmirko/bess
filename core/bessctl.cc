#include "bessctl.grpc.pb.h"
#include "message.h"
#include "module.h"
#include "opts.h"
#include "port.h"
#include "tc.h"
#include "time.h"
#include "worker.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using grpc::ClientContext;

using bess::BESSControl;
using bess::Error;
using bess::Empty;
using bess::AddTcRequest;
using bess::AddWorkerRequest;
using bess::AttachTaskRequest;
using bess::ConnectModulesRequest;
using bess::CreateModuleRequest;
using bess::CreateModuleResponse;
using bess::CreatePortRequest;
using bess::CreatePortResponse;
using bess::DestroyModuleRequest;
using bess::DestroyPortRequest;
using bess::DisableTcpdumpRequest;
using bess::DisconnectModulesRequest;
using bess::Empty;
using bess::EnableTcpdumpRequest;
using bess::Error;
using bess::GetDriverInfoRequest;
using bess::GetDriverInfoResponse;
using bess::GetModuleInfoRequest;
using bess::GetModuleInfoResponse;
using bess::GetModuleInfoResponse_Attribute;
using bess::GetModuleInfoResponse_IGate;
using bess::GetModuleInfoResponse_IGate_OGate;
using bess::GetModuleInfoResponse_OGate;
using bess::GetPortStatsRequest;
using bess::GetPortStatsResponse;
using bess::GetPortStatsResponse_Stat;
using bess::GetTcStatsRequest;
using bess::GetTcStatsResponse;
using bess::ListDriversResponse;
using bess::ListModulesResponse;
using bess::ListModulesResponse_Module;
using bess::ListPortsResponse;
using bess::ListTcsRequest;
using bess::ListTcsResponse;
using bess::ListTcsResponse_TrafficClassStatus;
using bess::ListWorkersResponse;
using bess::ListWorkersResponse_WorkerStatus;
using bess::TrafficClass;
using bess::TrafficClass_Resource;
using bess::EmptyResponse;

template <typename T>
static inline Status return_with_error(T* response, int code, const char* fmt,
                                       ...) {
  va_list ap;
  va_start(ap, fmt);
  response->mutable_error()->set_err(code);
  response->mutable_error()->set_errmsg(string_vformat(fmt, ap));
  va_end(ap);
  return Status::OK;
}

template <typename T>
static inline Status return_with_errno(T* response, int code) {
  response->mutable_error()->set_err(code);
  response->mutable_error()->set_errmsg(strerror(code));
  return Status::OK;
}

static int collect_igates(Module* m, GetModuleInfoResponse* response) {
  for (int i = 0; i < m->igates.curr_size; i++) {
    if (!is_active_gate(&m->igates, i)) continue;

    GetModuleInfoResponse_IGate* igate = response->add_igates();
    struct gate* g = m->igates.arr[i];
    struct gate* og;

    igate->set_igate(i);

    cdlist_for_each_entry(og, &g->in.ogates_upstream, out.igate_upstream) {
      GetModuleInfoResponse_IGate_OGate* ogate = igate->add_ogates();
      ogate->set_ogate(og->gate_idx);
      ogate->set_name(og->m->Name());
    }
  }

  return 0;
}

static int collect_ogates(Module* m, GetModuleInfoResponse* response) {
  for (int i = 0; i < m->ogates.curr_size; i++) {
    if (!is_active_gate(&m->ogates, i)) continue;
    GetModuleInfoResponse_OGate* ogate = response->add_ogates();
    struct gate* g = m->ogates.arr[i];

    ogate->set_ogate(i);
#if TRACK_GATES
    ogate->set_cnt(g->cnt);
    ogate->set_pkts(g->pkts);
    ogate->set_timestamp(get_epoch_time());
#endif
    ogate->set_name(g->out.igate->m->Name());
    ogate->set_igate(g->out.igate->gate_idx);
  }

  return 0;
}

static int collect_metadata(Module* m, GetModuleInfoResponse* response) {
  for (int i = 0; i < m->num_attrs; i++) {
    GetModuleInfoResponse_Attribute* attr = response->add_metadata();

    attr->set_name(m->attrs[i].name);
    attr->set_size(m->attrs[i].size);

    switch (m->attrs[i].mode) {
      case MT_READ:
        attr->set_mode("read");
        break;
      case MT_WRITE:
        attr->set_mode("write");
        break;
      case MT_UPDATE:
        attr->set_mode("update");
        break;
      default:
        assert(0);
    }

    attr->set_offset(m->attr_offsets[i]);
  }

  return 0;
}

class BESSControlImpl final : public BESSControl::Service {
 public:
  Status ResetAll(ClientContext* context, const Empty& request,
                  EmptyResponse* response) {
    Status status;
    status = ResetModules(context, request, response);
    if (response->error().err() != 0) {
      return status;
    }
    status = ResetPorts(context, request, response);
    if (response->error().err() != 0) {
      return status;
    }
    status = ResetTcs(context, request, response);
    if (response->error().err() != 0) {
      return status;
    }
    status = ResetWorkers(context, request, response);
    if (response->error().err() != 0) {
      return status;
    }
    return Status::OK;
  }
  Status PauseAll(ClientContext* context, const Empty& request,
                  EmptyResponse* response) {
    pause_all_workers();
    log_info("*** All workers have been paused ***\n");
    return Status::OK;
  }
  Status ResumeAll(ClientContext* context, const Empty& request,
                   EmptyResponse* response) {
    log_info("*** Resuming ***\n");
    resume_all_workers();
    return Status::OK;
  }
  Status ResetWorkers(ClientContext* context, const Empty& request,
                      EmptyResponse* response) {
    destroy_all_workers();
    log_info("*** All workers have been destroyed ***\n");
    return Status::OK;
  }
  Status ListWorkers(ClientContext* context, const Empty& request,
                     ListWorkersResponse* response) {
    for (int wid = 0; wid < MAX_WORKERS; wid++) {
      if (!is_worker_active(wid)) continue;
      ListWorkersResponse_WorkerStatus* status = response->add_workers_status();
      status->set_wid(wid);
      status->set_running(is_worker_running(wid));
      status->set_core(workers[wid]->core);
      status->set_num_tcs(workers[wid]->s->num_classes);
      status->set_silent_drops(workers[wid]->silent_drops);
    }
    return Status::OK;
  }
  Status AddWorker(ClientContext* context, const AddWorkerRequest& request,
                   EmptyResponse* response) {
    uint64_t wid = request.wid();
    if (wid >= MAX_WORKERS) {
      return return_with_error(response, EINVAL, "Missing 'wid' field");
    }
    uint64_t core = request.core();
    if (!is_cpu_present(core)) {
      return return_with_error(response, EINVAL, "Invalid core %d", core);
    }
    if (is_worker_active(wid)) {
      return return_with_error(response, EEXIST, "worker:%d is already active",
                               wid);
    }
    launch_worker(wid, core);
    return Status::OK;
  }
  Status ResetTcs(ClientContext* context, const Empty& request,
                  EmptyResponse* response) {
    struct ns_iter iter;
    struct tc* c;

    struct tc** c_arr;
    size_t arr_slots = 1024;
    size_t n = 0;

    c_arr = (struct tc**)malloc(arr_slots * sizeof(struct tc*));

    ns_init_iterator(&iter, NS_TYPE_TC);

    while ((c = (struct tc*)ns_next(&iter)) != NULL) {
      if (n >= arr_slots) {
        arr_slots *= 2;
        c_arr = (struct tc**)realloc(c_arr, arr_slots * sizeof(struct tc*));
      }

      c_arr[n] = c;
      n++;
    }

    ns_release_iterator(&iter);

    for (size_t i = 0; i < n; i++) {
      c = c_arr[i];

      if (c->num_tasks) {
        free(c_arr);
        return return_with_error(response, EBUSY, "TC %s still has %d tasks",
                                 c->settings.name, c->num_tasks);
      }

      if (c->settings.auto_free) continue;

      tc_leave(c);
      tc_dec_refcnt(c);
    }

    free(c_arr);
    return Status::OK;
  }
  Status ListTcs(ClientContext* context, const ListTcsRequest& request,
                 ListTcsResponse* response) {
    unsigned int wid_filter = MAX_WORKERS;

    struct ns_iter iter;

    struct tc* c;

    wid_filter = request.wid();
    if (wid_filter >= 0) {
      if (wid_filter >= MAX_WORKERS) {
        return return_with_error(response, EINVAL,
                                 "'wid' must be between 0 and %d",
                                 MAX_WORKERS - 1);
      }

      if (!is_worker_active(wid_filter)) {
        return return_with_error(response, EINVAL, "worker:%d does not exist",
                                 wid_filter);
      }
    }

    ns_init_iterator(&iter, NS_TYPE_TC);

    while ((c = (struct tc*)ns_next(&iter)) != NULL) {
      int wid;

      if (wid_filter < MAX_WORKERS) {
        if (workers[wid_filter]->s != c->s) continue;
        wid = wid_filter;
      } else {
        for (wid = 0; wid < MAX_WORKERS; wid++)
          if (is_worker_active(wid) && workers[wid]->s == c->s) break;
      }

      ListTcsResponse_TrafficClassStatus* status =
          response->add_classes_status();

      status->set_parent(c->parent->settings.name);
      status->set_tasks(c->num_tasks);

      status->mutable_class_()->set_name(c->settings.name);
      status->mutable_class_()->set_priority(c->settings.priority);

      if (wid < MAX_WORKERS)
        status->mutable_class_()->set_wid(wid);
      else
        status->mutable_class_()->set_wid(-1);

      status->mutable_class_()->mutable_limit()->set_schedules(
          c->settings.limit[0]);
      status->mutable_class_()->mutable_limit()->set_cycles(
          c->settings.limit[1]);
      status->mutable_class_()->mutable_limit()->set_packets(
          c->settings.limit[2]);
      status->mutable_class_()->mutable_limit()->set_bits(c->settings.limit[3]);

      status->mutable_class_()->mutable_max_burst()->set_schedules(
          c->settings.max_burst[0]);
      status->mutable_class_()->mutable_max_burst()->set_cycles(
          c->settings.max_burst[1]);
      status->mutable_class_()->mutable_max_burst()->set_packets(
          c->settings.max_burst[2]);
      status->mutable_class_()->mutable_max_burst()->set_bits(
          c->settings.max_burst[3]);
    }

    ns_release_iterator(&iter);

    return Status::OK;
  }
  Status AddTc(ClientContext* context, const AddTcRequest& request,
               EmptyResponse* response) {
    int wid;

    struct tc_params params;
    struct tc* c;

    const char* tc_name = request.class_().name().c_str();
    if (request.class_().name().length() == 0) {
      return return_with_error(response, EINVAL, "Missing 'name' field");
    }

    if (!ns_is_valid_name(tc_name)) {
      return return_with_error(response, EINVAL, "'%s' is an invalid name",
                               tc_name);
    }

    if (ns_name_exists(tc_name)) {
      return return_with_error(response, EINVAL, "Name '%s' already exists",
                               tc_name);
    }

    wid = request.class_().wid();
    if (wid >= MAX_WORKERS) {
      return return_with_error(
          response, EINVAL, "'wid' must be between 0 and %d", MAX_WORKERS - 1);
    }

    if (!is_worker_active(wid)) {
      if (num_workers == 0 && wid == 0)
        launch_worker(wid, global_opts.default_core);
      else {
        return return_with_error(response, EINVAL, "worker:%d does not exist",
                                 wid);
      }
    }

    memset(&params, 0, sizeof(params));
    strcpy(params.name, tc_name);

    params.priority = request.class_().priority();
    if (params.priority == DEFAULT_PRIORITY)
      return return_with_error(response, EINVAL, "Priority %d is reserved",
                               DEFAULT_PRIORITY);

    /* TODO: add support for other parameters */
    params.share = 1;
    params.share_resource = RESOURCE_CNT;

    if (request.class_().has_limit()) {
      params.limit[0] = request.class_().limit().schedules();
      params.limit[1] = request.class_().limit().cycles();
      params.limit[2] = request.class_().limit().packets();
      params.limit[3] = request.class_().limit().bits();
    }

    if (request.class_().has_max_burst()) {
      params.max_burst[0] = request.class_().max_burst().schedules();
      params.max_burst[1] = request.class_().max_burst().cycles();
      params.max_burst[2] = request.class_().max_burst().packets();
      params.max_burst[3] = request.class_().max_burst().bits();
    }

    c = tc_init(workers[wid]->s, &params);
    if (is_err(c))
      return return_with_error(response, -ptr_to_err(c), "tc_init() failed");

    tc_join(c);

    return Status::OK;
  }
  Status GetTcStats(ClientContext* context, const GetTcStatsRequest& request,
                    GetTcStatsResponse* response) {
    const char* tc_name = request.name().c_str();

    struct tc* c;

    if (request.name().length() == 0)
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");

    c = (struct tc*)ns_lookup(NS_TYPE_TC, tc_name);
    if (!c)
      return return_with_error(response, ENOENT, "No TC '%s' found", tc_name);

    response->set_timestamp(get_epoch_time());
    response->set_count(c->stats.usage[RESOURCE_CNT]);
    response->set_cycles(c->stats.usage[RESOURCE_CYCLE]);
    response->set_packets(c->stats.usage[RESOURCE_PACKET]);
    response->set_bits(c->stats.usage[RESOURCE_BIT]);

    return Status::OK;
  }
  Status ListDrivers(ClientContext* context, const Empty& request,
                     ListDriversResponse* response) {
    int cnt = 1;
    int offset;

    for (offset = 0; cnt != 0; offset += cnt) {
      const int arr_size = 16;
      const Driver* drivers[arr_size];

      int i;

      cnt = list_drivers(drivers, arr_size, offset);

      for (i = 0; i < cnt; i++) {
        response->add_driver_names(drivers[i]->Name());
      }
    };

    return Status::OK;
  }
  Status GetDriverInfo(ClientContext* context,
                       const GetDriverInfoRequest& request,
                       GetDriverInfoResponse* response) {
    const char* drv_name;
    const Driver* drv;

    if (request.driver_name().length() == 0)
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");

    drv_name = request.driver_name().c_str();

    if ((drv = find_driver(drv_name)) == NULL)
      return return_with_error(response, ENOENT, "No module class '%s' found",
                               drv_name);

#if 0
                        for (int i = 0; i < MAX_COMMANDS; i++) {
                          if (!drv->commands[i].cmd)
                            break;
                          response->add_commands(drv->commands[i].cmd);
                        }
#endif
    response->set_name(drv->Name());
    response->set_help(drv->Help());

    return Status::OK;
  }
  Status ResetPorts(ClientContext* context, const Empty& request,
                    EmptyResponse* response) {
    Port* p;
    while (list_ports((const Port**)&p, 1, 0)) {
      int ret = destroy_port(p);
      if (ret) {
        return return_with_errno(response, -ret);
      }
    }

    log_info("*** All ports have been destroyed ***\n");
    return Status::OK;
  }
  Status ListPorts(ClientContext* context, const Empty& request,
                   ListPortsResponse* response) {
    int cnt = 1;
    int offset;

    for (offset = 0; cnt != 0; offset += cnt) {
      const int arr_size = 16;
      const Port* ports[arr_size];

      int i;

      cnt = list_ports(ports, arr_size, offset);

      for (i = 0; i < cnt; i++) {
        bess::Port* port = response->add_ports();
        port->set_name(ports[i]->Name());
        port->set_driver(ports[i]->GetDriver()->Name());
      }
    };

    return Status::OK;
  }
  Status CreatePort(ClientContext* context, const CreatePortRequest& request,
                    CreatePortResponse* response) {
    const char* driver_name;
    const Driver* driver;
    Port* port;

    if (request.port().driver().length() == 0)
      return return_with_error(response, EINVAL, "Missing 'driver' field");

    driver_name = request.port().driver().c_str();
    driver = find_driver(driver_name);
    if (!driver) {
      return return_with_error(response, ENOENT, "No port driver '%s' found",
                               driver_name);
    }

    Error* error = response->mutable_error();

    switch (request.arg_case()) {
      case CreatePortRequest::kPcapArg:
        port = create_port(
            request.port().name().c_str(), driver, request.num_inc_q(),
            request.num_out_q(), request.size_inc_q(), request.size_out_q(),
            request.mac_addr().c_str(), request.pcap_arg(), error);
        break;
      case CreatePortRequest::kPmdArg:
        port = create_port(
            request.port().name().c_str(), driver, request.num_inc_q(),
            request.num_out_q(), request.size_inc_q(), request.size_out_q(),
            request.mac_addr().c_str(), request.pmd_arg(), error);
        break;
      case CreatePortRequest::kSocketArg:
        port = create_port(
            request.port().name().c_str(), driver, request.num_inc_q(),
            request.num_out_q(), request.size_inc_q(), request.size_out_q(),
            request.mac_addr().c_str(), request.socket_arg(), error);
        break;
      case CreatePortRequest::kZcvportArg:
        port = create_port(
            request.port().name().c_str(), driver, request.num_inc_q(),
            request.num_out_q(), request.size_inc_q(), request.size_out_q(),
            request.mac_addr().c_str(), request.zcvport_arg(), error);
        break;
      case CreatePortRequest::kVportArg:
        port = create_port(
            request.port().name().c_str(), driver, request.num_inc_q(),
            request.num_out_q(), request.size_inc_q(), request.size_out_q(),
            request.mac_addr().c_str(), request.vport_arg(), error);
        break;
      case CreatePortRequest::ARG_NOT_SET:
        return return_with_error(response, CreatePortRequest::ARG_NOT_SET,
                                 "Missing argument");
    }

    if (!port) return Status::OK;

    response->set_name(port->Name());
    return Status::OK;
  }
  Status DestroyPort(ClientContext* context, const DestroyPortRequest& request,
                     EmptyResponse* response) {
    const char* port_name;

    Port* port;

    int ret;

    if (!request.name().length())
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");

    port_name = request.name().c_str();
    port = find_port(port_name);
    if (!port)
      return return_with_error(response, ENOENT, "No port `%s' found",
                               port_name);

    ret = destroy_port(port);
    if (ret) {
      return return_with_errno(response, -ret);
    }

    return Status::OK;
  }
  Status GetPortStats(ClientContext* context,
                      const GetPortStatsRequest& request,
                      GetPortStatsResponse* response) {
    const char* port_name;

    Port* port;

    port_stats_t stats;

    if (!request.name().length())
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");
    port_name = request.name().c_str();

    port = find_port(port_name);
    if (!port)
      return return_with_error(response, ENOENT, "No port '%s' found",
                               port_name);

    get_port_stats(port, &stats);

    response->mutable_inc()->set_packets(stats[PACKET_DIR_INC].packets);
    response->mutable_inc()->set_dropped(stats[PACKET_DIR_INC].dropped);
    response->mutable_inc()->set_bytes(stats[PACKET_DIR_INC].bytes);

    response->mutable_out()->set_packets(stats[PACKET_DIR_OUT].packets);
    response->mutable_out()->set_dropped(stats[PACKET_DIR_OUT].dropped);
    response->mutable_out()->set_bytes(stats[PACKET_DIR_OUT].bytes);

    response->set_timestamp(get_epoch_time());

    return Status::OK;
  }
  Status ResetModules(ClientContext* context, const Empty& request,
                      EmptyResponse* response) {
    Module* m;

    while (list_modules((const Module**)&m, 1, 0)) destroy_module(m);

    log_info("*** All modules have been destroyed ***\n");
    return Status::OK;
  }
  Status ListModules(ClientContext* context, const Empty& request,
                     ListModulesResponse* response) {
    int cnt = 1;
    int offset;

    for (offset = 0; cnt != 0; offset += cnt) {
      const int arr_size = 16;
      const Module* modules[arr_size];

      int i;

      cnt = list_modules(modules, arr_size, offset);

      ListModulesResponse_Module* module = response->add_modules();

      for (i = 0; i < cnt; i++) {
        const Module* m = modules[i];

        module->set_name(m->Name());
        module->set_mclass(m->Class()->Name());
        module->set_desc(m->GetDesc());
      }
    };

    return Status::OK;
  }
  Status CreateModule(ClientContext* context,
                      const CreateModuleRequest& request,
                      CreateModuleResponse* response) {
    const char* mclass_name;
    const ModuleClass* mclass;
    Module* module;

    if (!request.mclass().length())
      return return_with_error(response, EINVAL, "Missing 'mclass' field");
    mclass_name = request.mclass().c_str();

    mclass = find_mclass(mclass_name);
    if (!mclass)
      return return_with_error(response, ENOENT, "No mclass '%s' found",
                               mclass_name);

    Error* error = response->mutable_error();
    // TODO: Argument!
    switch (request.arg_case()) {
      case CreateModuleRequest::kBpfArg:
        module = create_module(request.name().c_str(), mclass,
                               request.bpf_arg(), error);
        break;
      case CreateModuleRequest::kBufferArg:
        module = create_module(request.name().c_str(), mclass,
                               request.buffer_arg(), error);
        break;
      case CreateModuleRequest::kBypassArg:
        module = create_module(request.name().c_str(), mclass,
                               request.bypass_arg(), error);
        break;
      case CreateModuleRequest::kDumpArg:
        module = create_module(request.name().c_str(), mclass,
                               request.dump_arg(), error);
        break;
      case CreateModuleRequest::kEtherEncapArg:
        module = create_module(request.name().c_str(), mclass,
                               request.ether_encap_arg(), error);
        break;
      case CreateModuleRequest::kExactMatchArg:
        module = create_module(request.name().c_str(), mclass,
                               request.exact_match_arg(), error);
        break;
      case CreateModuleRequest::kFlowGenArg:
        module = create_module(request.name().c_str(), mclass,
                               request.flow_gen_arg(), error);
        break;
      case CreateModuleRequest::kGenericDecapArg:
        module = create_module(request.name().c_str(), mclass,
                               request.generic_decap_arg(), error);
        break;
      case CreateModuleRequest::kGenericEncapArg:
        module = create_module(request.name().c_str(), mclass,
                               request.generic_encap_arg(), error);
        break;
      case CreateModuleRequest::kHashLbArg:
        module = create_module(request.name().c_str(), mclass,
                               request.hash_lb_arg(), error);
        break;
      case CreateModuleRequest::kIpEncapArg:
        module = create_module(request.name().c_str(), mclass,
                               request.ip_encap_arg(), error);
        break;
      case CreateModuleRequest::kIpLookupArg:
        module = create_module(request.name().c_str(), mclass,
                               request.ip_lookup_arg(), error);
        break;
      case CreateModuleRequest::kL2ForwardArg:
        module = create_module(request.name().c_str(), mclass,
                               request.l2_forward_arg(), error);
        break;
      case CreateModuleRequest::kMacSwapArg:
        module = create_module(request.name().c_str(), mclass,
                               request.mac_swap_arg(), error);
        break;
      case CreateModuleRequest::kMeasureArg:
        module = create_module(request.name().c_str(), mclass,
                               request.measure_arg(), error);
        break;
      case CreateModuleRequest::kMergeArg:
        module = create_module(request.name().c_str(), mclass,
                               request.merge_arg(), error);
        break;
      case CreateModuleRequest::kMetadataTestArg:
        module = create_module(request.name().c_str(), mclass,
                               request.metadata_test_arg(), error);
        break;
      case CreateModuleRequest::kNoopArg:
        module = create_module(request.name().c_str(), mclass,
                               request.noop_arg(), error);
        break;
      case CreateModuleRequest::kPortIncArg:
        module = create_module(request.name().c_str(), mclass,
                               request.port_inc_arg(), error);
        break;
      case CreateModuleRequest::kPortOutArg:
        module = create_module(request.name().c_str(), mclass,
                               request.port_out_arg(), error);
        break;
      case CreateModuleRequest::kQueueIncArg:
        module = create_module(request.name().c_str(), mclass,
                               request.queue_inc_arg(), error);
        break;
      case CreateModuleRequest::kQueueOutArg:
        module = create_module(request.name().c_str(), mclass,
                               request.queue_out_arg(), error);
        break;
      case CreateModuleRequest::kQueueArg:
        module = create_module(request.name().c_str(), mclass,
                               request.queue_arg(), error);
        break;
      case CreateModuleRequest::kRandomUpdateArg:
        module = create_module(request.name().c_str(), mclass,
                               request.random_update_arg(), error);
        break;
      case CreateModuleRequest::kRewriteArg:
        module = create_module(request.name().c_str(), mclass,
                               request.rewrite_arg(), error);
        break;
      case CreateModuleRequest::kRoundRobinArg:
        module = create_module(request.name().c_str(), mclass,
                               request.round_robin_arg(), error);
        break;
      case CreateModuleRequest::kSetMetadataArg:
        module = create_module(request.name().c_str(), mclass,
                               request.set_metadata_arg(), error);
        break;
      case CreateModuleRequest::kSinkArg:
        module = create_module(request.name().c_str(), mclass,
                               request.sink_arg(), error);
        break;
      case CreateModuleRequest::kSourceArg:
        module = create_module(request.name().c_str(), mclass,
                               request.source_arg(), error);
        break;
      case CreateModuleRequest::kSplitArg:
        module = create_module(request.name().c_str(), mclass,
                               request.split_arg(), error);
        break;
      case CreateModuleRequest::kTimestampArg:
        module = create_module(request.name().c_str(), mclass,
                               request.timestamp_arg(), error);
        break;
      case CreateModuleRequest::kUpdateArg:
        module = create_module(request.name().c_str(), mclass,
                               request.update_arg(), error);
        break;
      case CreateModuleRequest::kVlanPopArg:
        module = create_module(request.name().c_str(), mclass,
                               request.vlan_pop_arg(), error);
        break;
      case CreateModuleRequest::kVlanPushArg:
        module = create_module(request.name().c_str(), mclass,
                               request.vlan_push_arg(), error);
        break;
      case CreateModuleRequest::kVlanSplitArg:
        module = create_module(request.name().c_str(), mclass,
                               request.vlan_split_arg(), error);
        break;
      case CreateModuleRequest::kVxlanEncapArg:
        module = create_module(request.name().c_str(), mclass,
                               request.vxlan_encap_arg(), error);
        break;
      case CreateModuleRequest::kVxlanDecapArg:
        module = create_module(request.name().c_str(), mclass,
                               request.vxlan_decap_arg(), error);
        break;
      case CreateModuleRequest::kWildcardMatchArg:
        module = create_module(request.name().c_str(), mclass,
                               request.wildcard_match_arg(), error);
        break;
      case CreateModuleRequest::ARG_NOT_SET:
        return return_with_error(response, CreateModuleRequest::ARG_NOT_SET,
                                 "Missing argument");
    }

    if (!module) return Status::OK;

    response->set_name(module->Name());
    return Status::OK;
  }
  Status DestroyModule(ClientContext* context,
                       const DestroyModuleRequest& request,
                       EmptyResponse* response) {
    const char* m_name;
    Module* m;

    if (!request.name().length())
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");
    m_name = request.name().c_str();

    if ((m = find_module(m_name)) == NULL)
      return return_with_error(response, ENOENT, "No module '%s' found",
                               m_name);

    destroy_module(m);

    return Status::OK;
  }
  Status GetModuleInfo(ClientContext* context,
                       const GetModuleInfoRequest& request,
                       GetModuleInfoResponse* response) {
    const char* m_name;
    Module* m;

    if (!request.name().length())
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");
    m_name = request.name().c_str();

    if ((m = find_module(m_name)) == NULL)
      return return_with_error(response, ENOENT, "No module '%s' found",
                               m_name);

    response->set_name(m->Name());
    response->set_mclass(m->Class()->Name());

    response->set_desc(m->GetDesc());

    // TODO: Dump!

    collect_igates(m, response);
    collect_ogates(m, response);
    collect_metadata(m, response);

    return Status::OK;
  }
  Status ConnectModules(ClientContext* context,
                        const ConnectModulesRequest& request,
                        EmptyResponse* response) {
    const char* m1_name;
    const char* m2_name;
    gate_idx_t ogate;
    gate_idx_t igate;

    Module* m1;
    Module* m2;

    int ret;

    m1_name = request.m1().c_str();
    m2_name = request.m2().c_str();
    ogate = request.ogate();
    igate = request.igate();

    if (!m1_name || !m2_name)
      return return_with_error(response, EINVAL, "Missing 'm1' or 'm2' field");

    if ((m1 = find_module(m1_name)) == NULL)
      return return_with_error(response, ENOENT, "No module '%s' found",
                               m1_name);

    if ((m2 = find_module(m2_name)) == NULL)
      return return_with_error(response, ENOENT, "No module '%s' found",
                               m2_name);

    ret = connect_modules(m1, ogate, m2, igate);
    if (ret < 0)
      return return_with_error(response, -ret, "Connection %s:%d->%d:%s failed",
                               m1_name, ogate, igate, m2_name);

    return Status::OK;
  }
  Status DisconnectModules(ClientContext* context,
                           const DisconnectModulesRequest& request,
                           EmptyResponse* response) {
    const char* m_name;
    gate_idx_t ogate;

    Module* m;

    int ret;

    m_name = request.name().c_str();
    ogate = request.ogate();

    if (!request.name().length())
      return return_with_error(response, EINVAL, "Missing 'name' field");

    if ((m = find_module(m_name)) == NULL)
      return return_with_error(response, ENOENT, "No module '%s' found",
                               m_name);

    ret = disconnect_modules(m, ogate);
    if (ret < 0)
      return return_with_error(response, -ret, "Disconnection %s:%d failed",
                               m_name, ogate);

    return Status::OK;
  }
  Status AttachTask(ClientContext* context, const AttachTaskRequest& request,
                    EmptyResponse* response) {
    const char* m_name;
    const char* tc_name;

    task_id_t tid;

    Module* m;
    struct task* t;

    m_name = request.name().c_str();

    if (!request.name().length())
      return return_with_error(response, EINVAL, "Missing 'name' field");

    if ((m = find_module(m_name)) == NULL)
      return return_with_error(response, ENOENT, "No module '%s' found",
                               m_name);

    tid = request.taskid();
    if (tid >= MAX_TASKS_PER_MODULE)
      return return_with_error(response, EINVAL,
                               "'taskid' must be between 0 and %d",
                               MAX_TASKS_PER_MODULE - 1);

    if ((t = m->tasks[tid]) == NULL)
      return return_with_error(response, ENOENT, "Task %s:%hu does not exist",
                               m_name, tid);

    tc_name = request.tc().c_str();

    if (request.tc().length() > 0) {
      struct tc* c;

      c = (struct tc*)ns_lookup(NS_TYPE_TC, tc_name);
      if (!c)
        return return_with_error(response, ENOENT, "No TC '%s' found", tc_name);

      task_attach(t, c);
    } else {
      int wid; /* TODO: worker_id_t */

      if (task_is_attached(t))
        return return_with_error(response, EBUSY,
                                 "Task %s:%hu is already "
                                 "attached to a TC",
                                 m_name, tid);

      wid = request.wid();
      if (wid >= MAX_WORKERS)
        return return_with_error(response, EINVAL,
                                 "'wid' must be between 0 and %d",
                                 MAX_WORKERS - 1);

      if (!is_worker_active(wid))
        return return_with_error(response, EINVAL, "Worker %d does not exist",
                                 wid);

      assign_default_tc(wid, t);
    }

    return Status::OK;
  }
  Status EnableTcpdump(ClientContext* context,
                       const EnableTcpdumpRequest& request,
                       EmptyResponse* response) {
    const char* m_name;
    const char* fifo;
    gate_idx_t ogate;

    Module* m;

    int ret;

    m_name = request.name().c_str();
    ogate = request.ogate();
    fifo = request.fifo().c_str();

    if (!request.name().length())
      return return_with_error(response, EINVAL, "Missing 'name' field");

    if ((m = find_module(m_name)) == NULL)
      return return_with_error(response, ENOENT, "No module '%s' found",
                               m_name);

    if (ogate >= m->ogates.curr_size)
      return return_with_error(response, EINVAL,
                               "Output gate '%hu' does not exist", ogate);

    ret = enable_tcpdump(fifo, m, ogate);

    if (ret < 0) {
      return return_with_error(response, -ret, "Enabling tcpdump %s:%d failed",
                               m_name, ogate);
    }

    return Status::OK;
  }
  Status DisableTcpdump(ClientContext* context,
                        const DisableTcpdumpRequest& request,
                        EmptyResponse* response) {
    const char* m_name;
    gate_idx_t ogate;

    Module* m;

    int ret;

    m_name = request.name().c_str();
    ogate = request.ogate();

    if (!request.name().length())
      return return_with_error(response, EINVAL, "Missing 'name' field");

    if ((m = find_module(m_name)) == NULL)
      return return_with_error(response, ENOENT, "No module '%s' found",
                               m_name);

    if (ogate >= m->ogates.curr_size)
      return return_with_error(response, EINVAL,
                               "Output gate '%hu' does not exist", ogate);

    ret = disable_tcpdump(m, ogate);

    if (ret < 0) {
      return return_with_error(response, -ret, "Disabling tcpdump %s:%d failed",
                               m_name, ogate);
    }
    return Status::OK;
  }

  Status KillBess(ClientContext* context, const Empty& request,
                  EmptyResponse* response) {
    log_notice("Halt requested by a client\n");
    exit(EXIT_SUCCESS);

    /* Never called */
    return Status::OK;
  }
};
