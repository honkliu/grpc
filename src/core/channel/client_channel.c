/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/channel/client_channel.h"

#include <stdio.h>
#include <string.h>

#include "src/core/channel/channel_args.h"
#include "src/core/channel/connected_channel.h"
#include "src/core/channel/connectivity_state.h"
#include "src/core/iomgr/iomgr.h"
#include "src/core/iomgr/pollset_set.h"
#include "src/core/support/string.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>
#include <grpc/support/useful.h>

/* Client channel implementation */

typedef struct call_data call_data;

typedef struct {
  /** metadata context for this channel */
  grpc_mdctx *mdctx;
  /** resolver for this channel */
  grpc_resolver *resolver;

  /** mutex protecting client configuration, resolution state */
  gpr_mu mu_config;
  /** currently active load balancer - guarded by mu_config */
  grpc_lb_policy *lb_policy;
  /** incoming configuration - set by resolver.next
      guarded by mu_config */
  grpc_client_config *incoming_configuration;
  /** a list of closures that are all waiting for config to come in */
  grpc_iomgr_closure *waiting_for_config_closures;
  /** resolver callback */
  grpc_iomgr_closure on_config_changed;
  /** connectivity state being tracked */
  grpc_connectivity_state_tracker state_tracker;
} channel_data;

typedef enum {
  CALL_CREATED,
  CALL_WAITING_FOR_CONFIG,
  CALL_WAITING_FOR_PICK,
  CALL_WAITING_FOR_CALL,
  CALL_ACTIVE,
  CALL_CANCELLED
} call_state;

struct call_data {
  /* owning element */
  grpc_call_element *elem;

  gpr_mu mu_state;

  call_state state;
  gpr_timespec deadline;
  grpc_subchannel *picked_channel;
  grpc_iomgr_closure async_setup_task;
  grpc_transport_stream_op waiting_op;
  /* our child call stack */
  grpc_subchannel_call *subchannel_call;
  grpc_linked_mdelem status;
  grpc_linked_mdelem details;
};

static void handle_op_after_cancellation(grpc_call_element *elem,
                                         grpc_transport_stream_op *op) {
  call_data *calld = elem->call_data;
  channel_data *chand = elem->channel_data;
  if (op->send_ops) {
    grpc_stream_ops_unref_owned_objects(op->send_ops->ops, op->send_ops->nops);
    op->on_done_send->cb(op->on_done_send->cb_arg, 0);
  }
  if (op->recv_ops) {
    char status[GPR_LTOA_MIN_BUFSIZE];
    grpc_metadata_batch mdb;
    gpr_ltoa(GRPC_STATUS_CANCELLED, status);
    calld->status.md =
        grpc_mdelem_from_strings(chand->mdctx, "grpc-status", status);
    calld->details.md =
        grpc_mdelem_from_strings(chand->mdctx, "grpc-message", "Cancelled");
    calld->status.prev = calld->details.next = NULL;
    calld->status.next = &calld->details;
    calld->details.prev = &calld->status;
    mdb.list.head = &calld->status;
    mdb.list.tail = &calld->details;
    mdb.garbage.head = mdb.garbage.tail = NULL;
    mdb.deadline = gpr_inf_future;
    grpc_sopb_add_metadata(op->recv_ops, mdb);
    *op->recv_state = GRPC_STREAM_CLOSED;
    op->on_done_recv->cb(op->on_done_recv->cb_arg, 1);
  }
  if (op->on_consumed) {
    op->on_consumed->cb(op->on_consumed->cb_arg, 0);
  }
}

typedef struct {
  grpc_iomgr_closure closure;
  grpc_call_element *elem;
} waiting_call;

static void perform_transport_stream_op(grpc_call_element *elem, grpc_transport_stream_op *op, int continuation);

static void continue_with_pick(void *arg, int iomgr_success) {
  waiting_call *wc = arg;
  call_data *calld = wc->elem->call_data;
  perform_transport_stream_op(wc->elem, &calld->waiting_op, 1);
  gpr_free(wc);
}

static void add_to_lb_policy_wait_queue_locked_state_config(grpc_call_element *elem) {
  channel_data *chand = elem->channel_data;
  waiting_call *wc = gpr_malloc(sizeof(*wc));
  grpc_iomgr_closure_init(&wc->closure, continue_with_pick, wc);
  wc->elem = elem;
  wc->closure.next = chand->waiting_for_config_closures;
  chand->waiting_for_config_closures = &wc->closure;
}

