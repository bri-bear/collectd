/**
 * collectd - src/rdtmon.c
 *
 * Copyright(c) 2016 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Serhiy Pshyk <serhiyx.pshyk@intel.com>
 **/

#include <pqos.h>

#include "common.h"

#define RDTMON_PLUGIN "rdtmon"

#define RDTMON_MAX_SOCKETS 8
#define RDTMON_MAX_SOCKET_CORES 64
#define RDTMON_MAX_CORES (RDTMON_MAX_SOCKET_CORES * RDTMON_MAX_SOCKETS)

struct rdtmon_core_group_s {
  char *desc;
  int num_cores;
  unsigned *cores;
  enum pqos_mon_event events;
};
typedef struct rdtmon_core_group_s rdtmon_core_group_t;

struct rdtmon_ctx_s {
  rdtmon_core_group_t cgroups[RDTMON_MAX_CORES];
  struct pqos_mon_data *pgroups[RDTMON_MAX_CORES];
  int num_groups;
  const struct pqos_cpuinfo *pqos_cpu;
  const struct pqos_cap *pqos_cap;
  const struct pqos_capability *cap_mon;
};
typedef struct rdtmon_ctx_s rdtmon_ctx_t;

static rdtmon_ctx_t *g_rdtmon = NULL;

static int isdup(const uint64_t *nums, unsigned size, uint64_t val) {
  for (unsigned i = 0; i < size; i++)
    if (nums[i] == val)
      return 1;
  return 0;
}

static int strtouint64(const char *s, uint64_t *n) {
  char *endptr = NULL;

  assert(s != NULL);
  assert(n != NULL);

  *n = strtoull(s, &endptr, 0);

  if (!(*s != '\0' && *endptr == '\0')) {
    DEBUG(RDTMON_PLUGIN ": Error converting '%s' to unsigned number.", s);
    return (-EINVAL);
  }

  return (0);
}

/*
 * NAME
 *   strlisttonums
 *
 * DESCRIPTION
 *   Converts string of characters representing list of numbers into array of
 *   numbers. Allowed formats are:
 *     0,1,2,3
 *     0-10,20-18
 *     1,3,5-8,10,0x10-12
 *
 *   Numbers can be in decimal or hexadecimal format.
 *
 * PARAMETERS
 *   `s'         String representing list of unsigned numbers.
 *   `nums'      Array to put converted numeric values into.
 *   `max'       Maximum number of elements that nums can accommodate.
 *
 * RETURN VALUE
 *    Number of elements placed into nums.
 */
static unsigned strlisttonums(char *s, uint64_t *nums, unsigned max) {
  int ret;
  unsigned index = 0;
  char *saveptr = NULL;

  if (s == NULL || nums == NULL || max == 0)
    return index;

  for (;;) {
    char *p = NULL;
    char *token = NULL;

    token = strtok_r(s, ",", &saveptr);
    if (token == NULL)
      break;

    s = NULL;

    while (isspace(*token))
      token++;
    if (*token == '\0')
      continue;

    p = strchr(token, '-');
    if (p != NULL) {
      uint64_t n, start, end;
      *p = '\0';
      ret = strtouint64(token, &start);
      if (ret < 0)
        return (0);
      ret = strtouint64(p + 1, &end);
      if (ret < 0)
        return (0);
      if (start > end) {
        n = start;
        start = end;
        end = n;
      }
      for (n = start; n <= end; n++) {
        if (!(isdup(nums, index, n))) {
          nums[index] = n;
          index++;
        }
        if (index >= max)
          return index;
      }
    } else {
      uint64_t val;

      ret = strtouint64(token, &val);
      if (ret < 0)
        return (0);

      if (!(isdup(nums, index, val))) {
        nums[index] = val;
        index++;
      }
      if (index >= max)
        return index;
    }
  }

  return index;
}

/*
 * NAME
 *   cgroup_cmp
 *
 * DESCRIPTION
 *   Function to compare cores in 2 core groups.
 *
 * PARAMETERS
 *   `cg_a'      Pointer to core group a.
 *   `cg_b'      Pointer to core group b.
 *
 * RETURN VALUE
 *    1 if both groups contain the same cores
 *    0 if none of their cores match
 *    -1 if some but not all cores match
 */
