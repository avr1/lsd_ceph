// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.	See file COPYING.
 *
 */
#include "include/int_types.h"

#include <errno.h>

#include "common/deleter.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/TracepointProvider.h"
#include "include/Context.h"

#include "cls/rbd/cls_rbd_client.h"
#include "cls/rbd/cls_rbd_types.h"
#include "librbd/Features.h"
#include "librbd/ImageCtx.h"
#include "librbd/ImageState.h"
#include "librbd/internal.h"
#include "librbd/Operations.h"
#include "librbd/api/Config.h"
#include "librbd/api/DiffIterate.h"
#include "librbd/api/Group.h"
#include "librbd/api/Image.h"
#include "librbd/api/Migration.h"
#include "librbd/api/Mirror.h"
#include "librbd/api/Namespace.h"
#include "librbd/api/Pool.h"
#include "librbd/api/PoolMetadata.h"
#include "librbd/api/Snapshot.h"
#include "librbd/api/Trash.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/ImageRequestWQ.h"
#include "librbd/io/ReadResult.h"
#include <algorithm>
#include <string>
#include <vector>

#ifdef WITH_LTTNG
#define TRACEPOINT_DEFINE
#define TRACEPOINT_PROBE_DYNAMIC_LINKAGE
#include "tracing/librbd.h"
#undef TRACEPOINT_PROBE_DYNAMIC_LINKAGE
#undef TRACEPOINT_DEFINE
#else
#define tracepoint(...)
#endif

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd: "

using std::string;
using std::vector;

using ceph::bufferlist;
using librados::snap_t;
using librados::IoCtx;

namespace {

TracepointProvider::Traits tracepoint_traits("librbd_tp.so", "rbd_tracing");

struct UserBufferDeleter : public deleter::impl {
  CephContext* cct;
  librbd::io::AioCompletion* aio_completion;

  UserBufferDeleter(CephContext* cct, librbd::io::AioCompletion* aio_completion)
    : deleter::impl(deleter()), cct(cct), aio_completion(aio_completion) {
   aio_completion->block(cct);
  }

  ~UserBufferDeleter() override {
    aio_completion->unblock(cct);
  }
};

static auto create_write_raw(librbd::ImageCtx *ictx, const char *buf,
                             size_t len,
                             librbd::io::AioCompletion* aio_completion) {
  if (ictx->disable_zero_copy || aio_completion == nullptr) {
    // must copy the buffer if writeback/writearound cache is in-use (or using
    // non-AIO)
    return buffer::copy(buf, len);
  }

  // avoid copying memory for AIO operations, but possibly delay completions
  // until the last reference to the user's memory has been released
  return ceph::unique_leakable_ptr<ceph::buffer::raw>(
    buffer::claim_buffer(
      len, const_cast<char*>(buf),
      deleter(new UserBufferDeleter(ictx->cct, aio_completion))));
}

CephContext* get_cct(IoCtx &io_ctx) {
  return reinterpret_cast<CephContext*>(io_ctx.cct());
}

librbd::io::AioCompletion* get_aio_completion(librbd::RBD::AioCompletion *comp) {
  return reinterpret_cast<librbd::io::AioCompletion *>(comp->pc);
}

struct C_AioCompletion : public Context {
  CephContext *cct;
  librbd::io::aio_type_t aio_type;
  librbd::io::AioCompletion* aio_comp;

  C_AioCompletion(librbd::ImageCtx *ictx, librbd::io::aio_type_t aio_type,
                  librbd::io::AioCompletion* aio_comp)
    : cct(ictx->cct), aio_type(aio_type), aio_comp(aio_comp) {
    aio_comp->init_time(ictx, aio_type);
    aio_comp->get();
  }
  virtual ~C_AioCompletion() {
    aio_comp->put();
  }

  void finish(int r) override {
    ldout(cct, 20) << "C_AioComplete::finish: r=" << r << dendl;
    if (r < 0) {
      aio_comp->fail(r);
    } else {
      aio_comp->complete();
    }
  }
};

struct C_OpenComplete : public C_AioCompletion {
  librbd::ImageCtx *ictx;
  void **ictxp;
  C_OpenComplete(librbd::ImageCtx *ictx, librbd::io::AioCompletion* comp,
		 void **ictxp)
    : C_AioCompletion(ictx, librbd::io::AIO_TYPE_OPEN, comp),
      ictx(ictx), ictxp(ictxp) {
  }
  void finish(int r) override {
    ldout(ictx->cct, 20) << "C_OpenComplete::finish: r=" << r << dendl;
    if (r < 0) {
      *ictxp = nullptr;
    } else {
      *ictxp = ictx;
    }

    C_AioCompletion::finish(r);
  }
};

struct C_OpenAfterCloseComplete : public Context {
  librbd::ImageCtx *ictx;
  librbd::io::AioCompletion* comp;
  void **ictxp;
  C_OpenAfterCloseComplete(librbd::ImageCtx *ictx,
                           librbd::io::AioCompletion* comp,
			   void **ictxp)
    : ictx(ictx), comp(comp), ictxp(ictxp) {
  }
  void finish(int r) override {
    ldout(ictx->cct, 20) << "C_OpenAfterCloseComplete::finish: r=" << r
			 << dendl;
    delete reinterpret_cast<librbd::ImageCtx*>(*ictxp);
    *ictxp = nullptr;

    ictx->state->open(0, new C_OpenComplete(ictx, comp, ictxp));
  }
};

struct C_UpdateWatchCB : public librbd::UpdateWatchCtx {
  rbd_update_callback_t watch_cb;
  void *arg;
  uint64_t handle = 0;

  C_UpdateWatchCB(rbd_update_callback_t watch_cb, void *arg) :
    watch_cb(watch_cb), arg(arg) {
  }
  void handle_notify() override {
    watch_cb(arg);
  }
};

void group_image_status_cpp_to_c(const librbd::group_image_info_t &cpp_info,
				 rbd_group_image_info_t *c_info) {
  c_info->name = strdup(cpp_info.name.c_str());
  c_info->pool = cpp_info.pool;
  c_info->state = cpp_info.state;
}

void group_info_cpp_to_c(const librbd::group_info_t &cpp_info,
			 rbd_group_info_t *c_info) {
  c_info->name = strdup(cpp_info.name.c_str());
  c_info->pool = cpp_info.pool;
}

void group_snap_info_cpp_to_c(const librbd::group_snap_info_t &cpp_info,
			      rbd_group_snap_info_t *c_info) {
  c_info->name = strdup(cpp_info.name.c_str());
  c_info->state = cpp_info.state;
}

void mirror_image_info_cpp_to_c(const librbd::mirror_image_info_t &cpp_info,
				rbd_mirror_image_info_t *c_info) {
  c_info->global_id = strdup(cpp_info.global_id.c_str());
  c_info->state = cpp_info.state;
  c_info->primary = cpp_info.primary;
}

int get_local_mirror_image_site_status(
    const librbd::mirror_image_global_status_t& status,
    librbd::mirror_image_site_status_t* local_status) {
  auto it = std::find_if(status.site_statuses.begin(),
                         status.site_statuses.end(),
                         [](const librbd::mirror_image_site_status_t& s) {
      return (s.mirror_uuid ==
                cls::rbd::MirrorImageSiteStatus::LOCAL_MIRROR_UUID);
    });
  if (it == status.site_statuses.end()) {
    return -ENOENT;
  }

  *local_status = *it;
  return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

int mirror_image_global_status_cpp_to_c(
    const librbd::mirror_image_global_status_t &cpp_status,
    rbd_mirror_image_status_t *c_status) {
  c_status->name = strdup(cpp_status.name.c_str());
  mirror_image_info_cpp_to_c(cpp_status.info, &c_status->info);

  librbd::mirror_image_site_status_t local_status;
  int r = get_local_mirror_image_site_status(cpp_status, &local_status);
  if (r < 0) {
    return r;
  }

  c_status->state = local_status.state;
  c_status->description = strdup(local_status.description.c_str());
  c_status->last_update = local_status.last_update;
  c_status->up = local_status.up;
  return 0;
}

#pragma GCC diagnostic pop

void mirror_image_global_status_cpp_to_c(
    const librbd::mirror_image_global_status_t &cpp_status,
    rbd_mirror_image_global_status_t *c_status) {
  c_status->name = strdup(cpp_status.name.c_str());
  mirror_image_info_cpp_to_c(cpp_status.info, &c_status->info);

  c_status->site_statuses_count = cpp_status.site_statuses.size();
  c_status->site_statuses = (rbd_mirror_image_site_status_t*)calloc(
    cpp_status.site_statuses.size(), sizeof(rbd_mirror_image_site_status_t));

  auto idx = 0U;
  for (auto it = cpp_status.site_statuses.begin();
       it != cpp_status.site_statuses.end(); ++it) {
    auto& s_status = c_status->site_statuses[idx++];
    s_status.mirror_uuid = strdup(it->mirror_uuid.c_str());
    s_status.state = it->state;
    s_status.description = strdup(it->description.c_str());
    s_status.last_update = it->last_update;
    s_status.up = it->up;
  }
}

void trash_image_info_cpp_to_c(const librbd::trash_image_info_t &cpp_info,
                               rbd_trash_image_info_t *c_info) {
  c_info->id = strdup(cpp_info.id.c_str());
  c_info->name = strdup(cpp_info.name.c_str());
  c_info->source = cpp_info.source;
  c_info->deletion_time = cpp_info.deletion_time;
  c_info->deferment_end_time = cpp_info.deferment_end_time;
}

void config_option_cpp_to_c(const librbd::config_option_t &cpp_option,
                            rbd_config_option_t *c_option) {
  c_option->name = strdup(cpp_option.name.c_str());
  c_option->value = strdup(cpp_option.value.c_str());
  c_option->source = cpp_option.source;
}

void config_option_cleanup(rbd_config_option_t &option) {
    free(option.name);
    free(option.value);
}

struct C_MirrorImageGetInfo : public Context {
    rbd_mirror_image_info_t *mirror_image_info;
  Context *on_finish;

  librbd::mirror_image_info_t cpp_mirror_image_info;

  C_MirrorImageGetInfo(rbd_mirror_image_info_t *mirror_image_info,
                         Context *on_finish)
    : mirror_image_info(mirror_image_info), on_finish(on_finish) {
  }

  void finish(int r) override {
    if (r < 0) {
      on_finish->complete(r);
      return;
    }

    mirror_image_info_cpp_to_c(cpp_mirror_image_info, mirror_image_info);
    on_finish->complete(0);
  }
};

struct C_MirrorImageGetGlobalStatus : public Context {
  rbd_mirror_image_global_status_t *mirror_image_global_status;
  Context *on_finish;

  librbd::mirror_image_global_status_t cpp_mirror_image_global_status;

  C_MirrorImageGetGlobalStatus(
      rbd_mirror_image_global_status_t *mirror_image_global_status,
      Context *on_finish)
    : mirror_image_global_status(mirror_image_global_status),
      on_finish(on_finish) {
  }

  void finish(int r) override {
    if (r < 0) {
      on_finish->complete(r);
      return;
    }

    mirror_image_global_status_cpp_to_c(cpp_mirror_image_global_status,
                                        mirror_image_global_status);
    on_finish->complete(0);
  }
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

struct C_MirrorImageGetStatus : public Context {
  librbd::mirror_image_status_t *mirror_image_status_cpp = nullptr;
  rbd_mirror_image_status_t *mirror_image_status = nullptr;
  Context *on_finish;

  librbd::mirror_image_global_status_t cpp_mirror_image_global_status;

  C_MirrorImageGetStatus(rbd_mirror_image_status_t *mirror_image_status,
                         Context *on_finish)
    : mirror_image_status(mirror_image_status), on_finish(on_finish) {
  }
  C_MirrorImageGetStatus(librbd::mirror_image_status_t *mirror_image_status,
                         Context *on_finish)
    : mirror_image_status_cpp(mirror_image_status), on_finish(on_finish) {
  }


  void finish(int r) override {
    if (r < 0) {
      on_finish->complete(r);
      return;
    }

    if (mirror_image_status != nullptr) {
      r = mirror_image_global_status_cpp_to_c(cpp_mirror_image_global_status,
                                              mirror_image_status);
    } else if (mirror_image_status_cpp != nullptr) {
      librbd::mirror_image_site_status_t local_status;
      r = get_local_mirror_image_site_status(cpp_mirror_image_global_status,
                                             &local_status);
      if (r >= 0) {
        *mirror_image_status_cpp = {
          cpp_mirror_image_global_status.name,
          cpp_mirror_image_global_status.info,
          local_status.state, local_status.description,
          local_status.last_update, local_status.up};
      }
    }
    on_finish->complete(r);
  }
};

#pragma GCC diagnostic pop

} // anonymous namespace

namespace librbd {
  ProgressContext::~ProgressContext()
  {
  }

  class CProgressContext : public ProgressContext
  {
  public:
    CProgressContext(librbd_progress_fn_t fn, void *data)
      : m_fn(fn), m_data(data)
    {
    }
    int update_progress(uint64_t offset, uint64_t src_size) override
    {
      return m_fn(offset, src_size, m_data);
    }
  private:
    librbd_progress_fn_t m_fn;
    void *m_data;
  };

  /*
   * Pool stats
   */
  PoolStats::PoolStats() {
    rbd_pool_stats_create(&pool_stats);
  }

  PoolStats::~PoolStats() {
    rbd_pool_stats_destroy(pool_stats);
  }

  int PoolStats::add(rbd_pool_stat_option_t option, uint64_t* opt_val) {
    return rbd_pool_stats_option_add_uint64(pool_stats, option, opt_val);
  }

  /*
   *  RBD
   */
  RBD::RBD()
  {
  }

  RBD::~RBD()
  {
  }

  void RBD::version(int *major, int *minor, int *extra)
  {
    rbd_version(major, minor, extra);
  }

  int RBD::open(IoCtx& io_ctx, Image& image, const char *name)
  {
    return open(io_ctx, image, name, NULL);
  }

  int RBD::open_by_id(IoCtx& io_ctx, Image& image, const char *id)
  {
    return open_by_id(io_ctx, image, id, nullptr);
  }

  int RBD::open(IoCtx& io_ctx, Image& image, const char *name,
		const char *snap_name)
  {
    ImageCtx *ictx = new ImageCtx(name, "", snap_name, io_ctx, false);
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, open_image_enter, ictx, ictx->name.c_str(), ictx->id.c_str(), ictx->snap_name.c_str(), ictx->read_only);

    if (image.ctx != NULL) {
      reinterpret_cast<ImageCtx*>(image.ctx)->state->close();
      image.ctx = NULL;
    }

    int r = ictx->state->open(0);
    if (r < 0) {
      tracepoint(librbd, open_image_exit, r);
      return r;
    }

    image.ctx = (image_ctx_t) ictx;
    tracepoint(librbd, open_image_exit, 0);
    return 0;
  }

  int RBD::open_by_id(IoCtx& io_ctx, Image& image, const char *id,
		      const char *snap_name)
  {
    ImageCtx *ictx = new ImageCtx("", id, snap_name, io_ctx, false);
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, open_image_by_id_enter, ictx, ictx->id.c_str(),
               ictx->snap_name.c_str(), ictx->read_only);

    if (image.ctx != nullptr) {
      reinterpret_cast<ImageCtx*>(image.ctx)->state->close();
      image.ctx = nullptr;
    }

    int r = ictx->state->open(0);
    if (r < 0) {
      tracepoint(librbd, open_image_by_id_exit, r);
      return r;
    }

    image.ctx = (image_ctx_t) ictx;
    tracepoint(librbd, open_image_by_id_exit, 0);
    return 0;
  }