static int is_empty(void *p, int len) {
  char *ptr = p;
  int i;
  for (i = 0; i < len; i++) {
    if (ptr[i] != 0) return 0;
  }
  return 1;
}

static void started_call(void *arg, int iomgr_success) {
  call_data *calld = arg;
  grpc_transport_stream_op op;
  int have_waiting;

  gpr_mu_lock(&calld->mu_state);
  if (calld->state == CALL_CANCELLED && calld->subchannel_call != NULL) {
    memset(&op, 0, sizeof(op));
    op.cancel_with_status = GRPC_STATUS_CANCELLED;
    gpr_mu_unlock(&calld->mu_state);
    grpc_subchannel_call_process_op(calld->subchannel_call, &op);
  } else if (calld->state == CALL_WAITING_FOR_CALL) {
    have_waiting = !is_empty(&calld->waiting_op, sizeof(calld->waiting_op));
    if (calld->subchannel_call != NULL) {
      calld->state = CALL_ACTIVE;
      gpr_mu_unlock(&calld->mu_state);
      if (have_waiting) {
        grpc_subchannel_call_process_op(calld->subchannel_call, &calld->waiting_op);
      }
    } else {
      calld->state = CALL_CANCELLED;
      gpr_mu_unlock(&calld->mu_state);
      if (have_waiting) {
        handle_op_after_cancellation(calld->elem, &calld->waiting_op);
      }
    }
  } else {
    GPR_ASSERT(calld->state == CALL_CANCELLED);
  }
}

static void picked_target(void *arg, int iomgr_success) {
  call_data *calld = arg;
  grpc_transport_stream_op op;

  if (calld->picked_channel == NULL) {
    /* treat this like a cancellation */
    calld->waiting_op.cancel_with_status = GRPC_STATUS_UNAVAILABLE;
    perform_transport_stream_op(calld->elem, &calld->waiting_op, 1);
  } else {
    gpr_mu_lock(&calld->mu_state);
    if (calld->state == CALL_CANCELLED) {
      gpr_mu_unlock(&calld->mu_state);
      handle_op_after_cancellation(calld->elem, &calld->waiting_op);
    } else {
      GPR_ASSERT(calld->state == CALL_WAITING_FOR_PICK);
      calld->state = CALL_WAITING_FOR_CALL;
      op = calld->waiting_op;
      memset(&calld->waiting_op, 0, sizeof(calld->waiting_op));
      gpr_mu_unlock(&calld->mu_state);
      grpc_iomgr_closure_init(&calld->async_setup_task, started_call, calld);
      grpc_subchannel_create_call(calld->picked_channel, &op,
                                  &calld->subchannel_call,
                                  &calld->async_setup_task);
    }
  }
}

static void pick_target(grpc_lb_policy *lb_policy, call_data *calld) {
  grpc_metadata_batch *initial_metadata;
  grpc_transport_stream_op *op = &calld->waiting_op;

  GPR_ASSERT(op->bind_pollset);
  GPR_ASSERT(op->send_ops);
  GPR_ASSERT(op->send_ops->nops >= 1);
  GPR_ASSERT(op->send_ops->ops[0].type == GRPC_OP_METADATA);
  initial_metadata = &op->send_ops->ops[0].data.metadata;

  grpc_iomgr_closure_init(&calld->async_setup_task, picked_target, calld);
  grpc_lb_policy_pick(lb_policy, op->bind_pollset, 
    initial_metadata, &calld->picked_channel, &calld->async_setup_task);
}