static int cgroup_cmp(const rdtmon_core_group_t *cg_a,
                      const rdtmon_core_group_t *cg_b) {
  int found = 0;

  assert(cg_a != NULL);
  assert(cg_b != NULL);

  const int sz_a = cg_a->num_cores;
  const int sz_b = cg_b->num_cores;
  const unsigned *tab_a = cg_a->cores;
  const unsigned *tab_b = cg_b->cores;

  for (int i = 0; i < sz_a; i++) {
    for (int j = 0; j < sz_b; j++)
      if (tab_a[i] == tab_b[j])
        found++;
  }
  /* if no cores are the same */
  if (!found)
    return 0;
  /* if group contains same cores */
  if (sz_a == sz_b && sz_b == found)
    return 1;
  /* if not all cores are the same */
  return -1;
}

static int cgroup_set(rdtmon_core_group_t *cg, char *desc, uint64_t *cores,
                      int num_cores) {
  assert(cg != NULL);
  assert(desc != NULL);
  assert(cores != NULL);
  assert(num_cores > 0);

  cg->cores = malloc(sizeof(unsigned) * num_cores);
  if (cg->cores == NULL) {
    ERROR(RDTMON_PLUGIN ": Error allocating core group table");
    return (-ENOMEM);
  }
  cg->num_cores = num_cores;
  cg->desc = desc;

  for (int i = 0; i < num_cores; i++)
    cg->cores[i] = (unsigned)cores[i];

  return 0;
}

/*
 * NAME
 *   oconfig_to_cgroups
 *
 * DESCRIPTION
 *   Function to set the descriptions and cores for each core group.
 *   Takes a config option containing list of strings that are used to set
 *   core group values.
 *
 * PARAMETERS
 *   `item'        Config option containing core groups.
 *   `groups'      Table of core groups to set values in.
 *   `max'         Maximum number of core groups allowed.
 *
 * RETURN VALUE
 *   On success, the number of core groups set up. On error, appropriate
 *   negative error value.
 */
static int oconfig_to_cgroups(oconfig_item_t *item, rdtmon_core_group_t *groups,
                              unsigned max) {
  int ret;
  unsigned n, index = 0;
  uint64_t cores[RDTMON_MAX_CORES];
  char value[DATA_MAX_NAME_LEN];

  assert(groups != NULL);
  assert(max > 0);
  assert(item != NULL);

  for (int j = 0; j < item->values_num; j++) {
    if (item->values[j].value.string != NULL &&
        strlen(item->values[j].value.string)) {
      char *desc = NULL;

      sstrncpy(value, item->values[j].value.string, sizeof(value));

      memset(cores, 0, sizeof(cores));

      n = strlisttonums(value, cores, RDTMON_MAX_CORES);
      if (n == 0) {
        ERROR(RDTMON_PLUGIN ": Error parsing core group (%s)", value);
        return (-EINVAL);
      }

      desc = strdup(item->values[j].value.string);

      /* set core group info */
      ret = cgroup_set(&groups[index], desc, cores, n);
      if (ret < 0) {
        free(desc);
        return ret;
      }

      index++;

      if (index >= max) {
        WARNING(RDTMON_PLUGIN ": Too many core groups configured");
        return index;
      }
    }
  }

  return index;
}

#if COLLECT_DEBUG
static void rdtmon_dump_cgroups(void) {
  char cores[RDTMON_MAX_CORES * 4];

  if (g_rdtmon == NULL)
    return;

  DEBUG(RDTMON_PLUGIN ": Core Groups Dump");
  DEBUG(RDTMON_PLUGIN ":  groups count: %d", g_rdtmon->num_groups);

  for (int i = 0; i < g_rdtmon->num_groups; i++) {

    memset(cores, 0, sizeof(cores));
    for (int j = 0; j < g_rdtmon->cgroups[i].num_cores; j++) {
      snprintf(cores + strlen(cores), sizeof(cores) - strlen(cores) - 1, " %d",
               g_rdtmon->cgroups[i].cores[j]);
    }

    DEBUG(RDTMON_PLUGIN ":  group[%d]:", i);
    DEBUG(RDTMON_PLUGIN ":    description: %s", g_rdtmon->cgroups[i].desc);
    DEBUG(RDTMON_PLUGIN ":    cores: %s", cores);
    DEBUG(RDTMON_PLUGIN ":    events: 0x%X", g_rdtmon->cgroups[i].events);
  }

  return;
}