  int RBD::aio_open(IoCtx& io_ctx, Image& image, const char *name,
		    const char *snap_name, RBD::AioCompletion *c)
  {
    ImageCtx *ictx = new ImageCtx(name, "", snap_name, io_ctx, false);
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, aio_open_image_enter, ictx, ictx->name.c_str(), ictx->id.c_str(), ictx->snap_name.c_str(), ictx->read_only, c->pc);

    if (image.ctx != NULL) {
      reinterpret_cast<ImageCtx*>(image.ctx)->state->close(
	new C_OpenAfterCloseComplete(ictx, get_aio_completion(c), &image.ctx));
    } else {
      ictx->state->open(0, new C_OpenComplete(ictx, get_aio_completion(c),
                                              &image.ctx));
    }
    tracepoint(librbd, aio_open_image_exit, 0);
    return 0;
  }

  int RBD::aio_open_by_id(IoCtx& io_ctx, Image& image, const char *id,
		          const char *snap_name, RBD::AioCompletion *c)
  {
    ImageCtx *ictx = new ImageCtx("", id, snap_name, io_ctx, false);
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, aio_open_image_by_id_enter, ictx, ictx->id.c_str(),
               ictx->snap_name.c_str(), ictx->read_only, c->pc);

    if (image.ctx != nullptr) {
      reinterpret_cast<ImageCtx*>(image.ctx)->state->close(
	new C_OpenAfterCloseComplete(ictx, get_aio_completion(c), &image.ctx));
    } else {
      ictx->state->open(0, new C_OpenComplete(ictx, get_aio_completion(c),
                                              &image.ctx));
    }
    tracepoint(librbd, aio_open_image_by_id_exit, 0);
    return 0;
  }

  int RBD::open_read_only(IoCtx& io_ctx, Image& image, const char *name,
			  const char *snap_name)
  {
    ImageCtx *ictx = new ImageCtx(name, "", snap_name, io_ctx, true);
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, open_image_enter, ictx, ictx->name.c_str(), ictx->id.c_str(), ictx->snap_name.c_str(), ictx->read_only);

    if (image.ctx != NULL) {
      reinterpret_cast<ImageCtx*>(image.ctx)->state->close();
      image.ctx = NULL;
    }

    int r = ictx->state->open(0);
    if (r < 0) {
      tracepoint(librbd, open_image_exit, r);
      return r;
    }

    image.ctx = (image_ctx_t) ictx;
    tracepoint(librbd, open_image_exit, 0);
    return 0;
  }

  int RBD::open_by_id_read_only(IoCtx& io_ctx, Image& image, const char *id,
			        const char *snap_name)
  {
    ImageCtx *ictx = new ImageCtx("", id, snap_name, io_ctx, true);
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, open_image_by_id_enter, ictx, ictx->id.c_str(),
               ictx->snap_name.c_str(), ictx->read_only);

    if (image.ctx != nullptr) {
      reinterpret_cast<ImageCtx*>(image.ctx)->state->close();
      image.ctx = nullptr;
    }

    int r = ictx->state->open(0);
    if (r < 0) {
      tracepoint(librbd, open_image_by_id_exit, r);
      return r;
    }

    image.ctx = (image_ctx_t) ictx;
    tracepoint(librbd, open_image_by_id_exit, 0);
    return 0;
  }

  int RBD::aio_open_read_only(IoCtx& io_ctx, Image& image, const char *name,
			      const char *snap_name, RBD::AioCompletion *c)
  {
    ImageCtx *ictx = new ImageCtx(name, "", snap_name, io_ctx, true);
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, aio_open_image_enter, ictx, ictx->name.c_str(), ictx->id.c_str(), ictx->snap_name.c_str(), ictx->read_only, c->pc);

    if (image.ctx != NULL) {
      reinterpret_cast<ImageCtx*>(image.ctx)->state->close(
	new C_OpenAfterCloseComplete(ictx, get_aio_completion(c), &image.ctx));
    } else {
      ictx->state->open(0, new C_OpenComplete(ictx, get_aio_completion(c),
                                              &image.ctx));
    }
    tracepoint(librbd, aio_open_image_exit, 0);
    return 0;
  }

  int RBD::aio_open_by_id_read_only(IoCtx& io_ctx, Image& image, const char *id,
	                            const char *snap_name, RBD::AioCompletion *c)
  {
    ImageCtx *ictx = new ImageCtx("", id, snap_name, io_ctx, true);
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, aio_open_image_by_id_enter, ictx, ictx->id.c_str(),
               ictx->snap_name.c_str(), ictx->read_only, c->pc);

    if (image.ctx != nullptr) {
      reinterpret_cast<ImageCtx*>(image.ctx)->state->close(
	new C_OpenAfterCloseComplete(ictx, get_aio_completion(c), &image.ctx));
    } else {
      ictx->state->open(0, new C_OpenComplete(ictx, get_aio_completion(c),
                                              &image.ctx));
    }
    tracepoint(librbd, aio_open_image_by_id_exit, 0);
    return 0;
  }

  int RBD::features_to_string(uint64_t features, std::string *str_features)
  {
    std::stringstream err;
    *str_features = librbd::rbd_features_to_string(features, &err);
    if (!err.str().empty()) {
      return -EINVAL;
    }

    return 0;
  }

  int RBD::features_from_string(const std::string str_features, uint64_t *features)
  {
    std::stringstream err;
    *features = librbd::rbd_features_from_string(str_features, &err);
    if (!err.str().empty()) {
      return -EINVAL;
    }

    return 0;
  }

  int RBD::create(IoCtx& io_ctx, const char *name, uint64_t size, int *order)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, create_enter, io_ctx.get_pool_name().c_str(), io_ctx.get_id(), name, size, *order);
    int r = librbd::create(io_ctx, name, size, order);
    tracepoint(librbd, create_exit, r, *order);
    return r;
  }

  int RBD::create2(IoCtx& io_ctx, const char *name, uint64_t size,
		   uint64_t features, int *order)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, create2_enter, io_ctx.get_pool_name().c_str(), io_ctx.get_id(), name, size, features, *order);
    int r = librbd::create(io_ctx, name, size, false, features, order, 0, 0);
    tracepoint(librbd, create2_exit, r, *order);
    return r;
  }

  int RBD::create3(IoCtx& io_ctx, const char *name, uint64_t size,
		   uint64_t features, int *order, uint64_t stripe_unit,
		   uint64_t stripe_count)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, create3_enter, io_ctx.get_pool_name().c_str(), io_ctx.get_id(), name, size, features, *order, stripe_unit, stripe_count);
    int r = librbd::create(io_ctx, name, size, false, features, order,
			  stripe_unit, stripe_count);
    tracepoint(librbd, create3_exit, r, *order);
    return r;
  }

  int RBD::create4(IoCtx& io_ctx, const char *name, uint64_t size,
		   ImageOptions& opts)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, create4_enter, io_ctx.get_pool_name().c_str(), io_ctx.get_id(), name, size, opts.opts);
    int r = librbd::create(io_ctx, name, "", size, opts, "", "", false);
    tracepoint(librbd, create4_exit, r);
    return r;
  }

  int RBD::clone(IoCtx& p_ioctx, const char *p_name, const char *p_snap_name,
		 IoCtx& c_ioctx, const char *c_name, uint64_t features,
		 int *c_order)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(p_ioctx));
    tracepoint(librbd, clone_enter, p_ioctx.get_pool_name().c_str(), p_ioctx.get_id(), p_name, p_snap_name, c_ioctx.get_pool_name().c_str(), c_ioctx.get_id(), c_name, features);
    int r = librbd::clone(p_ioctx, p_name, p_snap_name, c_ioctx, c_name,
			 features, c_order, 0, 0);
    tracepoint(librbd, clone_exit, r, *c_order);
    return r;
  }

  int RBD::clone2(IoCtx& p_ioctx, const char *p_name, const char *p_snap_name,
		  IoCtx& c_ioctx, const char *c_name, uint64_t features,
		  int *c_order, uint64_t stripe_unit, int stripe_count)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(p_ioctx));
    tracepoint(librbd, clone2_enter, p_ioctx.get_pool_name().c_str(), p_ioctx.get_id(), p_name, p_snap_name, c_ioctx.get_pool_name().c_str(), c_ioctx.get_id(), c_name, features, stripe_unit, stripe_count);
    int r = librbd::clone(p_ioctx, p_name, p_snap_name, c_ioctx, c_name,
			 features, c_order, stripe_unit, stripe_count);
    tracepoint(librbd, clone2_exit, r, *c_order);
    return r;
  }

  int RBD::clone3(IoCtx& p_ioctx, const char *p_name, const char *p_snap_name,
		  IoCtx& c_ioctx, const char *c_name, ImageOptions& c_opts)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(p_ioctx));
    tracepoint(librbd, clone3_enter, p_ioctx.get_pool_name().c_str(), p_ioctx.get_id(), p_name, p_snap_name, c_ioctx.get_pool_name().c_str(), c_ioctx.get_id(), c_name, c_opts.opts);
    int r = librbd::clone(p_ioctx, nullptr, p_name, p_snap_name, c_ioctx,
                          nullptr, c_name, c_opts, "", "");
    tracepoint(librbd, clone3_exit, r);
    return r;
  }

  int RBD::remove(IoCtx& io_ctx, const char *name)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, remove_enter, io_ctx.get_pool_name().c_str(), io_ctx.get_id(), name);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::api::Image<>::remove(io_ctx, name, prog_ctx);
    tracepoint(librbd, remove_exit, r);
    return r;
  }

  int RBD::remove_with_progress(IoCtx& io_ctx, const char *name,
				ProgressContext& pctx)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, remove_enter, io_ctx.get_pool_name().c_str(), io_ctx.get_id(), name);
    int r = librbd::api::Image<>::remove(io_ctx, name, pctx);
    tracepoint(librbd, remove_exit, r);
    return r;
  }

  int RBD::trash_move(IoCtx &io_ctx, const char *name, uint64_t delay) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, trash_move_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), name);
    int r = librbd::api::Trash<>::move(io_ctx, RBD_TRASH_IMAGE_SOURCE_USER,
                                       name, delay);
    tracepoint(librbd, trash_move_exit, r);
    return r;
  }

  int RBD::trash_get(IoCtx &io_ctx, const char *id, trash_image_info_t *info) {
    return librbd::api::Trash<>::get(io_ctx, id, info);
  }

  int RBD::trash_list(IoCtx &io_ctx, vector<trash_image_info_t> &entries) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, trash_list_enter,
               io_ctx.get_pool_name().c_str(), io_ctx.get_id());
    int r = librbd::api::Trash<>::list(io_ctx, entries, true);
#ifdef WITH_LTTNG
    if (r >= 0) {
      for (const auto& entry : entries) {
	tracepoint(librbd, trash_list_entry, entry.id.c_str());
      }
    }