static void perform_transport_stream_op(grpc_call_element *elem, grpc_transport_stream_op *op, int continuation) {
  call_data *calld = elem->call_data;
  channel_data *chand = elem->channel_data;
  grpc_subchannel_call *subchannel_call;
  grpc_lb_policy *lb_policy;
  GPR_ASSERT(elem->filter == &grpc_client_channel_filter);
  GRPC_CALL_LOG_OP(GPR_INFO, elem, op);

  gpr_mu_lock(&calld->mu_state);
  switch (calld->state) {
    case CALL_ACTIVE:
      GPR_ASSERT(!continuation);
      subchannel_call = calld->subchannel_call;
      gpr_mu_unlock(&calld->mu_state);
      grpc_subchannel_call_process_op(subchannel_call, op);
      break;
    case CALL_CANCELLED:
      gpr_mu_unlock(&calld->mu_state);
      handle_op_after_cancellation(elem, op);
      break;
    case CALL_WAITING_FOR_CONFIG:
    case CALL_WAITING_FOR_PICK:
    case CALL_WAITING_FOR_CALL:
      if (!continuation) {
        if (op->cancel_with_status != GRPC_STATUS_OK) {
          calld->state = CALL_CANCELLED;
          gpr_mu_unlock(&calld->mu_state);
          handle_op_after_cancellation(elem, op);
        } else {
          GPR_ASSERT((calld->waiting_op.send_ops == NULL) !=
                     (op->send_ops == NULL));
          GPR_ASSERT((calld->waiting_op.recv_ops == NULL) !=
                     (op->recv_ops == NULL));
          if (op->send_ops != NULL) {
            calld->waiting_op.send_ops = op->send_ops;
            calld->waiting_op.is_last_send = op->is_last_send;
            calld->waiting_op.on_done_send = op->on_done_send;
          }
          if (op->recv_ops != NULL) {
            calld->waiting_op.recv_ops = op->recv_ops;
            calld->waiting_op.recv_state = op->recv_state;
            calld->waiting_op.on_done_recv = op->on_done_recv;
          }
          gpr_mu_unlock(&calld->mu_state);
          if (op->on_consumed != NULL) {
            op->on_consumed->cb(op->on_consumed->cb_arg, 0);
          }
        }
        break;
      }
      /* fall through */
    case CALL_CREATED:
      if (op->cancel_with_status != GRPC_STATUS_OK) {
        calld->state = CALL_CANCELLED;
        gpr_mu_unlock(&calld->mu_state);
        handle_op_after_cancellation(elem, op);
      } else {
        calld->waiting_op = *op;

        gpr_mu_lock(&chand->mu_config);
        lb_policy = chand->lb_policy;
        if (lb_policy) {
          GRPC_LB_POLICY_REF(lb_policy, "pick");
          gpr_mu_unlock(&chand->mu_config);
          calld->state = CALL_WAITING_FOR_PICK;
          gpr_mu_unlock(&calld->mu_state);

          pick_target(lb_policy, calld);

          GRPC_LB_POLICY_UNREF(lb_policy, "pick");
        } else {
          calld->state = CALL_WAITING_FOR_CONFIG;
          add_to_lb_policy_wait_queue_locked_state_config(elem);
          gpr_mu_unlock(&chand->mu_config);
          gpr_mu_unlock(&calld->mu_state);
        }
      }
      break;
  }
}

static void cc_start_transport_stream_op(grpc_call_element *elem,
                                  grpc_transport_stream_op *op) {
  perform_transport_stream_op(elem, op, 0);
}

static void update_state_locked(channel_data *chand) {
  gpr_log(GPR_ERROR, "update_state_locked not implemented");
}

static void cc_on_config_changed(void *arg, int iomgr_success) {
  channel_data *chand = arg;
  grpc_lb_policy *lb_policy = NULL;
  grpc_lb_policy *old_lb_policy;
  grpc_resolver *old_resolver;
  grpc_iomgr_closure *wakeup_closures = NULL;

  if (chand->incoming_configuration != NULL) {
    lb_policy = grpc_client_config_get_lb_policy(chand->incoming_configuration);
    GRPC_LB_POLICY_REF(lb_policy, "channel");

    grpc_client_config_unref(chand->incoming_configuration);
  }

  chand->incoming_configuration = NULL;

  gpr_mu_lock(&chand->mu_config);
  old_lb_policy = chand->lb_policy;
  chand->lb_policy = lb_policy;
  if (lb_policy != NULL) {
    wakeup_closures = chand->waiting_for_config_closures;
    chand->waiting_for_config_closures = NULL;
  }
  gpr_mu_unlock(&chand->mu_config);

  while (wakeup_closures) {
    grpc_iomgr_closure *next = wakeup_closures->next;
    grpc_iomgr_add_callback(wakeup_closures);
    wakeup_closures = next;
  }

  if (old_lb_policy) {
    GRPC_LB_POLICY_UNREF(old_lb_policy, "channel");
  }

  if (iomgr_success) {
    grpc_resolver_next(chand->resolver, &chand->incoming_configuration, &chand->on_config_changed);
  } else {
    gpr_mu_lock(&chand->mu_config);
    old_resolver = chand->resolver;
    chand->resolver = NULL;
    update_state_locked(chand);
    gpr_mu_unlock(&chand->mu_config);
    grpc_resolver_unref(old_resolver);
  }
}

