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

#include "src/core/client_config/resolvers/dns_resolver.h"

#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/string_util.h>

#include "src/core/client_config/lb_policies/pick_first.h"
#include "src/core/iomgr/resolve_address.h"
#include "src/core/support/string.h"

typedef struct {
  /** base class: must be first */
  grpc_resolver base;
  /** refcount */
  gpr_refcount refs;
  /** name to resolve */
  char *name;
  /** default port to use */
  char *default_port;
  /** subchannel factory */
  grpc_subchannel_factory *subchannel_factory;
  /** load balancing policy factory */
  grpc_lb_policy *(*lb_policy_factory)(grpc_subchannel **subchannels,
                                       size_t num_subchannels);

  /** mutex guarding the rest of the state */
  gpr_mu mu;
  /** are we currently resolving? */
  int resolving;
  /** which version of resolved_config have we published? */
  int published_version;
  /** which version of resolved_config is current? */
  int resolved_version;
  /** pending next completion, or NULL */
  grpc_iomgr_closure *next_completion;
  /** target config address for next completion */
  grpc_client_config **target_config;
  /** current (fully resolved) config */
  grpc_client_config *resolved_config;
} dns_resolver;

static void dns_destroy(dns_resolver *r);

static void dns_start_resolving_locked(dns_resolver *r);
static void dns_maybe_finish_next_locked(dns_resolver *r);

static void dns_ref(grpc_resolver *r);
static void dns_unref(grpc_resolver *r);
static void dns_shutdown(grpc_resolver *r);
static void dns_channel_saw_error(grpc_resolver *r,
                                  struct sockaddr *failing_address,
                                  int failing_address_len);
static void dns_next(grpc_resolver *r, grpc_client_config **target_config,
                     grpc_iomgr_closure *on_complete);

static const grpc_resolver_vtable dns_resolver_vtable = {
    dns_ref, dns_unref, dns_shutdown, dns_channel_saw_error, dns_next};

static void dns_ref(grpc_resolver *resolver) {
  dns_resolver *r = (dns_resolver *)resolver;
  gpr_ref(&r->refs);
}

static void dns_unref(grpc_resolver *resolver) {
  dns_resolver *r = (dns_resolver *)resolver;
  if (gpr_unref(&r->refs)) {
    dns_destroy(r);
  }
}

static void dns_shutdown(grpc_resolver *resolver) {
  dns_resolver *r = (dns_resolver *)resolver;
  gpr_mu_lock(&r->mu);
  if (r->next_completion != NULL) {
    *r->target_config = NULL;
    /* TODO(ctiller): add delayed callback */
    grpc_iomgr_add_callback(r->next_completion);
    r->next_completion = NULL;
  }
  gpr_mu_unlock(&r->mu);
}

static void dns_channel_saw_error(grpc_resolver *resolver, struct sockaddr *sa,
                                  int len) {
  dns_resolver *r = (dns_resolver *)resolver;
  gpr_mu_lock(&r->mu);
  if (!r->resolving) {
    dns_start_resolving_locked(r);
  }
  gpr_mu_unlock(&r->mu);
}

static void dns_next(grpc_resolver *resolver,
                     grpc_client_config **target_config,
                     grpc_iomgr_closure *on_complete) {
  dns_resolver *r = (dns_resolver *)resolver;
  gpr_mu_lock(&r->mu);
  GPR_ASSERT(!r->next_completion);
  r->next_completion = on_complete;
  r->target_config = target_config;
  if (r->resolved_version == 0 && !r->resolving) {
    dns_start_resolving_locked(r);
  } else {
    dns_maybe_finish_next_locked(r);
  }
  gpr_mu_unlock(&r->mu);
}

