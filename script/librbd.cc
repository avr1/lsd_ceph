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

extern "C" int rbd_open(rados_ioctx_t p, const char *name, rbd_image_t *image,
			const char *snap_name)
{
  printf("librbd NULL implementation: in %s  \n",__func__);
	return 0;
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
  // librbd NULL implementation::ImageCtx *ictx = (librbd NULL implementation::ImageCtx *)image;
  // tracepoint(librbd, resize_enter, ictx, ictx->name.c_str(), ictx->snap_name.c_str(), ictx->read_only, size);
  // librbd NULL implementation::NoOpProgressContext prog_ctx;
  // int r = ictx->operations->resize(size, true, prog_ctx);
  // tracepoint(librbd, resize_exit, r);
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