#endif
    tracepoint(librbd, trash_list_exit, r, r);
    return r;
  }

  int RBD::trash_remove(IoCtx &io_ctx, const char *image_id, bool force) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, trash_remove_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), image_id, force);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::api::Trash<>::remove(io_ctx, image_id, force, prog_ctx);
    tracepoint(librbd, trash_remove_exit, r);
    return r;
  }

  int RBD::trash_remove_with_progress(IoCtx &io_ctx, const char *image_id,
                                      bool force, ProgressContext &pctx) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, trash_remove_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), image_id, force);
    int r = librbd::api::Trash<>::remove(io_ctx, image_id, force, pctx);
    tracepoint(librbd, trash_remove_exit, r);
    return r;
  }

  int RBD::trash_restore(IoCtx &io_ctx, const char *id, const char *name) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, trash_undelete_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), id, name);
    int r = librbd::api::Trash<>::restore(
      io_ctx, librbd::api::Trash<>::RESTORE_SOURCE_WHITELIST, id, name);
    tracepoint(librbd, trash_undelete_exit, r);
    return r;
  }

  int RBD::trash_purge(IoCtx &io_ctx, time_t expire_ts, float threshold) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, trash_purge_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), expire_ts, threshold);
    NoOpProgressContext nop_pctx;
    int r = librbd::api::Trash<>::purge(io_ctx, expire_ts, threshold, nop_pctx);
    tracepoint(librbd, trash_purge_exit, r);
    return r;
  }

  int RBD::trash_purge_with_progress(IoCtx &io_ctx, time_t expire_ts,
                                     float threshold, ProgressContext &pctx) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, trash_purge_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), expire_ts, threshold);
    int r = librbd::api::Trash<>::purge(io_ctx, expire_ts, threshold, pctx);
    tracepoint(librbd, trash_purge_exit, r);
    return r;
  }

  int RBD::namespace_create(IoCtx& io_ctx, const char *namespace_name) {
    return librbd::api::Namespace<>::create(io_ctx, namespace_name);
  }

  int RBD::namespace_remove(IoCtx& io_ctx, const char *namespace_name) {
    return librbd::api::Namespace<>::remove(io_ctx, namespace_name);
  }

  int RBD::namespace_list(IoCtx& io_ctx,
                          std::vector<std::string>* namespace_names) {
    return librbd::api::Namespace<>::list(io_ctx, namespace_names);
  }

  int RBD::namespace_exists(IoCtx& io_ctx, const char *namespace_name,
                            bool *exists) {
    return librbd::api::Namespace<>::exists(io_ctx, namespace_name, exists);
  }

  int RBD::pool_init(IoCtx& io_ctx, bool force) {
    return librbd::api::Pool<>::init(io_ctx, force);
  }

  int RBD::pool_stats_get(IoCtx& io_ctx, PoolStats* stats) {
    auto pool_stat_options =
      reinterpret_cast<librbd::api::Pool<>::StatOptions*>(stats->pool_stats);
    return librbd::api::Pool<>::get_stats(io_ctx, pool_stat_options);
  }

  int RBD::list(IoCtx& io_ctx, vector<string>& names)
  {
    std::vector<image_spec_t> image_specs;
    int r = list2(io_ctx, &image_specs);
    if (r < 0) {
      return r;
    }

    names.clear();
    for (auto& it : image_specs) {
      names.push_back(it.name);
    }
    return 0;
  }

  int RBD::list2(IoCtx& io_ctx, std::vector<image_spec_t> *images)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, list_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id());

    int r = librbd::api::Image<>::list_images(io_ctx, images);
#ifdef WITH_LTTNG
    if (r >= 0) {
      for (auto& it : *images) {
        tracepoint(librbd, list_entry, it.name.c_str());
      }
    }
#endif
    tracepoint(librbd, list_exit, r, r);
    return r;
  }

  int RBD::rename(IoCtx& src_io_ctx, const char *srcname, const char *destname)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(src_io_ctx));
    tracepoint(librbd, rename_enter, src_io_ctx.get_pool_name().c_str(), src_io_ctx.get_id(), srcname, destname);
    int r = librbd::rename(src_io_ctx, srcname, destname);
    tracepoint(librbd, rename_exit, r);
    return r;
  }

  int RBD::migration_prepare(IoCtx& io_ctx, const char *image_name,
                             IoCtx& dest_io_ctx, const char *dest_image_name,
                             ImageOptions& opts)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, migration_prepare_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), image_name, dest_io_ctx.get_pool_name().c_str(),
               dest_io_ctx.get_id(), dest_image_name, opts.opts);
    int r = librbd::api::Migration<>::prepare(io_ctx, image_name, dest_io_ctx,
                                              dest_image_name, opts);
    tracepoint(librbd, migration_prepare_exit, r);
    return r;
  }

  int RBD::migration_execute(IoCtx& io_ctx, const char *image_name)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, migration_execute_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), image_name);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::api::Migration<>::execute(io_ctx, image_name, prog_ctx);
    tracepoint(librbd, migration_execute_exit, r);
    return r;
  }

  int RBD::migration_execute_with_progress(IoCtx& io_ctx,
                                           const char *image_name,
                                           librbd::ProgressContext &prog_ctx)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, migration_execute_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), image_name);
    int r = librbd::api::Migration<>::execute(io_ctx, image_name, prog_ctx);
    tracepoint(librbd, migration_execute_exit, r);
    return r;
  }

  int RBD::migration_abort(IoCtx& io_ctx, const char *image_name)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, migration_abort_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), image_name);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::api::Migration<>::abort(io_ctx, image_name, prog_ctx);
    tracepoint(librbd, migration_abort_exit, r);
    return r;
  }

  int RBD::migration_abort_with_progress(IoCtx& io_ctx, const char *image_name,
                                         librbd::ProgressContext &prog_ctx)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, migration_abort_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), image_name);
    int r = librbd::api::Migration<>::abort(io_ctx, image_name, prog_ctx);
    tracepoint(librbd, migration_abort_exit, r);
    return r;
  }

  int RBD::migration_commit(IoCtx& io_ctx, const char *image_name)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, migration_commit_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), image_name);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::api::Migration<>::commit(io_ctx, image_name, prog_ctx);
    tracepoint(librbd, migration_commit_exit, r);
    return r;
  }

  int RBD::migration_commit_with_progress(IoCtx& io_ctx, const char *image_name,
                                          librbd::ProgressContext &prog_ctx)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, migration_commit_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), image_name);
    int r = librbd::api::Migration<>::commit(io_ctx, image_name, prog_ctx);
    tracepoint(librbd, migration_commit_exit, r);
    return r;
  }

  int RBD::migration_status(IoCtx& io_ctx, const char *image_name,
                            image_migration_status_t *status,
                            size_t status_size)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, migration_status_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), image_name);

    if (status_size != sizeof(image_migration_status_t)) {
      tracepoint(librbd, migration_status_exit, -ERANGE);
      return -ERANGE;
    }

    int r = librbd::api::Migration<>::status(io_ctx, image_name, status);
    tracepoint(librbd, migration_status_exit, r);
    return r;
  }

  int RBD::mirror_mode_get(IoCtx& io_ctx, rbd_mirror_mode_t *mirror_mode) {
    return librbd::api::Mirror<>::mode_get(io_ctx, mirror_mode);
  }

  int RBD::mirror_mode_set(IoCtx& io_ctx, rbd_mirror_mode_t mirror_mode) {
    return librbd::api::Mirror<>::mode_set(io_ctx, mirror_mode);
  }

  int RBD::mirror_uuid_get(IoCtx& io_ctx, std::string* mirror_uuid) {
    return librbd::api::Mirror<>::uuid_get(io_ctx, mirror_uuid);
  }

  int RBD::mirror_site_name_get(librados::Rados& rados,
                                std::string* site_name) {
    return librbd::api::Mirror<>::site_name_get(rados, site_name);
  }

  int RBD::mirror_site_name_set(librados::Rados& rados,
                                const std::string& site_name) {
    return librbd::api::Mirror<>::site_name_set(rados, site_name);
  }

  int RBD::mirror_peer_bootstrap_create(IoCtx& io_ctx, std::string* token) {
    return librbd::api::Mirror<>::peer_bootstrap_create(io_ctx, token);
  }

  int RBD::mirror_peer_bootstrap_import(IoCtx& io_ctx,
                                        rbd_mirror_peer_direction_t direction,
                                        const std::string& token) {
    return librbd::api::Mirror<>::peer_bootstrap_import(io_ctx, direction,
                                                        token);
  }

  int RBD::mirror_peer_site_add(IoCtx& io_ctx, std::string *uuid,
                                mirror_peer_direction_t direction,
                                const std::string &site_name,
                                const std::string &client_name) {
    return librbd::api::Mirror<>::peer_site_add(
      io_ctx, uuid, direction, site_name, client_name);
  }

  int RBD::mirror_peer_site_remove(IoCtx& io_ctx, const std::string &uuid) {
    return librbd::api::Mirror<>::peer_site_remove(io_ctx, uuid);
  }

  int RBD::mirror_peer_site_list(
      IoCtx& io_ctx, std::vector<mirror_peer_site_t> *peer_sites) {
    return librbd::api::Mirror<>::peer_site_list(io_ctx, peer_sites);
  }

  int RBD::mirror_peer_site_set_client_name(
      IoCtx& io_ctx, const std::string &uuid, const std::string &client_name) {
    return librbd::api::Mirror<>::peer_site_set_client(io_ctx, uuid,
                                                       client_name);
  }

  int RBD::mirror_peer_site_set_name(IoCtx& io_ctx, const std::string &uuid,
                                     const std::string &site_name) {
    return librbd::api::Mirror<>::peer_site_set_name(io_ctx, uuid,
                                                     site_name);
  }

  int RBD::mirror_peer_site_set_direction(IoCtx& io_ctx,
                                          const std::string& uuid,
                                          mirror_peer_direction_t direction) {
    return librbd::api::Mirror<>::peer_site_set_direction(io_ctx, uuid,
                                                          direction);
  }

  int RBD::mirror_peer_site_get_attributes(
      IoCtx& io_ctx, const std::string &uuid,
      std::map<std::string, std::string> *key_vals) {
    return librbd::api::Mirror<>::peer_site_get_attributes(io_ctx, uuid,
                                                           key_vals);
  }

  int RBD::mirror_peer_site_set_attributes(
      IoCtx& io_ctx, const std::string &uuid,
      const std::map<std::string, std::string>& key_vals) {
    return librbd::api::Mirror<>::peer_site_set_attributes(io_ctx, uuid,
                                                           key_vals);
  }

  int RBD::mirror_image_global_status_list(
      IoCtx& io_ctx, const std::string &start_id, size_t max,
      std::map<std::string, mirror_image_global_status_t> *global_statuses) {
    return librbd::api::Mirror<>::image_global_status_list(
      io_ctx, start_id, max, global_statuses);
  }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

  int RBD::mirror_peer_add(IoCtx& io_ctx, std::string *uuid,
                           const std::string &cluster_name,
                           const std::string &client_name) {
    return librbd::api::Mirror<>::peer_site_add(
      io_ctx, uuid, RBD_MIRROR_PEER_DIRECTION_RX_TX, cluster_name, client_name);
  }

  int RBD::mirror_peer_remove(IoCtx& io_ctx, const std::string &uuid) {
    return librbd::api::Mirror<>::peer_site_remove(io_ctx, uuid);
  }

  int RBD::mirror_peer_list(IoCtx& io_ctx, std::vector<mirror_peer_t> *peers) {
    std::vector<mirror_peer_site_t> peer_sites;
    int r = librbd::api::Mirror<>::peer_site_list(io_ctx, &peer_sites);
    if (r < 0) {
      return r;
    }

    peers->clear();
    peers->reserve(peer_sites.size());
    for (auto& peer_site : peer_sites) {
      peers->push_back({peer_site.uuid, peer_site.site_name,
                        peer_site.client_name});
    }
    return 0;
  }

  int RBD::mirror_peer_set_client(IoCtx& io_ctx, const std::string &uuid,
                                  const std::string &client_name) {
    return librbd::api::Mirror<>::peer_site_set_client(io_ctx, uuid,
                                                       client_name);
  }

  int RBD::mirror_peer_set_cluster(IoCtx& io_ctx, const std::string &uuid,
                                   const std::string &cluster_name) {
    return librbd::api::Mirror<>::peer_site_set_name(io_ctx, uuid,
                                                     cluster_name);
  }

  int RBD::mirror_peer_get_attributes(
      IoCtx& io_ctx, const std::string &uuid,
      std::map<std::string, std::string> *key_vals) {
    return librbd::api::Mirror<>::peer_site_get_attributes(io_ctx, uuid,
                                                           key_vals);
  }

  int RBD::mirror_peer_set_attributes(
      IoCtx& io_ctx, const std::string &uuid,
      const std::map<std::string, std::string>& key_vals) {
    return librbd::api::Mirror<>::peer_site_set_attributes(io_ctx, uuid,
                                                           key_vals);
  }

  int RBD::mirror_image_status_list(IoCtx& io_ctx, const std::string &start_id,
      size_t max, std::map<std::string, mirror_image_status_t> *images) {
    std::map<std::string, mirror_image_global_status_t> global_statuses;

    int r = librbd::api::Mirror<>::image_global_status_list(
      io_ctx, start_id, max, &global_statuses);
    if (r < 0) {
      return r;
    }

    images->clear();
    for (auto &[id, global_status] : global_statuses) {
      if (global_status.site_statuses.empty() ||
          global_status.site_statuses[0].mirror_uuid !=
            cls::rbd::MirrorImageSiteStatus::LOCAL_MIRROR_UUID) {
        continue;
      }

      auto& site_status = global_status.site_statuses[0];
      (*images)[id] = mirror_image_status_t{
        global_status.name, global_status.info, site_status.state,
        site_status.description, site_status.last_update, site_status.up};
    }

    return 0;
  }