static void dns_on_resolved(void *arg, grpc_resolved_addresses *addresses) {
  dns_resolver *r = arg;
  grpc_client_config *config = NULL;
  grpc_subchannel **subchannels;
  grpc_subchannel_args args;
  grpc_lb_policy *lb_policy;
  size_t i;
  if (addresses) {
    config = grpc_client_config_create();
    subchannels = gpr_malloc(sizeof(grpc_subchannel *) * addresses->naddrs);
    for (i = 0; i < addresses->naddrs; i++) {
      memset(&args, 0, sizeof(args));
      args.addr = (struct sockaddr *)(addresses->addrs[i].addr);
      args.addr_len = addresses->addrs[i].len;
      subchannels[i] = grpc_subchannel_factory_create_subchannel(
          r->subchannel_factory, &args);
    }
    lb_policy = r->lb_policy_factory(subchannels, addresses->naddrs);
    grpc_client_config_set_lb_policy(config, lb_policy);
    GRPC_LB_POLICY_UNREF(lb_policy, "construction");
  }
  gpr_mu_lock(&r->mu);
  if (r->resolved_config) {
    grpc_client_config_unref(r->resolved_config);
  }
  r->resolved_config = config;
  r->resolved_version++;
  dns_maybe_finish_next_locked(r);
  gpr_mu_unlock(&r->mu);

  dns_unref(&r->base);
}

static void dns_start_resolving_locked(dns_resolver *r) {
  dns_ref(&r->base);
  r->resolving = 1;
  grpc_resolve_address(r->name, r->default_port, dns_on_resolved, r);
}

static void dns_maybe_finish_next_locked(dns_resolver *r) {
  if (r->next_completion != NULL &&
      r->resolved_version != r->published_version) {
    *r->target_config = r->resolved_config;
    grpc_client_config_ref(r->resolved_config);
    grpc_iomgr_add_callback(r->next_completion);
    r->next_completion = NULL;
    r->published_version = r->resolved_version;
  }
}

static void dns_destroy(dns_resolver *r) {
  gpr_mu_destroy(&r->mu);
  if (r->resolved_config) {
    grpc_client_config_unref(r->resolved_config);
  }
  grpc_subchannel_factory_unref(r->subchannel_factory);
  gpr_free(r->name);
  gpr_free(r->default_port);
  gpr_free(r);
}

static grpc_resolver *dns_create(
    grpc_uri *uri, const char *default_port,
    grpc_lb_policy *(*lb_policy_factory)(grpc_subchannel **subchannels,
                                         size_t num_subchannels),
    grpc_subchannel_factory *subchannel_factory) {
  dns_resolver *r;
  const char *path = uri->path;

  if (0 != strcmp(uri->authority, "")) {
    gpr_log(GPR_ERROR, "authority based uri's not supported");
    return NULL;
  }

  if (path[0] == '/') ++path;

  r = gpr_malloc(sizeof(dns_resolver));
  memset(r, 0, sizeof(*r));
  gpr_ref_init(&r->refs, 1);
  gpr_mu_init(&r->mu);
  r->base.vtable = &dns_resolver_vtable;
  r->name = gpr_strdup(path);
  r->default_port = gpr_strdup(default_port);
  r->subchannel_factory = subchannel_factory;
  r->lb_policy_factory = lb_policy_factory;
  grpc_subchannel_factory_ref(subchannel_factory);
  return &r->base;
}

/*
 * FACTORY
 */

static void dns_factory_ref(grpc_resolver_factory *factory) {}

static void dns_factory_unref(grpc_resolver_factory *factory) {}

static grpc_resolver *dns_factory_create_resolver(
    grpc_resolver_factory *factory, grpc_uri *uri,
    grpc_subchannel_factory *subchannel_factory) {
  return dns_create(uri, "https", grpc_create_pick_first_lb_policy,
                    subchannel_factory);
}

static const grpc_resolver_factory_vtable dns_factory_vtable = {
    dns_factory_ref, dns_factory_unref, dns_factory_create_resolver};
static grpc_resolver_factory dns_resolver_factory = {&dns_factory_vtable};

grpc_resolver_factory *grpc_dns_resolver_factory_create() {
  return &dns_resolver_factory;
}