static inline double bytes_to_kb(const double bytes) { return bytes / 1024.0; }

static inline double bytes_to_mb(const double bytes) {
  return bytes / (1024.0 * 1024.0);
}

static void rdtmon_dump_data(void) {
  /*
   * CORE - monitored group of cores
   * RMID - Resource Monitoring ID associated with the monitored group
   * LLC - last level cache occupancy
   * MBL - local memory bandwidth
   * MBR - remote memory bandwidth
   */
  DEBUG("  CORE     RMID    LLC[KB]   MBL[MB]    MBR[MB]");
  for (int i = 0; i < g_rdtmon->num_groups; i++) {

    const struct pqos_event_values *pv = &g_rdtmon->pgroups[i]->values;

    double llc = bytes_to_kb(pv->llc);
    double mbr = bytes_to_mb(pv->mbm_remote_delta);
    double mbl = bytes_to_mb(pv->mbm_local_delta);

    DEBUG(" [%s] %8u %10.1f %10.1f %10.1f", g_rdtmon->cgroups[i].desc,
          g_rdtmon->pgroups[i]->poll_ctx[0].rmid, llc, mbl, mbr);
  }
}
#endif /* COLLECT_DEBUG */

static void rdtmon_free_cgroups(void) {
  for (int i = 0; i < RDTMON_MAX_CORES; i++) {
    if (g_rdtmon->cgroups[i].desc) {
      sfree(g_rdtmon->cgroups[i].desc);
    }

    if (g_rdtmon->cgroups[i].cores) {
      sfree(g_rdtmon->cgroups[i].cores);
      g_rdtmon->cgroups[i].num_cores = 0;
    }

    if (g_rdtmon->pgroups[i]) {
      sfree(g_rdtmon->pgroups[i]);
    }
  }
}

static int rdtmon_default_cgroups(void) {
  int ret;

  /* configure each core in separate group */
  for (int i = 0; i < g_rdtmon->pqos_cpu->num_cores; i++) {
    char *desc;
    uint64_t core = i;

    desc = ssnprintf_alloc("%d", g_rdtmon->pqos_cpu->cores[i].lcore);
    if (desc == NULL)
      return (-ENOMEM);

    /* set core group info */
    ret = cgroup_set(&g_rdtmon->cgroups[i], desc, &core, 1);
    if (ret < 0) {
      free(desc);
      return ret;
    }
  }

  return g_rdtmon->pqos_cpu->num_cores;
}

static int rdtmon_config_cgroups(oconfig_item_t *item) {
  int n = 0;
  enum pqos_mon_event events = 0;

  if (item == NULL) {
    DEBUG(RDTMON_PLUGIN ": cgroups_config: Invalid argument.");
    return (-EINVAL);
  }

  DEBUG(RDTMON_PLUGIN ": Core groups [%d]:", item->values_num);
  for (int j = 0; j < item->values_num; j++) {
    if (item->values[j].type != OCONFIG_TYPE_STRING) {
      ERROR(RDTMON_PLUGIN ": given core group value is not a string [idx=%d]",
            j);
      return (-EINVAL);
    }
    DEBUG(RDTMON_PLUGIN ":  [%d]: %s", j, item->values[j].value.string);
  }

  n = oconfig_to_cgroups(item, g_rdtmon->cgroups, RDTMON_MAX_CORES);
  if (n < 0) {
    rdtmon_free_cgroups();
    ERROR(RDTMON_PLUGIN ": Error parsing core groups configuration.");
    return (-EINVAL);
  }

  if (n == 0) {
    /* create default core groups if "Cores" config option is empty */
    n = rdtmon_default_cgroups();
    if (n < 0) {
      rdtmon_free_cgroups();
      ERROR(RDTMON_PLUGIN
            ": Error creating default core groups configuration.");
      return n;
    }
    INFO(RDTMON_PLUGIN
         ": No core groups configured. Default core groups created.");
  }

  /* Get all available events on this platform */
  for (int i = 0; i < g_rdtmon->cap_mon->u.mon->num_events; i++)
    events |= g_rdtmon->cap_mon->u.mon->events[i].type;

  events &= ~(PQOS_PERF_EVENT_LLC_MISS);

  DEBUG(RDTMON_PLUGIN ": Available events to monitor [0x%X]", events);

  g_rdtmon->num_groups = n;
  for (int i = 0; i < n; i++) {
    int found = 0;

    for (int j = 0; j < i; j++) {
      found = cgroup_cmp(&g_rdtmon->cgroups[j], &g_rdtmon->cgroups[i]);
      if (found != 0) {
        rdtmon_free_cgroups();
        ERROR(RDTMON_PLUGIN ": Cannot monitor same cores in different groups.");
        return (-EINVAL);
      }
    }

    g_rdtmon->cgroups[i].events = events;
    g_rdtmon->pgroups[i] = malloc(sizeof(struct pqos_mon_data));
    if (g_rdtmon->pgroups[i] == NULL) {
      rdtmon_free_cgroups();
      ERROR(RDTMON_PLUGIN ": Failed to allocate memory for monitoring data.");
      return (-ENOMEM);
    }
  }

  return (0);
}