#pragma GCC diagnostic pop

  int RBD::mirror_image_status_summary(IoCtx& io_ctx,
      std::map<mirror_image_status_state_t, int> *states) {
    return librbd::api::Mirror<>::image_status_summary(io_ctx, states);
  }

  int RBD::mirror_image_instance_id_list(IoCtx& io_ctx,
      const std::string &start_id, size_t max,
      std::map<std::string, std::string> *instance_ids) {
    return librbd::api::Mirror<>::image_instance_id_list(io_ctx, start_id, max,
                                                         instance_ids);
  }

  int RBD::mirror_image_info_list(
      IoCtx& io_ctx, mirror_image_mode_t *mode_filter,
      const std::string &start_id, size_t max,
      std::map<std::string, std::pair<mirror_image_mode_t,
                                      mirror_image_info_t>> *entries) {
    return librbd::api::Mirror<>::image_info_list(io_ctx, mode_filter, start_id,
                                                  max, entries);
  }

  int RBD::group_create(IoCtx& io_ctx, const char *group_name)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, group_create_enter, io_ctx.get_pool_name().c_str(),
	       io_ctx.get_id(), group_name);
    int r = librbd::api::Group<>::create(io_ctx, group_name);
    tracepoint(librbd, group_create_exit, r);
    return r;
  }

  int RBD::group_remove(IoCtx& io_ctx, const char *group_name)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, group_remove_enter, io_ctx.get_pool_name().c_str(),
	       io_ctx.get_id(), group_name);
    int r = librbd::api::Group<>::remove(io_ctx, group_name);
    tracepoint(librbd, group_remove_exit, r);
    return r;
  }

  int RBD::group_list(IoCtx& io_ctx, vector<string> *names)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, group_list_enter, io_ctx.get_pool_name().c_str(),
	       io_ctx.get_id());

    int r = librbd::api::Group<>::list(io_ctx, names);
    if (r >= 0) {
      for (auto itr : *names) {
	tracepoint(librbd, group_list_entry, itr.c_str());
      }
    }
    tracepoint(librbd, group_list_exit, r);
    return r;
  }

  int RBD::group_rename(IoCtx& io_ctx, const char *src_name,
                        const char *dest_name)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
    tracepoint(librbd, group_rename_enter, io_ctx.get_pool_name().c_str(),
               io_ctx.get_id(), src_name, dest_name);
    int r = librbd::api::Group<>::rename(io_ctx, src_name, dest_name);
    tracepoint(librbd, group_rename_exit, r);
    return r;
  }

  int RBD::group_image_add(IoCtx& group_ioctx, const char *group_name,
                           IoCtx& image_ioctx, const char *image_name)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(group_ioctx));
    tracepoint(librbd, group_image_add_enter,
               group_ioctx.get_pool_name().c_str(),
               group_ioctx.get_id(), group_name,
               image_ioctx.get_pool_name().c_str(),
               image_ioctx.get_id(), image_name);
    int r = librbd::api::Group<>::image_add(group_ioctx, group_name,
                                            image_ioctx, image_name);
    tracepoint(librbd, group_image_add_exit, r);
    return r;
  }

  int RBD::group_image_remove(IoCtx& group_ioctx, const char *group_name,
                              IoCtx& image_ioctx, const char *image_name)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(group_ioctx));
    tracepoint(librbd, group_image_remove_enter,
               group_ioctx.get_pool_name().c_str(),
               group_ioctx.get_id(), group_name,
               image_ioctx.get_pool_name().c_str(),
               image_ioctx.get_id(), image_name);
    int r = librbd::api::Group<>::image_remove(group_ioctx, group_name,
                                               image_ioctx, image_name);
    tracepoint(librbd, group_image_remove_exit, r);
    return r;
  }

  int RBD::group_image_remove_by_id(IoCtx& group_ioctx, const char *group_name,
                                    IoCtx& image_ioctx, const char *image_id)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(group_ioctx));
    tracepoint(librbd, group_image_remove_by_id_enter,
               group_ioctx.get_pool_name().c_str(),
               group_ioctx.get_id(), group_name,
               image_ioctx.get_pool_name().c_str(),
               image_ioctx.get_id(), image_id);
    int r = librbd::api::Group<>::image_remove_by_id(group_ioctx, group_name,
                                                     image_ioctx, image_id);
    tracepoint(librbd, group_image_remove_by_id_exit, r);
    return r;
  }

  int RBD::group_image_list(IoCtx& group_ioctx, const char *group_name,
                            std::vector<group_image_info_t> *images,
                            size_t group_image_info_size)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(group_ioctx));
    tracepoint(librbd, group_image_list_enter,
               group_ioctx.get_pool_name().c_str(),
	       group_ioctx.get_id(), group_name);

    if (group_image_info_size != sizeof(group_image_info_t)) {
      tracepoint(librbd, group_image_list_exit, -ERANGE);
      return -ERANGE;
    }

    int r = librbd::api::Group<>::image_list(group_ioctx, group_name, images);
    tracepoint(librbd, group_image_list_exit, r);
    return r;
  }

  int RBD::group_snap_create(IoCtx& group_ioctx, const char *group_name,
			     const char *snap_name) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(group_ioctx));
    tracepoint(librbd, group_snap_create_enter,
               group_ioctx.get_pool_name().c_str(),
	       group_ioctx.get_id(), group_name, snap_name);
    int r = librbd::api::Group<>::snap_create(group_ioctx, group_name,
                                              snap_name);
    tracepoint(librbd, group_snap_create_exit, r);
    return r;
  }

  int RBD::group_snap_remove(IoCtx& group_ioctx, const char *group_name,
			     const char *snap_name) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(group_ioctx));
    tracepoint(librbd, group_snap_remove_enter,
               group_ioctx.get_pool_name().c_str(),
	       group_ioctx.get_id(), group_name, snap_name);
    int r = librbd::api::Group<>::snap_remove(group_ioctx, group_name,
                                              snap_name);
    tracepoint(librbd, group_snap_remove_exit, r);
    return r;
  }

  int RBD::group_snap_list(IoCtx& group_ioctx, const char *group_name,
			   std::vector<group_snap_info_t> *snaps,
                           size_t group_snap_info_size)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(group_ioctx));
    tracepoint(librbd, group_snap_list_enter,
               group_ioctx.get_pool_name().c_str(),
	       group_ioctx.get_id(), group_name);

    if (group_snap_info_size != sizeof(group_snap_info_t)) {
      tracepoint(librbd, group_snap_list_exit, -ERANGE);
      return -ERANGE;
    }

    int r = librbd::api::Group<>::snap_list(group_ioctx, group_name, snaps);
    tracepoint(librbd, group_snap_list_exit, r);
    return r;
  }

  int RBD::group_snap_rename(IoCtx& group_ioctx, const char *group_name,
                             const char *old_snap_name,
                             const char *new_snap_name)
  {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(group_ioctx));
    tracepoint(librbd, group_snap_rename_enter,
               group_ioctx.get_pool_name().c_str(), group_ioctx.get_id(),
               group_name, old_snap_name, new_snap_name);
    int r = librbd::api::Group<>::snap_rename(group_ioctx, group_name,
                                              old_snap_name, new_snap_name);
    tracepoint(librbd, group_snap_list_exit, r);
    return r;
  }

  int RBD::group_snap_rollback(IoCtx& group_ioctx, const char *group_name,
                               const char *snap_name) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(group_ioctx));
    tracepoint(librbd, group_snap_rollback_enter,
               group_ioctx.get_pool_name().c_str(),
               group_ioctx.get_id(), group_name, snap_name);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::api::Group<>::snap_rollback(group_ioctx, group_name,
                                                snap_name, prog_ctx);
    tracepoint(librbd, group_snap_rollback_exit, r);
    return r;
  }

  int RBD::group_snap_rollback_with_progress(IoCtx& group_ioctx,
                                             const char *group_name,
                                             const char *snap_name,
                                             ProgressContext& prog_ctx) {
    TracepointProvider::initialize<tracepoint_traits>(get_cct(group_ioctx));
    tracepoint(librbd, group_snap_rollback_enter,
               group_ioctx.get_pool_name().c_str(),
               group_ioctx.get_id(), group_name, snap_name);
    int r = librbd::api::Group<>::snap_rollback(group_ioctx, group_name,
                                                snap_name, prog_ctx);
    tracepoint(librbd, group_snap_rollback_exit, r);
    return r;
  }

  int RBD::pool_metadata_get(IoCtx& ioctx, const std::string &key,
                             std::string *value)
  {
    int r = librbd::api::PoolMetadata<>::get(ioctx, key, value);
    return r;
  }

  int RBD::pool_metadata_set(IoCtx& ioctx, const std::string &key,
                             const std::string &value)
  {
    int r = librbd::api::PoolMetadata<>::set(ioctx, key, value);
    return r;
  }

  int RBD::pool_metadata_remove(IoCtx& ioctx, const std::string &key)
  {
    int r = librbd::api::PoolMetadata<>::remove(ioctx, key);
    return r;
  }

  int RBD::pool_metadata_list(IoCtx& ioctx, const std::string &start,
                              uint64_t max, map<string, bufferlist> *pairs)
  {
    int r = librbd::api::PoolMetadata<>::list(ioctx, start, max, pairs);
    return r;
  }

  int RBD::config_list(IoCtx& io_ctx, std::vector<config_option_t> *options) {
    return librbd::api::Config<>::list(io_ctx, options);
  }

  RBD::AioCompletion::AioCompletion(void *cb_arg, callback_t complete_cb)
  {
    auto aio_comp = librbd::io::AioCompletion::create(
      cb_arg, complete_cb, this);
    aio_comp->external_callback = true;
    pc = reinterpret_cast<void*>(aio_comp);
  }

  bool RBD::AioCompletion::is_complete()
  {
    librbd::io::AioCompletion *c = (librbd::io::AioCompletion *)pc;
    return c->is_complete();
  }

  int RBD::AioCompletion::wait_for_complete()
  {
    librbd::io::AioCompletion *c = (librbd::io::AioCompletion *)pc;
    return c->wait_for_complete();
  }

  ssize_t RBD::AioCompletion::get_return_value()
  {
    librbd::io::AioCompletion *c = (librbd::io::AioCompletion *)pc;
    return c->get_return_value();
  }

  void *RBD::AioCompletion::get_arg()
  {
    librbd::io::AioCompletion *c = (librbd::io::AioCompletion *)pc;
    return c->get_arg();
  }

  void RBD::AioCompletion::release()
  {
    librbd::io::AioCompletion *c = (librbd::io::AioCompletion *)pc;
    c->release();
    delete this;
  }

  /*
    ImageOptions
  */

  ImageOptions::ImageOptions()
  {
    librbd::image_options_create(&opts);
  }

  ImageOptions::ImageOptions(rbd_image_options_t opts_)
  {
    librbd::image_options_create_ref(&opts, opts_);
  }

  ImageOptions::ImageOptions(const ImageOptions &imgopts)
  {
    librbd::image_options_copy(&opts, imgopts);
  }

  ImageOptions::~ImageOptions()
  {
    librbd::image_options_destroy(opts);
  }

  int ImageOptions::set(int optname, const std::string& optval)
  {
    return librbd::image_options_set(opts, optname, optval);
  }

  int ImageOptions::set(int optname, uint64_t optval)
  {
    return librbd::image_options_set(opts, optname, optval);
  }

  int ImageOptions::get(int optname, std::string* optval) const
  {
    return librbd::image_options_get(opts, optname, optval);
  }

  int ImageOptions::get(int optname, uint64_t* optval) const
  {
    return librbd::image_options_get(opts, optname, optval);
  }

  int ImageOptions::is_set(int optname, bool* is_set)
  {
    return librbd::image_options_is_set(opts, optname, is_set);
  }

  int ImageOptions::unset(int optname)
  {
    return librbd::image_options_unset(opts, optname);
  }

  void ImageOptions::clear()
  {
    librbd::image_options_clear(opts);
  }

  bool ImageOptions::empty() const
  {
    return librbd::image_options_is_empty(opts);
  }

  /*
    Image
  */

  Image::Image() : ctx(NULL)
  {
  }

  Image::~Image()
  {
    close();
  }

  int Image::close()
  {
    int r = 0;
    if (ctx) {
      ImageCtx *ictx = (ImageCtx *)ctx;
      tracepoint(librbd, close_image_enter, ictx, ictx->name.c_str(), ictx->id.c_str());

      r = ictx->state->close();
      ctx = NULL;

      tracepoint(librbd, close_image_exit, r);
    }
    return r;
  }

  int Image::aio_close(RBD::AioCompletion *c)
  {
    if (!ctx) {
      return -EINVAL;
    }

    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, aio_close_image_enter, ictx, ictx->name.c_str(), ictx->id.c_str(), c->pc);

    ictx->state->close(new C_AioCompletion(ictx, librbd::io::AIO_TYPE_CLOSE,
                                           get_aio_completion(c)));
    ctx = NULL;

    tracepoint(librbd, aio_close_image_exit, 0);
    return 0;
  }

  int Image::resize(uint64_t size)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, resize_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, size);
    librbd::NoOpProgressContext prog_ctx;
    int r = ictx->operations->resize(size, true, prog_ctx);
    tracepoint(librbd, resize_exit, r);
    return r;
  }

  int Image::resize2(uint64_t size, bool allow_shrink, librbd::ProgressContext& pctx)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, resize_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, size);
    int r = ictx->operations->resize(size, allow_shrink, pctx);
    tracepoint(librbd, resize_exit, r);
    return r;
  }

  int Image::resize_with_progress(uint64_t size, librbd::ProgressContext& pctx)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, resize_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, size);
    int r = ictx->operations->resize(size, true, pctx);
    tracepoint(librbd, resize_exit, r);
    return r;
  }

  int Image::stat(image_info_t& info, size_t infosize)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, stat_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    int r = librbd::info(ictx, info, infosize);
    tracepoint(librbd, stat_exit, r, &info);
    return r;
  }

  int Image::old_format(uint8_t *old)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, get_old_format_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    int r = librbd::get_old_format(ictx, old);
    tracepoint(librbd, get_old_format_exit, r, *old);
    return r;
  }

  int Image::size(uint64_t *size)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, get_size_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    int r = librbd::get_size(ictx, size);
    tracepoint(librbd, get_size_exit, r, *size);
    return r;
  }

  int Image::get_group(group_info_t *group_info, size_t group_info_size)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, image_get_group_enter, ictx->name.c_str());

    if (group_info_size != sizeof(group_info_t)) {
      tracepoint(librbd, image_get_group_exit, -ERANGE);
      return -ERANGE;
    }

    int r = librbd::api::Group<>::image_get_group(ictx, group_info);
    tracepoint(librbd, image_get_group_exit, r);
    return r;
  }

  int Image::features(uint64_t *features)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, get_features_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    int r = librbd::get_features(ictx, features);
    tracepoint(librbd, get_features_exit, r, *features);
    return r;
  }

  int Image::update_features(uint64_t features, bool enabled)
  {
    ImageCtx *ictx = reinterpret_cast<ImageCtx *>(ctx);
    tracepoint(librbd, update_features_enter, ictx, features, enabled);
    int r = ictx->operations->update_features(features, enabled);
    tracepoint(librbd, update_features_exit, r);
    return r;
  }

  int Image::get_op_features(uint64_t *op_features)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Image<>::get_op_features(ictx, op_features);
  }

  uint64_t Image::get_stripe_unit() const
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, get_stripe_unit_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    uint64_t stripe_unit = ictx->get_stripe_unit();
    tracepoint(librbd, get_stripe_unit_exit, 0, stripe_unit);
    return stripe_unit;
  }

  uint64_t Image::get_stripe_count() const
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, get_stripe_count_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    uint64_t stripe_count = ictx->get_stripe_count();
    tracepoint(librbd, get_stripe_count_exit, 0, stripe_count);
    return stripe_count;
  }

  int Image::get_create_timestamp(struct timespec *timestamp)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, get_create_timestamp_enter, ictx, ictx->name.c_str(),
               ictx->read_only);
    utime_t time = ictx->get_create_timestamp();
    time.to_timespec(timestamp);
    tracepoint(librbd, get_create_timestamp_exit, 0, timestamp);
    return 0;
  }

  int Image::get_access_timestamp(struct timespec *timestamp)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, get_access_timestamp_enter, ictx, ictx->name.c_str(),
               ictx->read_only);
    {
      std::shared_lock timestamp_locker{ictx->timestamp_lock};
      utime_t time = ictx->get_access_timestamp();
      time.to_timespec(timestamp);
    }
    tracepoint(librbd, get_access_timestamp_exit, 0, timestamp);
    return 0;
  }

  int Image::get_modify_timestamp(struct timespec *timestamp)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, get_modify_timestamp_enter, ictx, ictx->name.c_str(),
               ictx->read_only);
    {
      std::shared_lock timestamp_locker{ictx->timestamp_lock};
      utime_t time = ictx->get_modify_timestamp();
      time.to_timespec(timestamp);
    }
    tracepoint(librbd, get_modify_timestamp_exit, 0, timestamp);
    return 0;
  }

  int Image::overlap(uint64_t *overlap)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, get_overlap_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    int r = librbd::get_overlap(ictx, overlap);
    tracepoint(librbd, get_overlap_exit, r, *overlap);
    return r;
  }

  int Image::get_name(std::string *name)
  {
    ImageCtx *ictx = reinterpret_cast<ImageCtx *>(ctx);
    *name = ictx->name;
    return 0;
  }

  int Image::get_id(std::string *id)
  {
    ImageCtx *ictx = reinterpret_cast<ImageCtx *>(ctx);
    if (ictx->old_format) {
      return -EINVAL;
    }
    *id = ictx->id;
    return 0;
  }

  std::string Image::get_block_name_prefix()
  {
    ImageCtx *ictx = reinterpret_cast<ImageCtx *>(ctx);
    return ictx->object_prefix;
  }

  int64_t Image::get_data_pool_id()
  {
    ImageCtx *ictx = reinterpret_cast<ImageCtx *>(ctx);
    return librbd::api::Image<>::get_data_pool_id(ictx);
  }

  int Image::parent_info(string *parent_pool_name, string *parent_name,
			 string *parent_snap_name)
  {
    librbd::linked_image_spec_t parent_image;
    librbd::snap_spec_t parent_snap;
    int r = get_parent(&parent_image, &parent_snap);
    if (r >= 0) {
      if (parent_pool_name != nullptr) {
        *parent_pool_name = parent_image.pool_name;
      }
      if (parent_name != nullptr) {
        *parent_name = parent_image.image_name;
      }
      if (parent_snap_name != nullptr) {
        *parent_snap_name = parent_snap.name;
      }
    }
    return r;
  }

  int Image::parent_info2(string *parent_pool_name, string *parent_name,
                          string *parent_id, string *parent_snap_name)
  {
    librbd::linked_image_spec_t parent_image;
    librbd::snap_spec_t parent_snap;
    int r = get_parent(&parent_image, &parent_snap);
    if (r >= 0) {
      if (parent_pool_name != nullptr) {
        *parent_pool_name = parent_image.pool_name;
      }
      if (parent_name != nullptr) {
        *parent_name = parent_image.image_name;
      }
      if (parent_id != nullptr) {
        *parent_id = parent_image.image_id;
      }
      if (parent_snap_name != nullptr) {
        *parent_snap_name = parent_snap.name;
      }
    }
    return r;
  }

  int Image::get_parent(linked_image_spec_t *parent_image,
                        snap_spec_t *parent_snap)
  {
    auto ictx = reinterpret_cast<ImageCtx*>(ctx);
    tracepoint(librbd, get_parent_info_enter, ictx, ictx->name.c_str(),
               ictx->snap_name.c_str(), ictx->read_only);

    int r = librbd::api::Image<>::get_parent(ictx, parent_image, parent_snap);

    tracepoint(librbd, get_parent_info_exit, r,
               parent_image->pool_name.c_str(),
               parent_image->image_name.c_str(),
               parent_image->image_id.c_str(),
               parent_snap->name.c_str());
    return r;
  }

  int Image::get_flags(uint64_t *flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, get_flags_enter, ictx);
    int r = librbd::get_flags(ictx, flags);
    tracepoint(librbd, get_flags_exit, ictx, r, *flags);
    return r;
  }

  int Image::set_image_notification(int fd, int type)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, set_image_notification_enter, ictx, fd, type);
    int r = librbd::set_image_notification(ictx, fd, type);
    tracepoint(librbd, set_image_notification_exit, ictx, r);
    return r;
  }

  int Image::is_exclusive_lock_owner(bool *is_owner)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, is_exclusive_lock_owner_enter, ictx);
    int r = librbd::is_exclusive_lock_owner(ictx, is_owner);
    tracepoint(librbd, is_exclusive_lock_owner_exit, ictx, r, *is_owner);
    return r;
  }

  int Image::lock_acquire(rbd_lock_mode_t lock_mode)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, lock_acquire_enter, ictx, lock_mode);
    int r = librbd::lock_acquire(ictx, lock_mode);
    tracepoint(librbd, lock_acquire_exit, ictx, r);
    return r;
  }

  int Image::lock_release()
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, lock_release_enter, ictx);
    int r = librbd::lock_release(ictx);
    tracepoint(librbd, lock_release_exit, ictx, r);
    return r;
  }

  int Image::lock_get_owners(rbd_lock_mode_t *lock_mode,
                             std::list<std::string> *lock_owners)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, lock_get_owners_enter, ictx);
    int r = librbd::lock_get_owners(ictx, lock_mode, lock_owners);
    tracepoint(librbd, lock_get_owners_exit, ictx, r);
    return r;
  }

  int Image::lock_break(rbd_lock_mode_t lock_mode,
                        const std::string &lock_owner)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, lock_break_enter, ictx, lock_mode, lock_owner.c_str());
    int r = librbd::lock_break(ictx, lock_mode, lock_owner);
    tracepoint(librbd, lock_break_exit, ictx, r);
    return r;
  }

  int Image::rebuild_object_map(ProgressContext &prog_ctx)
  {
    ImageCtx *ictx = reinterpret_cast<ImageCtx*>(ctx);
    return ictx->operations->rebuild_object_map(prog_ctx);
  }

  int Image::check_object_map(ProgressContext &prog_ctx)
  {
    ImageCtx *ictx = reinterpret_cast<ImageCtx*>(ctx);
    return ictx->operations->check_object_map(prog_ctx);
  }

  int Image::copy(IoCtx& dest_io_ctx, const char *destname)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, copy_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, dest_io_ctx.get_pool_name().c_str(), dest_io_ctx.get_id(), destname);
    ImageOptions opts;
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::copy(ictx, dest_io_ctx, destname, opts, prog_ctx, 0);
    tracepoint(librbd, copy_exit, r);
    return r;
  }

  int Image::copy2(Image& dest)
  {
    ImageCtx *srcctx = (ImageCtx *)ctx;
    ImageCtx *destctx = (ImageCtx *)dest.ctx;
    tracepoint(librbd, copy2_enter, srcctx, srcctx->name.c_str(), srcctx->snap_name.c_str(), srcctx->read_only, destctx, destctx->name.c_str(), destctx->snap_name.c_str(), destctx->read_only);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::copy(srcctx, destctx, prog_ctx, 0);
    tracepoint(librbd, copy2_exit, r);
    return r;
  }

  int Image::copy3(IoCtx& dest_io_ctx, const char *destname, ImageOptions& opts)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, copy3_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, dest_io_ctx.get_pool_name().c_str(), dest_io_ctx.get_id(), destname, opts.opts);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::copy(ictx, dest_io_ctx, destname, opts, prog_ctx, 0);
    tracepoint(librbd, copy3_exit, r);
    return r;
  }

  int Image::copy4(IoCtx& dest_io_ctx, const char *destname, ImageOptions& opts, size_t sparse_size)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, copy4_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, dest_io_ctx.get_pool_name().c_str(), dest_io_ctx.get_id(), destname, opts.opts, sparse_size);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::copy(ictx, dest_io_ctx, destname, opts, prog_ctx, sparse_size);
    tracepoint(librbd, copy4_exit, r);
    return r;
  }

  int Image::copy_with_progress(IoCtx& dest_io_ctx, const char *destname,
				librbd::ProgressContext &pctx)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, copy_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, dest_io_ctx.get_pool_name().c_str(), dest_io_ctx.get_id(), destname);
    ImageOptions opts;
    int r = librbd::copy(ictx, dest_io_ctx, destname, opts, pctx, 0);
    tracepoint(librbd, copy_exit, r);
    return r;
  }

  int Image::copy_with_progress2(Image& dest, librbd::ProgressContext &pctx)
  {
    ImageCtx *srcctx = (ImageCtx *)ctx;
    ImageCtx *destctx = (ImageCtx *)dest.ctx;
    tracepoint(librbd, copy2_enter, srcctx, srcctx->name.c_str(), srcctx->snap_name.c_str(), srcctx->read_only, destctx, destctx->name.c_str(), destctx->snap_name.c_str(), destctx->read_only);
    int r = librbd::copy(srcctx, destctx, pctx, 0);
    tracepoint(librbd, copy2_exit, r);
    return r;
  }

  int Image::copy_with_progress3(IoCtx& dest_io_ctx, const char *destname,
				 ImageOptions& opts,
				 librbd::ProgressContext &pctx)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, copy3_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, dest_io_ctx.get_pool_name().c_str(), dest_io_ctx.get_id(), destname, opts.opts);
    int r = librbd::copy(ictx, dest_io_ctx, destname, opts, pctx, 0);
    tracepoint(librbd, copy3_exit, r);
    return r;
  }

  int Image::copy_with_progress4(IoCtx& dest_io_ctx, const char *destname,
				 ImageOptions& opts,
				 librbd::ProgressContext &pctx,
				 size_t sparse_size)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, copy4_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, dest_io_ctx.get_pool_name().c_str(), dest_io_ctx.get_id(), destname, opts.opts, sparse_size);
    int r = librbd::copy(ictx, dest_io_ctx, destname, opts, pctx, sparse_size);
    tracepoint(librbd, copy4_exit, r);
    return r;
  }

  int Image::deep_copy(IoCtx& dest_io_ctx, const char *destname,
                       ImageOptions& opts)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, deep_copy_enter, ictx, ictx->name.c_str(),
               ictx->snap_name.c_str(), ictx->read_only,
               dest_io_ctx.get_pool_name().c_str(), dest_io_ctx.get_id(),
               destname, opts.opts);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::api::Image<>::deep_copy(ictx, dest_io_ctx, destname, opts,
                                            prog_ctx);
    tracepoint(librbd, deep_copy_exit, r);
    return r;
  }

  int Image::deep_copy_with_progress(IoCtx& dest_io_ctx, const char *destname,
                                     ImageOptions& opts,
                                     librbd::ProgressContext &prog_ctx)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, deep_copy_enter, ictx, ictx->name.c_str(),
               ictx->snap_name.c_str(), ictx->read_only,
               dest_io_ctx.get_pool_name().c_str(), dest_io_ctx.get_id(),
               destname, opts.opts);
    int r = librbd::api::Image<>::deep_copy(ictx, dest_io_ctx, destname, opts,
                                            prog_ctx);
    tracepoint(librbd, deep_copy_exit, r);
    return r;
  }

  int Image::flatten()
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, flatten_enter, ictx, ictx->name.c_str(), ictx->id.c_str());
    librbd::NoOpProgressContext prog_ctx;
    int r = ictx->operations->flatten(prog_ctx);
    tracepoint(librbd, flatten_exit, r);
    return r;
  }

  int Image::flatten_with_progress(librbd::ProgressContext& prog_ctx)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, flatten_enter, ictx, ictx->name.c_str(), ictx->id.c_str());
    int r = ictx->operations->flatten(prog_ctx);
    tracepoint(librbd, flatten_exit, r);
    return r;
  }

  int Image::sparsify(size_t sparse_size)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, sparsify_enter, ictx, ictx->name.c_str(), sparse_size,
               ictx->id.c_str());
    librbd::NoOpProgressContext prog_ctx;
    int r = ictx->operations->sparsify(sparse_size, prog_ctx);
    tracepoint(librbd, sparsify_exit, r);
    return r;
  }

  int Image::sparsify_with_progress(size_t sparse_size,
                                    librbd::ProgressContext& prog_ctx)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, sparsify_enter, ictx, ictx->name.c_str(), sparse_size,
               ictx->id.c_str());
    int r = ictx->operations->sparsify(sparse_size, prog_ctx);
    tracepoint(librbd, sparsify_exit, r);
    return r;
  }

  int Image::list_children(set<pair<string, string> > *children)
  {
    std::vector<linked_image_spec_t> images;
    int r = list_children3(&images);
    if (r < 0) {
      return r;
    }

    for (auto& image : images) {
      if (!image.trash) {
        children->insert({image.pool_name, image.image_name});
      }
    }
    return 0;
  }

  int Image::list_children2(vector<librbd::child_info_t> *children)
  {
    std::vector<linked_image_spec_t> images;
    int r = list_children3(&images);
    if (r < 0) {
      return r;
    }

    for (auto& image : images) {
      children->push_back({
        .pool_name = image.pool_name,
        .image_name = image.image_name,
        .image_id = image.image_id,
        .trash = image.trash});
    }

    return 0;
  }

  int Image::list_children3(std::vector<linked_image_spec_t> *images)
  {
    auto ictx = reinterpret_cast<ImageCtx*>(ctx);
    tracepoint(librbd, list_children_enter, ictx, ictx->name.c_str(),
               ictx->snap_name.c_str(), ictx->read_only);

    int r = librbd::api::Image<>::list_children(ictx, images);
#ifdef WITH_LTTNG
    if (r >= 0) {
      for (auto& it : *images) {
        tracepoint(librbd, list_children_entry, it.pool_name.c_str(),
                   it.image_name.c_str());
      }
    }
#endif
    tracepoint(librbd, list_children_exit, r);
    return r;
  }

  int Image::list_descendants(std::vector<linked_image_spec_t> *images)
  {
    auto ictx = reinterpret_cast<ImageCtx*>(ctx);

    images->clear();
    int r = librbd::api::Image<>::list_descendants(ictx, {}, images);
    return r;
  }

  int Image::list_lockers(std::list<librbd::locker_t> *lockers,
			  bool *exclusive, string *tag)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, list_lockers_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    int r = librbd::list_lockers(ictx, lockers, exclusive, tag);
    if (r >= 0) {
      for (std::list<librbd::locker_t>::const_iterator it = lockers->begin();
	   it != lockers->end(); ++it) {
	tracepoint(librbd, list_lockers_entry, it->client.c_str(), it->cookie.c_str(), it->address.c_str());
      }
    }
    tracepoint(librbd, list_lockers_exit, r);
    return r;
  }

  int Image::lock_exclusive(const string& cookie)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, lock_exclusive_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, cookie.c_str());
    int r = librbd::lock(ictx, true, cookie, "");
    tracepoint(librbd, lock_exclusive_exit, r);
    return r;
  }

  int Image::lock_shared(const string& cookie, const std::string& tag)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, lock_shared_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, cookie.c_str(), tag.c_str());
    int r = librbd::lock(ictx, false, cookie, tag);
    tracepoint(librbd, lock_shared_exit, r);
    return r;
  }

  int Image::unlock(const string& cookie)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, unlock_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, cookie.c_str());
    int r = librbd::unlock(ictx, cookie);
    tracepoint(librbd, unlock_exit, r);
    return r;
  }

  int Image::break_lock(const string& client, const string& cookie)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, break_lock_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, client.c_str(), cookie.c_str());
    int r = librbd::break_lock(ictx, client, cookie);
    tracepoint(librbd, break_lock_exit, r);
    return r;
  }

  int Image::snap_create(const char *snap_name)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_create_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, snap_name);
    int r = ictx->operations->snap_create(cls::rbd::UserSnapshotNamespace(),
					  snap_name);
    tracepoint(librbd, snap_create_exit, r);
    return r;
  }

  int Image::snap_remove(const char *snap_name)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_remove_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, snap_name);
    librbd::NoOpProgressContext prog_ctx;
    int r = librbd::api::Snapshot<>::remove(ictx, snap_name, 0, prog_ctx);
    tracepoint(librbd, snap_remove_exit, r);
    return r;
  }

  int Image::snap_remove2(const char *snap_name, uint32_t flags, ProgressContext& pctx)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_remove2_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, snap_name, flags);
    int r = librbd::api::Snapshot<>::remove(ictx, snap_name, flags, pctx);
    tracepoint(librbd, snap_remove_exit, r);
    return r;
  }

  int Image::snap_remove_by_id(uint64_t snap_id)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Snapshot<>::remove(ictx, snap_id);
  }

  int Image::snap_rollback(const char *snap_name)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_rollback_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, snap_name);
    librbd::NoOpProgressContext prog_ctx;
    int r = ictx->operations->snap_rollback(cls::rbd::UserSnapshotNamespace(), snap_name, prog_ctx);
    tracepoint(librbd, snap_rollback_exit, r);
    return r;
  }

  int Image::snap_rename(const char *srcname, const char *dstname)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_rename_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, srcname, dstname);
    int r = ictx->operations->snap_rename(srcname, dstname);
    tracepoint(librbd, snap_rename_exit, r);
    return r;
  }

  int Image::snap_rollback_with_progress(const char *snap_name,
					 ProgressContext& prog_ctx)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_rollback_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, snap_name);
    int r = ictx->operations->snap_rollback(cls::rbd::UserSnapshotNamespace(), snap_name, prog_ctx);
    tracepoint(librbd, snap_rollback_exit, r);
    return r;
  }

  int Image::snap_protect(const char *snap_name)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_protect_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, snap_name);
    int r = ictx->operations->snap_protect(cls::rbd::UserSnapshotNamespace(), snap_name);
    tracepoint(librbd, snap_protect_exit, r);
    return r;
  }

  int Image::snap_unprotect(const char *snap_name)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_unprotect_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, snap_name);
    int r = ictx->operations->snap_unprotect(cls::rbd::UserSnapshotNamespace(), snap_name);
    tracepoint(librbd, snap_unprotect_exit, r);
    return r;
  }

  int Image::snap_is_protected(const char *snap_name, bool *is_protected)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_is_protected_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, snap_name);
    int r = librbd::api::Snapshot<>::is_protected(ictx, snap_name, is_protected);
    tracepoint(librbd, snap_is_protected_exit, r, *is_protected ? 1 : 0);
    return r;
  }

  int Image::snap_list(vector<librbd::snap_info_t>& snaps)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_list_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, &snaps);
    int r = librbd::api::Snapshot<>::list(ictx, snaps);
    if (r >= 0) {
      for (int i = 0, n = snaps.size(); i < n; i++) {
	tracepoint(librbd, snap_list_entry, snaps[i].id, snaps[i].size, snaps[i].name.c_str());
      }
    }
    tracepoint(librbd, snap_list_exit, r, snaps.size());
    if (r >= 0) {
      // A little ugly, but the C++ API doesn't need a Image::snap_list_end,
      // and we want the tracepoints to mirror the C API
      tracepoint(librbd, snap_list_end_enter, &snaps);
      tracepoint(librbd, snap_list_end_exit);
    }
    return r;
  }

  bool Image::snap_exists(const char *snap_name)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_exists_enter, ictx, ictx->name.c_str(), 
      ictx->snap_name.c_str(), ictx->read_only, snap_name);
    bool exists; 
    int r = librbd::api::Snapshot<>::exists(ictx, cls::rbd::UserSnapshotNamespace(), snap_name, &exists);
    tracepoint(librbd, snap_exists_exit, r, exists);
    if (r < 0) {
      // lie to caller since we don't know the real answer yet.
      return false;
    }
    return exists;
  }

  // A safer verion of snap_exists.
  int Image::snap_exists2(const char *snap_name, bool *exists)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_exists_enter, ictx, ictx->name.c_str(), 
      ictx->snap_name.c_str(), ictx->read_only, snap_name);
    int r = librbd::api::Snapshot<>::exists(ictx, cls::rbd::UserSnapshotNamespace(), snap_name, exists);
    tracepoint(librbd, snap_exists_exit, r, *exists);
    return r;
  }

  int Image::snap_get_timestamp(uint64_t snap_id, struct timespec *timestamp)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_get_timestamp_enter, ictx, ictx->name.c_str());
    int r = librbd::api::Snapshot<>::get_timestamp(ictx, snap_id, timestamp);
    tracepoint(librbd, snap_get_timestamp_exit, r);
    return r;
  }

  int Image::snap_get_limit(uint64_t *limit)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_get_limit_enter, ictx, ictx->name.c_str());
    int r = librbd::api::Snapshot<>::get_limit(ictx, limit);
    tracepoint(librbd, snap_get_limit_exit, r, *limit);
    return r;
  }

  int Image::snap_get_namespace_type(uint64_t snap_id,
				     snap_namespace_type_t *namespace_type) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_get_namespace_type_enter, ictx, ictx->name.c_str());
    int r = librbd::api::Snapshot<>::get_namespace_type(ictx, snap_id, namespace_type);
    tracepoint(librbd, snap_get_namespace_type_exit, r);
    return r;
  }

  int Image::snap_get_group_namespace(uint64_t snap_id,
			              snap_group_namespace_t *group_snap,
                                      size_t group_snap_size) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_get_group_namespace_enter, ictx,
               ictx->name.c_str());

    if (group_snap_size != sizeof(snap_group_namespace_t)) {
      tracepoint(librbd, snap_get_group_namespace_exit, -ERANGE);
      return -ERANGE;
    }

    int r = librbd::api::Snapshot<>::get_group_namespace(ictx, snap_id,
                                                         group_snap);
    tracepoint(librbd, snap_get_group_namespace_exit, r);
    return r;
  }

  int Image::snap_get_trash_namespace(uint64_t snap_id,
                                      std::string* original_name) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Snapshot<>::get_trash_namespace(ictx, snap_id,
                                                        original_name);
  }

  int Image::snap_get_mirror_namespace(
      uint64_t snap_id, snap_mirror_namespace_t *mirror_snap,
      size_t mirror_snap_size) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    if (mirror_snap_size != sizeof(snap_mirror_namespace_t)) {
      return -ERANGE;
    }

    int r = librbd::api::Snapshot<>::get_mirror_namespace(
        ictx, snap_id, mirror_snap);
    return r;
  }

  int Image::snap_set_limit(uint64_t limit)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;

    tracepoint(librbd, snap_set_limit_enter, ictx, ictx->name.c_str(), limit);
    int r = ictx->operations->snap_set_limit(limit);
    tracepoint(librbd, snap_set_limit_exit, r);
    return r;
  }

  int Image::snap_set(const char *snap_name)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, snap_set_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, snap_name);
    int r = librbd::api::Image<>::snap_set(
      ictx, cls::rbd::UserSnapshotNamespace(), snap_name);
    tracepoint(librbd, snap_set_exit, r);
    return r;
  }

  int Image::snap_set_by_id(uint64_t snap_id)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Image<>::snap_set(ictx, snap_id);
  }

  int Image::snap_get_name(uint64_t snap_id, std::string *snap_name)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Snapshot<>::get_name(ictx, snap_id, snap_name);
  }

  int Image::snap_get_id(const std::string snap_name, uint64_t *snap_id)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Snapshot<>::get_id(ictx, snap_name, snap_id);
  }

  ssize_t Image::read(uint64_t ofs, size_t len, bufferlist& bl)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, read_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, ofs, len);
    bufferptr ptr(len);
    bl.push_back(std::move(ptr));
    
    int r = ictx->io_work_queue->read(ofs, len, io::ReadResult{&bl}, 0);
    tracepoint(librbd, read_exit, r);
    return r;
  }

  ssize_t Image::read2(uint64_t ofs, size_t len, bufferlist& bl, int op_flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, read2_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(),
		ictx->read_only, ofs, len, op_flags);
    bufferptr ptr(len);
    bl.push_back(std::move(ptr));
    
    int r = ictx->io_work_queue->read(ofs, len, io::ReadResult{&bl}, op_flags);
    tracepoint(librbd, read_exit, r);
    return r;
  }

  int64_t Image::read_iterate(uint64_t ofs, size_t len,
			      int (*cb)(uint64_t, size_t, const char *, void *),
			      void *arg)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, read_iterate_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, ofs, len);
    
    int64_t r = librbd::read_iterate(ictx, ofs, len, cb, arg);
    tracepoint(librbd, read_iterate_exit, r);
    return r;
  }

  int Image::read_iterate2(uint64_t ofs, uint64_t len,
			      int (*cb)(uint64_t, size_t, const char *, void *),
			      void *arg)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, read_iterate2_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, ofs, len);
    
    int64_t r = librbd::read_iterate(ictx, ofs, len, cb, arg);
    if (r > 0)
      r = 0;
    tracepoint(librbd, read_iterate2_exit, r);
    return (int)r;
  }

  int Image::diff_iterate(const char *fromsnapname,
			  uint64_t ofs, uint64_t len,
			  int (*cb)(uint64_t, size_t, int, void *),
			  void *arg)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, diff_iterate_enter, ictx, ictx->name.c_str(),
               ictx->snap_name.c_str(), ictx->read_only, fromsnapname, ofs, len,
               true, false);
    int r = librbd::api::DiffIterate<>::diff_iterate(ictx,
						     cls::rbd::UserSnapshotNamespace(),
						     fromsnapname, ofs,
                                                     len, true, false, cb, arg);
    tracepoint(librbd, diff_iterate_exit, r);
    return r;
  }

  int Image::diff_iterate2(const char *fromsnapname, uint64_t ofs, uint64_t len,
                           bool include_parent, bool whole_object,
                           int (*cb)(uint64_t, size_t, int, void *), void *arg)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, diff_iterate_enter, ictx, ictx->name.c_str(),
              ictx->snap_name.c_str(), ictx->read_only, fromsnapname, ofs, len,
              include_parent, whole_object);
    int r = librbd::api::DiffIterate<>::diff_iterate(ictx,
						     cls::rbd::UserSnapshotNamespace(),
						     fromsnapname, ofs,
                                                     len, include_parent,
                                                     whole_object, cb, arg);
    tracepoint(librbd, diff_iterate_exit, r);
    return r;
  }

  ssize_t Image::write(uint64_t ofs, size_t len, bufferlist& bl)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, write_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, ofs, len, bl.length() < len ? NULL : bl.c_str());
    if (bl.length() < len) {
      tracepoint(librbd, write_exit, -EINVAL);
      return -EINVAL;
    }

    int r = ictx->io_work_queue->write(ofs, len, bufferlist{bl}, 0);
    tracepoint(librbd, write_exit, r);
    return r;
  }

   ssize_t Image::write2(uint64_t ofs, size_t len, bufferlist& bl, int op_flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, write2_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only,
		ofs, len, bl.length() < len ? NULL : bl.c_str(), op_flags);
    if (bl.length() < len) {
      tracepoint(librbd, write_exit, -EINVAL);
      return -EINVAL;
    }

    int r = ictx->io_work_queue->write(ofs, len, bufferlist{bl}, op_flags);
    tracepoint(librbd, write_exit, r);
    return r;
  }

  int Image::discard(uint64_t ofs, uint64_t len)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, discard_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, ofs, len);
    if (len > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        tracepoint(librbd, discard_exit, -EINVAL);
        return -EINVAL;
    }
    int r = ictx->io_work_queue->discard(
      ofs, len, ictx->discard_granularity_bytes);
    tracepoint(librbd, discard_exit, r);
    return r;
  }

  ssize_t Image::writesame(uint64_t ofs, size_t len, bufferlist& bl, int op_flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, writesame_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(),
               ictx->read_only, ofs, len, bl.length() <= 0 ? NULL : bl.c_str(), bl.length(),
               op_flags);
    if (bl.length() <= 0 || len % bl.length() ||
        len > static_cast<size_t>(std::numeric_limits<int>::max())) {
      tracepoint(librbd, writesame_exit, -EINVAL);
      return -EINVAL;
    }

    bool discard_zero = ictx->config.get_val<bool>("rbd_discard_on_zeroed_write_same");
    if (discard_zero && bl.is_zero()) {
      int r = ictx->io_work_queue->write_zeroes(ofs, len, 0U, op_flags);
      tracepoint(librbd, writesame_exit, r);
      return r;
    }

    int r = ictx->io_work_queue->writesame(ofs, len, bufferlist{bl}, op_flags);
    tracepoint(librbd, writesame_exit, r);
    return r;
  }

  ssize_t Image::write_zeroes(uint64_t ofs, size_t len, int zero_flags,
                              int op_flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return ictx->io_work_queue->write_zeroes(ofs, len, zero_flags, op_flags);
  }

  ssize_t Image::compare_and_write(uint64_t ofs, size_t len,
                                   ceph::bufferlist &cmp_bl, ceph::bufferlist& bl,
                                   uint64_t *mismatch_off, int op_flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, compare_and_write_enter, ictx, ictx->name.c_str(),
               ictx->snap_name.c_str(),
               ictx->read_only, ofs, len, cmp_bl.length() < len ? NULL : cmp_bl.c_str(),
               bl.length() < len ? NULL : bl.c_str(), op_flags);

    if (bl.length() < len) {
      tracepoint(librbd, write_exit, -EINVAL);
      return -EINVAL;
    }

    int r = ictx->io_work_queue->compare_and_write(ofs, len, bufferlist{cmp_bl},
                                                   bufferlist{bl}, mismatch_off,
                                                   op_flags);

    tracepoint(librbd, compare_and_write_exit, r);

    return r;
  }

  int Image::aio_write(uint64_t off, size_t len, bufferlist& bl,
		       RBD::AioCompletion *c)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, aio_write_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, off, len, bl.length() < len ? NULL : bl.c_str(), c->pc);
    if (bl.length() < len) {
      tracepoint(librbd, aio_write_exit, -EINVAL);
      return -EINVAL;
    }
    ictx->io_work_queue->aio_write(get_aio_completion(c), off, len,
                                   bufferlist{bl}, 0);

    tracepoint(librbd, aio_write_exit, 0);
    return 0;
  }

  int Image::aio_write2(uint64_t off, size_t len, bufferlist& bl,
			  RBD::AioCompletion *c, int op_flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, aio_write2_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(),
		ictx->read_only, off, len, bl.length() < len ? NULL : bl.c_str(), c->pc, op_flags);
    if (bl.length() < len) {
      tracepoint(librbd, aio_write_exit, -EINVAL);
      return -EINVAL;
    }
    ictx->io_work_queue->aio_write(get_aio_completion(c), off, len,
                                   bufferlist{bl}, op_flags);

    tracepoint(librbd, aio_write_exit, 0);
    return 0;
  }

  int Image::aio_discard(uint64_t off, uint64_t len, RBD::AioCompletion *c)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, aio_discard_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, off, len, c->pc);
    ictx->io_work_queue->aio_discard(
      get_aio_completion(c), off, len, ictx->discard_granularity_bytes);
    tracepoint(librbd, aio_discard_exit, 0);
    return 0;
  }

  int Image::aio_read(uint64_t off, size_t len, bufferlist& bl,
		      RBD::AioCompletion *c)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, aio_read_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, off, len, bl.c_str(), c->pc);
    ldout(ictx->cct, 10) << "Image::aio_read() buf=" << (void *)bl.c_str() << "~"
			 << (void *)(bl.c_str() + len - 1) << dendl;

    ictx->io_work_queue->aio_read(get_aio_completion(c), off, len,
                                  io::ReadResult{&bl}, 0);
    tracepoint(librbd, aio_read_exit, 0);
    return 0;
  }

  int Image::aio_read2(uint64_t off, size_t len, bufferlist& bl,
			RBD::AioCompletion *c, int op_flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, aio_read2_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(),
		ictx->read_only, off, len, bl.c_str(), c->pc, op_flags);
    ldout(ictx->cct, 10) << "Image::aio_read() buf=" << (void *)bl.c_str() << "~"
			 << (void *)(bl.c_str() + len - 1) << dendl;

    ictx->io_work_queue->aio_read(get_aio_completion(c), off, len,
                                  io::ReadResult{&bl}, op_flags);
    tracepoint(librbd, aio_read_exit, 0);
    return 0;
  }

  int Image::flush()
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, flush_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    int r = ictx->io_work_queue->flush();
    tracepoint(librbd, flush_exit, r);
    return r;
  }

  int Image::aio_flush(RBD::AioCompletion *c)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, aio_flush_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, c->pc);
    ictx->io_work_queue->aio_flush(get_aio_completion(c));
    tracepoint(librbd, aio_flush_exit, 0);
    return 0;
  }

  int Image::aio_writesame(uint64_t off, size_t len, bufferlist& bl,
                           RBD::AioCompletion *c, int op_flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, aio_writesame_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(),
               ictx->read_only, off, len, bl.length() <= len ? NULL : bl.c_str(), bl.length(),
               c->pc, op_flags);
    if (bl.length() <= 0 || len % bl.length()) {
      tracepoint(librbd, aio_writesame_exit, -EINVAL);
      return -EINVAL;
    }

    bool discard_zero = ictx->config.get_val<bool>("rbd_discard_on_zeroed_write_same");
    if (discard_zero && bl.is_zero()) {
      ictx->io_work_queue->aio_write_zeroes(get_aio_completion(c), off, len, 0U,
                                            op_flags, true);
      tracepoint(librbd, aio_writesame_exit, 0);
      return 0;
    }

    ictx->io_work_queue->aio_writesame(get_aio_completion(c), off, len,
                                       bufferlist{bl}, op_flags);
    tracepoint(librbd, aio_writesame_exit, 0);
    return 0;
  }

  int Image::aio_write_zeroes(uint64_t off, size_t len, RBD::AioCompletion *c,
                              int zero_flags, int op_flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    ictx->io_work_queue->aio_write_zeroes(
      get_aio_completion(c), off, len, zero_flags, op_flags, true);
    return 0;
  }

  int Image::aio_compare_and_write(uint64_t off, size_t len,
                                   ceph::bufferlist& cmp_bl, ceph::bufferlist& bl,
                                   RBD::AioCompletion *c, uint64_t *mismatch_off,
                                   int op_flags)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, aio_compare_and_write_enter, ictx, ictx->name.c_str(),
               ictx->snap_name.c_str(),
               ictx->read_only, off, len, cmp_bl.length() < len ? NULL : cmp_bl.c_str(),
               bl.length() < len ? NULL : bl.c_str(), c->pc, op_flags);

    if (bl.length() < len) {
      tracepoint(librbd, compare_and_write_exit, -EINVAL);
      return -EINVAL;
    }

    ictx->io_work_queue->aio_compare_and_write(get_aio_completion(c), off, len,
                                               bufferlist{cmp_bl}, bufferlist{bl},
                                               mismatch_off, op_flags, false);

    tracepoint(librbd, aio_compare_and_write_exit, 0);

    return 0;
  }

  int Image::invalidate_cache()
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, invalidate_cache_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    int r = librbd::invalidate_cache(ictx);
    tracepoint(librbd, invalidate_cache_exit, r);
    return r;
  }

  int Image::poll_io_events(RBD::AioCompletion **comps, int numcomp)
  {
    io::AioCompletion *cs[numcomp];
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, poll_io_events_enter, ictx, numcomp);
    int r = librbd::poll_io_events(ictx, cs, numcomp);
    tracepoint(librbd, poll_io_events_exit, r);
    if (r > 0) {
      for (int i = 0; i < r; ++i)
        comps[i] = (RBD::AioCompletion *)cs[i]->rbd_comp;
    }
    return r;
  }

  int Image::metadata_get(const std::string &key, std::string *value)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, metadata_get_enter, ictx, key.c_str());
    int r = librbd::metadata_get(ictx, key, value);
    if (r < 0) {
      tracepoint(librbd, metadata_get_exit, r, key.c_str(), NULL);
    } else {
      tracepoint(librbd, metadata_get_exit, r, key.c_str(), value->c_str());
    }
    return r;
  }

  int Image::metadata_set(const std::string &key, const std::string &value)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, metadata_set_enter, ictx, key.c_str(), value.c_str());
    int r = ictx->operations->metadata_set(key, value);
    tracepoint(librbd, metadata_set_exit, r);
    return r;
  }

  int Image::metadata_remove(const std::string &key)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, metadata_remove_enter, ictx, key.c_str());
    int r = ictx->operations->metadata_remove(key);
    tracepoint(librbd, metadata_remove_exit, r);
    return r;
  }

  int Image::metadata_list(const std::string &start, uint64_t max, map<string, bufferlist> *pairs)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, metadata_list_enter, ictx);
    int r = librbd::metadata_list(ictx, start, max, pairs);
    if (r >= 0) {
      for (map<string, bufferlist>::iterator it = pairs->begin();
           it != pairs->end(); ++it) {
        tracepoint(librbd, metadata_list_entry, it->first.c_str(), it->second.c_str());
      }
    }
    tracepoint(librbd, metadata_list_exit, r);
    return r;
  }

  int Image::mirror_image_enable() {
    return mirror_image_enable2(RBD_MIRROR_IMAGE_MODE_JOURNAL);
  }

  int Image::mirror_image_enable2(mirror_image_mode_t mode) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Mirror<>::image_enable(ictx, mode, false);
  }

  int Image::mirror_image_disable(bool force) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Mirror<>::image_disable(ictx, force);
  }

  int Image::mirror_image_promote(bool force) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Mirror<>::image_promote(ictx, force);
  }

  int Image::mirror_image_demote() {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Mirror<>::image_demote(ictx);
  }

  int Image::mirror_image_resync()
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Mirror<>::image_resync(ictx);
  }

  int Image::mirror_image_create_snapshot(uint64_t *snap_id)
  {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Mirror<>::image_snapshot_create(ictx, 0U, snap_id);
  }

  int Image::mirror_image_get_info(mirror_image_info_t *mirror_image_info,
                                   size_t info_size) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    if (sizeof(mirror_image_info_t) != info_size) {
      return -ERANGE;
    }

    return librbd::api::Mirror<>::image_get_info(ictx, mirror_image_info);
  }

  int Image::mirror_image_get_mode(mirror_image_mode_t *mode) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    return librbd::api::Mirror<>::image_get_mode(ictx, mode);
  }

  int Image::mirror_image_get_global_status(
      mirror_image_global_status_t *mirror_image_global_status,
      size_t status_size) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    if (sizeof(mirror_image_global_status_t) != status_size) {
      return -ERANGE;
    }

    return librbd::api::Mirror<>::image_get_global_status(
      ictx, mirror_image_global_status);
  }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

  int Image::mirror_image_get_status(mirror_image_status_t *mirror_image_status,
				     size_t status_size) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    if (sizeof(mirror_image_status_t) != status_size) {
      return -ERANGE;
    }

    mirror_image_global_status_t mirror_image_global_status;
    int r = librbd::api::Mirror<>::image_get_global_status(
      ictx, &mirror_image_global_status);
    if (r < 0) {
      return r;
    }

    librbd::mirror_image_site_status_t local_status;
    r = get_local_mirror_image_site_status(mirror_image_global_status,
                                           &local_status);
    if (r < 0) {
      return r;
    }

    *mirror_image_status = mirror_image_status_t{
      mirror_image_global_status.name, mirror_image_global_status.info,
      local_status.state, local_status.description, local_status.last_update,
      local_status.up};
    return 0;
  }