static void cc_start_transport_op(grpc_channel_element *elem, grpc_transport_op *op) {
  grpc_lb_policy *lb_policy = NULL;
  channel_data *chand = elem->channel_data;
  grpc_iomgr_closure *on_consumed = op->on_consumed;
  op->on_consumed = NULL;

  GPR_ASSERT(op->set_accept_stream == NULL);
  GPR_ASSERT(op->bind_pollset == NULL);

  gpr_mu_lock(&chand->mu_config);
  if (op->on_connectivity_state_change != NULL) {
    grpc_connectivity_state_notify_on_state_change(&chand->state_tracker, op->connectivity_state, op->on_connectivity_state_change);
    op->on_connectivity_state_change = NULL;
    op->connectivity_state = NULL;
  }

  if (!is_empty(op, sizeof(*op))) {
    lb_policy = chand->lb_policy;
    if (lb_policy) {
      GRPC_LB_POLICY_REF(lb_policy, "broadcast");
    }
  }
  gpr_mu_unlock(&chand->mu_config);

  if (lb_policy) {
    grpc_lb_policy_broadcast(lb_policy, op);
    GRPC_LB_POLICY_UNREF(lb_policy, "broadcast");
  }

  if (on_consumed) {
    grpc_iomgr_add_callback(on_consumed);
  }
}

/* Constructor for call_data */
static void init_call_elem(grpc_call_element *elem,
                           const void *server_transport_data,
                           grpc_transport_stream_op *initial_op) {
  call_data *calld = elem->call_data;

  /* TODO(ctiller): is there something useful we can do here? */
  GPR_ASSERT(initial_op == NULL);

  GPR_ASSERT(elem->filter == &grpc_client_channel_filter);
  GPR_ASSERT(server_transport_data == NULL);
  gpr_mu_init(&calld->mu_state);
  calld->elem = elem;
  calld->state = CALL_CREATED;
  calld->deadline = gpr_inf_future;
}

/* Destructor for call_data */
static void destroy_call_elem(grpc_call_element *elem) {
  call_data *calld = elem->call_data;
  grpc_subchannel_call *subchannel_call;

  /* if the call got activated, we need to destroy the child stack also, and
     remove it from the in-flight requests tracked by the child_entry we
     picked */
  gpr_mu_lock(&calld->mu_state);
  switch (calld->state) {
    case CALL_ACTIVE:
      subchannel_call = calld->subchannel_call;
      gpr_mu_unlock(&calld->mu_state);
      grpc_subchannel_call_unref(subchannel_call);
      break;
    case CALL_CREATED:
    case CALL_CANCELLED:
      gpr_mu_unlock(&calld->mu_state);
      break;
    case CALL_WAITING_FOR_PICK:
    case CALL_WAITING_FOR_CONFIG:
    case CALL_WAITING_FOR_CALL:
      gpr_log(GPR_ERROR, "should never reach here");
      abort();
      break;
  }
}

/* Constructor for channel_data */
static void init_channel_elem(grpc_channel_element *elem,
                              const grpc_channel_args *args,
                              grpc_mdctx *metadata_context, int is_first,
                              int is_last) {
  channel_data *chand = elem->channel_data;

  memset(chand, 0, sizeof(*chand));

  GPR_ASSERT(is_last);
  GPR_ASSERT(elem->filter == &grpc_client_channel_filter);

  gpr_mu_init(&chand->mu_config);
  chand->mdctx = metadata_context;
  grpc_iomgr_closure_init(&chand->on_config_changed, cc_on_config_changed, chand);
}

/* Destructor for channel_data */
static void destroy_channel_elem(grpc_channel_element *elem) {
  channel_data *chand = elem->channel_data;

  if (chand->resolver != NULL) {
    grpc_resolver_unref(chand->resolver);
  }
  if (chand->lb_policy != NULL) {
    GRPC_LB_POLICY_UNREF(chand->lb_policy, "channel");
  }
  gpr_mu_destroy(&chand->mu_config);
}

const grpc_channel_filter grpc_client_channel_filter = {
    cc_start_transport_stream_op, cc_start_transport_op,           sizeof(call_data),
    init_call_elem,        destroy_call_elem,    sizeof(channel_data),
    init_channel_elem,     destroy_channel_elem, "client-channel",
};

void grpc_client_channel_set_resolver(grpc_channel_stack *channel_stack,
                                      grpc_resolver *resolver) {
  /* post construction initialization: set the transport setup pointer */
  grpc_channel_element *elem = grpc_channel_stack_last_element(channel_stack);
  channel_data *chand = elem->channel_data;
  GPR_ASSERT(!chand->resolver);
  chand->resolver = resolver;
  grpc_resolver_ref(resolver);
  grpc_resolver_next(resolver, &chand->incoming_configuration, &chand->on_config_changed);
}