static int rdtmon_preinit(void) {
  struct pqos_config pqos_cfg;
  int ret;

  if (g_rdtmon != NULL) {
    /* already initialized if config callback was called before init callback */
    return (0);
  }

  g_rdtmon = malloc(sizeof(rdtmon_ctx_t));
  if (g_rdtmon == NULL) {
    ERROR(RDTMON_PLUGIN ": Failed to allocate memory for rdtmon context.");
    return (-ENOMEM);
  }

  memset(g_rdtmon, 0, sizeof(rdtmon_ctx_t));

  /* init PQoS library */
  memset(&pqos_cfg, 0, sizeof(pqos_cfg));
  /* TODO:
   * stdout should not be used here. Will be reworked when support of log
   * callback is added to PQoS library.
  */
  pqos_cfg.fd_log = STDOUT_FILENO;
  pqos_cfg.verbose = 0;

  /* In case previous instance of the application was not closed properly
   * call fini and ignore return code. */
  pqos_fini();

  ret = pqos_init(&pqos_cfg);
  if (ret != PQOS_RETVAL_OK) {
    ERROR(RDTMON_PLUGIN ": Error initializing PQoS library!");
    goto rdtmon_preinit_error1;
  }

  ret = pqos_cap_get(&g_rdtmon->pqos_cap, &g_rdtmon->pqos_cpu);
  if (ret != PQOS_RETVAL_OK) {
    ERROR(RDTMON_PLUGIN ": Error retrieving PQoS capabilities.");
    goto rdtmon_preinit_error2;
  }

  ret = pqos_cap_get_type(g_rdtmon->pqos_cap, PQOS_CAP_TYPE_MON,
                          &g_rdtmon->cap_mon);
  if (ret == PQOS_RETVAL_PARAM) {
    ERROR(RDTMON_PLUGIN ": Error retrieving monitoring capabilities.");
    goto rdtmon_preinit_error2;
  }

  if (g_rdtmon->cap_mon == NULL) {
    ERROR(
        RDTMON_PLUGIN
        ": Monitoring capability not detected. Nothing to do for the plugin.");
    goto rdtmon_preinit_error2;
  }

  return (0);

rdtmon_preinit_error2:
  pqos_fini();

rdtmon_preinit_error1:

  sfree(g_rdtmon);

  return (-1);
}

static int rdtmon_config(oconfig_item_t *ci) {
  int ret = 0;

  ret = rdtmon_preinit();
  if (ret != 0)
    return ret;

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Cores", child->key) == 0) {

      ret = rdtmon_config_cgroups(child);
      if (ret != 0)
        return ret;

#if COLLECT_DEBUG
      rdtmon_dump_cgroups();
#endif /* COLLECT_DEBUG */

    } else {
      ERROR(RDTMON_PLUGIN ": Unknown configuration parameter \"%s\".",
            child->key);
    }
  }

  return (0);
}

static void rdtmon_submit_gauge(char *cgroup, char *type, gauge_t value) {
  value_t values[1];
  value_list_t vl = VALUE_LIST_INIT;

  values[0].gauge = value;

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);

  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, RDTMON_PLUGIN, sizeof(vl.plugin));
  snprintf(vl.plugin_instance, sizeof(vl.plugin_instance), "[%s]", cgroup);
  sstrncpy(vl.type, type, sizeof(vl.type));

  plugin_dispatch_values(&vl);
}