#pragma GCC diagnostic pop

  int Image::mirror_image_get_instance_id(std::string *instance_id) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    return librbd::api::Mirror<>::image_get_instance_id(ictx, instance_id);
  }

  int Image::aio_mirror_image_promote(bool force, RBD::AioCompletion *c) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    librbd::api::Mirror<>::image_promote(
      ictx, force, new C_AioCompletion(ictx, librbd::io::AIO_TYPE_GENERIC,
                                       get_aio_completion(c)));
    return 0;
  }

  int Image::aio_mirror_image_demote(RBD::AioCompletion *c) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    librbd::api::Mirror<>::image_demote(
      ictx, new C_AioCompletion(ictx, librbd::io::AIO_TYPE_GENERIC,
                                get_aio_completion(c)));
    return 0;
  }

  int Image::aio_mirror_image_get_info(mirror_image_info_t *mirror_image_info,
                                       size_t info_size,
                                       RBD::AioCompletion *c) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    if (sizeof(mirror_image_info_t) != info_size) {
      return -ERANGE;
    }

    librbd::api::Mirror<>::image_get_info(
      ictx, mirror_image_info,
      new C_AioCompletion(ictx, librbd::io::AIO_TYPE_GENERIC,
                          get_aio_completion(c)));
    return 0;
  }

  int Image::aio_mirror_image_get_mode(mirror_image_mode_t *mode,
                                       RBD::AioCompletion *c) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    librbd::api::Mirror<>::image_get_mode(
      ictx, mode, new C_AioCompletion(ictx, librbd::io::AIO_TYPE_GENERIC,
                                      get_aio_completion(c)));
    return 0;
  }

  int Image::aio_mirror_image_get_global_status(
      mirror_image_global_status_t *status, size_t status_size,
      RBD::AioCompletion *c) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    if (sizeof(mirror_image_global_status_t) != status_size) {
      return -ERANGE;
    }

    librbd::api::Mirror<>::image_get_global_status(
      ictx, status, new C_AioCompletion(ictx, librbd::io::AIO_TYPE_GENERIC,
                                        get_aio_completion(c)));
    return 0;
  }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

  int Image::aio_mirror_image_get_status(mirror_image_status_t *status,
                                         size_t status_size,
                                         RBD::AioCompletion *c) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    if (sizeof(mirror_image_status_t) != status_size) {
      return -ERANGE;
    }

    auto ctx = new C_MirrorImageGetStatus(
      status, new C_AioCompletion(ictx, librbd::io::AIO_TYPE_GENERIC,
                                  get_aio_completion(c)));
    librbd::api::Mirror<>::image_get_global_status(
      ictx, &ctx->cpp_mirror_image_global_status, ctx);
    return 0;
  }

