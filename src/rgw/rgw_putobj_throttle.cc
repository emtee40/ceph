// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#include "include/rados/librados.hpp"

#include "rgw_putobj_throttle.h"
#include "rgw_rados.h"

namespace rgw::putobj {

void AioThrottle::aio_cb(void *cb, void *arg)
{
  Pending& p = *static_cast<Pending*>(arg);
  p.result = p.completion->get_return_value();
  p.parent->put(p);
}

bool AioThrottle::waiter_ready() const
{
  switch (waiter) {
  case Wait::Available: return is_available();
  case Wait::Completion: return has_completion();
  case Wait::Drained: return is_drained();
  default: return false;
  }
}

ResultList AioThrottle::submit(rgw_rados_ref& ref, const rgw_raw_obj& obj,
                               librados::ObjectWriteOperation *op,
                               uint64_t cost)
{
  auto p = std::make_unique<Pending>();
  p->obj = obj;
  p->cost = cost;

  if (cost > window) {
    p->result = -EDEADLK; // would never succeed
    completed.push_back(*p);
  } else {
    get(*p);
    p->result = ref.ioctx.aio_operate(ref.oid, p->completion, op);
    if (p->result < 0) {
      put(*p);
    }
  }
  p.release();
  return std::move(completed);
}

ResultList AioThrottle::submit(rgw_rados_ref& ref, const rgw_raw_obj& obj,
                               librados::ObjectReadOperation *op,
                               bufferlist *data, uint64_t cost)
{
  auto p = std::make_unique<Pending>();
  p->obj = obj;
  p->cost = cost;

  if (cost > window) {
    p->result = -EDEADLK; // would never succeed
    completed.push_back(*p);
  } else {
    get(*p);
    p->result = ref.ioctx.aio_operate(ref.oid, p->completion, op, data);
    if (p->result < 0) {
      put(*p);
    }
  }
  p.release();
  return std::move(completed);
}

void AioThrottle::get(Pending& p)
{
  std::unique_lock lock{mutex};

  // wait for the write size to become available
  pending_size += p.cost;
  if (!is_available()) {
    ceph_assert(waiter == Wait::None);
    waiter = Wait::Available;
    cond.wait(lock, [this] { return is_available(); });
    waiter = Wait::None;
  }

  // register the pending write and attach a completion
  p.parent = this;
  p.completion = librados::Rados::aio_create_completion(&p, nullptr, aio_cb);
  pending.push_back(p);
}

void AioThrottle::put(Pending& p)
{
  p.completion->release();
  p.completion = nullptr;

  std::scoped_lock lock{mutex};

  // move from pending to completed
  pending.erase(pending.iterator_to(p));
  completed.push_back(p);

  pending_size -= p.cost;

  if (waiter_ready()) {
    cond.notify_one();
  }
}

ResultList AioThrottle::poll()
{
  std::unique_lock lock{mutex};
  return std::move(completed);
}

ResultList AioThrottle::wait()
{
  std::unique_lock lock{mutex};
  if (completed.empty() && !pending.empty()) {
    ceph_assert(waiter == Wait::None);
    waiter = Wait::Completion;
    cond.wait(lock, [this] { return has_completion(); });
    waiter = Wait::None;
  }
  return std::move(completed);
}

ResultList AioThrottle::drain()
{
  std::unique_lock lock{mutex};
  if (!pending.empty()) {
    ceph_assert(waiter == Wait::None);
    waiter = Wait::Drained;
    cond.wait(lock, [this] { return is_drained(); });
    waiter = Wait::None;
  }
  return std::move(completed);
}

} // namespace rgw::putobj
