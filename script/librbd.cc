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
#include "librbd/Utils.h"
#include "librbd/api/Config.h"
#include "librbd/api/DiffIterate.h"
#include "librbd/api/Group.h"
#include "librbd/api/Image.h"
#include "librbd/api/Io.h"
#include "librbd/api/Migration.h"
#include "librbd/api/Mirror.h"
#include "librbd/api/Namespace.h"
#include "librbd/api/Pool.h"
#include "librbd/api/PoolMetadata.h"
#include "librbd/api/Snapshot.h"
#include "librbd/api/Trash.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/ReadResult.h"
#include <algorithm>
#include <string>
#include <vector>

extern "C" int rbd_open(rados_ioctx_t p, const char *name, rbd_image_t *image,
			const char *snap_name)
{
	return 0;
}

extern "C" int rbd_close(rbd_image_t image)
{
  return 0;
}