#pragma GCC diagnostic pop

  int Image::aio_mirror_image_create_snapshot(uint32_t flags, uint64_t *snap_id,
                                              RBD::AioCompletion *c) {
    ImageCtx *ictx = (ImageCtx *)ctx;

    librbd::api::Mirror<>::image_snapshot_create(
        ictx, flags, snap_id, new C_AioCompletion(ictx,
                                                  librbd::io::AIO_TYPE_GENERIC,
                                                  get_aio_completion(c)));
    return 0;
  }

  int Image::update_watch(UpdateWatchCtx *wctx, uint64_t *handle) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, update_watch_enter, ictx, wctx);
    int r = ictx->state->register_update_watcher(wctx, handle);
    tracepoint(librbd, update_watch_exit, r, *handle);
    return r;
  }

  int Image::update_unwatch(uint64_t handle) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, update_unwatch_enter, ictx, handle);
    int r = ictx->state->unregister_update_watcher(handle);
    tracepoint(librbd, update_unwatch_exit, r);
    return r;
  }

  int Image::list_watchers(std::list<librbd::image_watcher_t> &watchers) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    tracepoint(librbd, list_watchers_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only);
    int r = librbd::list_watchers(ictx, watchers);
#ifdef WITH_LTTNG
    if (r >= 0) {
      for (auto &watcher : watchers) {
	tracepoint(librbd, list_watchers_entry, watcher.addr.c_str(), watcher.id, watcher.cookie);
      }
    }