static void rdtmon_submit_mbm(char *cgroup,
                              const struct pqos_event_values *pv) {
  value_t values[6];
  value_list_t vl = VALUE_LIST_INIT;

  values[0].gauge = pv->mbm_local;
  values[1].gauge = pv->mbm_remote;
  values[2].gauge = pv->mbm_total;
  values[3].gauge = pv->mbm_local_delta;
  values[4].gauge = pv->mbm_remote_delta;
  values[5].gauge = pv->mbm_total_delta;

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);

  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, RDTMON_PLUGIN, sizeof(vl.plugin));
  snprintf(vl.plugin_instance, sizeof(vl.plugin_instance), "[%s]", cgroup);
  sstrncpy(vl.type, "mbm", sizeof(vl.type));

  plugin_dispatch_values(&vl);
}

static int rdtmon_read(user_data_t *ud) {
  int ret;

  if (g_rdtmon == NULL) {
    ERROR(RDTMON_PLUGIN ": rdtmon_read: plugin not initialized.");
    return (-EINVAL);
  }

  ret = pqos_mon_poll(&g_rdtmon->pgroups[0], (unsigned)g_rdtmon->num_groups);
  if (ret != PQOS_RETVAL_OK) {
    ERROR(RDTMON_PLUGIN ": Failed to poll monitoring data.");
    return (-1);
  }

#if COLLECT_DEBUG
  rdtmon_dump_data();
#endif /* COLLECT_DEBUG */

  for (int i = 0; i < g_rdtmon->num_groups; i++) {
    enum pqos_mon_event mbm_events =
        (PQOS_MON_EVENT_LMEM_BW | PQOS_MON_EVENT_TMEM_BW |
         PQOS_MON_EVENT_RMEM_BW);

    const struct pqos_event_values *pv = &g_rdtmon->pgroups[i]->values;

    /* Submit only monitored events data */

    if (g_rdtmon->cgroups[i].events & PQOS_MON_EVENT_L3_OCCUP)
      rdtmon_submit_gauge(g_rdtmon->cgroups[i].desc, "llc", pv->llc);

    if (g_rdtmon->cgroups[i].events & PQOS_PERF_EVENT_IPC)
      rdtmon_submit_gauge(g_rdtmon->cgroups[i].desc, "ipc", pv->ipc);

    if (g_rdtmon->cgroups[i].events & mbm_events)
      rdtmon_submit_mbm(g_rdtmon->cgroups[i].desc, pv);
  }

  return (0);
}

static int rdtmon_init(void) {
  int ret;

  ret = rdtmon_preinit();
  if (ret != 0)
    return ret;

  /* Start monitoring */
  for (int i = 0; i < g_rdtmon->num_groups; i++) {
    rdtmon_core_group_t *cg = &g_rdtmon->cgroups[i];

    ret = pqos_mon_start(cg->num_cores, cg->cores, cg->events, (void *)cg->desc,
                         g_rdtmon->pgroups[i]);

    if (ret != PQOS_RETVAL_OK) {
      ERROR(RDTMON_PLUGIN ": Error starting monitoring (pqos status=%d)", ret);
      return (-1);
    }
  }

  return (0);
}

static int rdtmon_shutdown(void) {
  int ret;

  DEBUG(RDTMON_PLUGIN ": rdtmon_shutdown.");

  if (g_rdtmon == NULL) {
    ERROR(RDTMON_PLUGIN ": rdtmon_shutdown: plugin not initialized.");
    return (-EINVAL);
  }

  /* Stop monitoring */
  for (int i = 0; i < g_rdtmon->num_groups; i++) {
    pqos_mon_stop(g_rdtmon->pgroups[i]);
  }

  ret = pqos_fini();
  if (ret != PQOS_RETVAL_OK)
    ERROR(RDTMON_PLUGIN ": Error shutting down PQoS library.");

  rdtmon_free_cgroups();
  sfree(g_rdtmon);

  return (0);
}

void module_register(void) {
  plugin_register_init(RDTMON_PLUGIN, rdtmon_init);
  plugin_register_complex_config(RDTMON_PLUGIN, rdtmon_config);
  plugin_register_complex_read(NULL, RDTMON_PLUGIN, rdtmon_read, 0, NULL);
  plugin_register_shutdown(RDTMON_PLUGIN, rdtmon_shutdown);
}