#endif
    tracepoint(librbd, list_watchers_exit, r, watchers.size());
    return r;
  }

  int Image::config_list(std::vector<config_option_t> *options) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    return librbd::api::Config<>::list(ictx, options);
  }

} // namespace librbd



extern "C" int rbd_open(rados_ioctx_t p, const char *name, rbd_image_t *image,
			const char *snap_name)
{
  // printf("librbd NULL implementation: in %s  \n",__func__);
	// return 0;
  printf("librbd NULL implementation: in %s  \n",__func__);
  librados::IoCtx io_ctx;
  librados::IoCtx::from_rados_ioctx_t(p, io_ctx);
  TracepointProvider::initialize<tracepoint_traits>(get_cct(io_ctx));
   printf("In file: %s, function %s, line %d\n", __FILE__, __func__, __LINE__);

  librbd::ImageCtx *ictx = new librbd::ImageCtx(name, "", snap_name, io_ctx,
						false);
  tracepoint(librbd, open_image_enter, ictx, ictx->name.c_str(), ictx->id.c_str(), ictx->snap_name.c_str(), ictx->read_only);
 printf("In file: %s, function %s, line %d\n", __FILE__, __func__, __LINE__);
  int r = ictx->state->open(0);
  printf("In file: %s, function %s, line %d\n", __FILE__, __func__, __LINE__);
  if (r >= 0) {
    *image = (rbd_image_t)ictx;
  }
  printf("In file: %s, function %s, line %d\n", __FILE__, __func__, __LINE__);
  tracepoint(librbd, open_image_exit, r);


  return r;
}

extern "C" int rbd_close(rbd_image_t image)
{
  printf("librbd NULL implementation: in %s\n",__func__);
  return 0;
}

extern "C" int rbd_stat(rbd_image_t image, rbd_image_info_t *info,
			size_t infosize)
{
  printf("librbd NULL implementation: in %s\n",__func__);
  return 0;
}


extern "C" int rbd_resize(rbd_image_t image, uint64_t size)
{
  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;
}

extern "C" int rbd_get_size(rbd_image_t image, uint64_t *size)
{

  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;}

extern "C" int rbd_create(rados_ioctx_t p, const char *name, uint64_t size, int *order)
{

  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;}


extern "C" int rbd_remove(rados_ioctx_t p, const char *name)
{

  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;}


extern "C" void rbd_aio_release(rbd_completion_t c)
{

}

extern "C" int rbd_aio_create_completion(void *cb_arg,
					 rbd_callback_t complete_cb,
					 rbd_completion_t *c) {

  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;}

extern "C" int rbd_aio_readv(rbd_image_t image, const struct iovec *iov,
                             int iovcnt, uint64_t off, rbd_completion_t c)
{
  printf("librbd NULL implementation: in %s\n",__func__);
  librbd::RBD::AioCompletion *comp = (librbd::RBD::AioCompletion *)c;
  printf("In file: %s, function %s, line %d\n", __FILE__, __func__, __LINE__);

  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;

  // librbd::io::AioCompletion* aio_comp = get_aio_completion(comp);
  // printf("librbd NULL 3: state is %d\n",aio_comp->state.load());
  // aio_comp->state.store(1);
  // printf("librbd NULL complete called %d\n",aio_comp->state.load());
  size_t len = 0;

  librbd::io::ReadResult read_result;
  printf("In file: %s, function %s, line %d\n", __FILE__, __func__, __LINE__);
  librbd::io::AioCompletion*  io_comp = get_aio_completion(comp);
  printf("In file: %s, function %s, line %d\n", __FILE__, __func__, __LINE__);

  io_comp->complete();
  // ictx->io_work_queue->aio_read(get_aio_completion(comp), off, len,
  //                                 std::move(read_result), 0);
  printf("librbd NULL 3: in %s\n",__func__);

  return 0;
}


extern "C" int rbd_aio_writev(rbd_image_t image, const struct iovec *iov,
                              int iovcnt, uint64_t off, rbd_completion_t c)
{
 
  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;}

extern "C" int rbd_aio_discard(rbd_image_t image, uint64_t off, uint64_t len,
			       rbd_completion_t c)
{

  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;}


extern "C" int rbd_aio_flush(rbd_image_t image, rbd_completion_t c)
{

  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;}


extern "C" int rbd_aio_write_zeroes(rbd_image_t image, uint64_t off, size_t len,
                                    rbd_completion_t c, int zero_flags,
                                    int op_flags)
{

  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;
}

extern "C" ssize_t rbd_aio_get_return_value(rbd_completion_t c)
{
  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;
}


extern "C" int rbd_invalidate_cache(rbd_image_t image)
{
  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;
}

/* snapshots */
extern "C" int rbd_snap_create(rbd_image_t image, const char *snap_name)
{
  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;
}

extern "C" int rbd_snap_remove(rbd_image_t image, const char *snap_name)
{
  printf("librbd NULL implementation: in %s\n",__func__);
  return 0;
}

extern "C" int rbd_snap_rollback(rbd_image_t image, const char *snap_name)
{
  printf("librbd NULL implementation: in %s\n",__func__);

  return 0;
}

extern "C" int rbd_snap_list(rbd_image_t image, rbd_snap_info_t *snaps,
			     int *max_snaps)
{
  printf("librbd NULL implementation: in %s\n",__func__);
   return 0;
}

extern "C" void rbd_snap_list_end(rbd_snap_info_t *snaps)
{
  printf("librbd NULL implementation: in %s\n",__func__);
}
