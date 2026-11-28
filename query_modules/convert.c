// Copyright 2026 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mg_procedure.h"

#define INITIAL_CAPACITY 16
#define SMALL_MERGE_BUCKETS 64
#define HASH_TABLE_MIN_CAPACITY 8

static const char *kProcedureToTree = "to_tree";

static const char *kParameterPaths = "paths";
static const char *kParameterConfig = "config";
static const char *kParameterLowerCaseRels = "lowerCaseRels";

static const char *kReturnValue = "value";

typedef enum { FILTER_MODE_INCLUDE, FILTER_MODE_EXCLUDE, FILTER_MODE_WILDCARD, FILTER_MODE_INVALID } filter_mode_t;

typedef struct {
  filter_mode_t mode;
  char **properties;
  size_t property_count;
  size_t property_cap;
} property_filter_t;

typedef struct {
  size_t filter_count;
  size_t filter_cap;
  property_filter_t *filters;
  char **labels;
} node_property_filter_config_t;

typedef struct {
  size_t filter_count;
  size_t filter_cap;
  property_filter_t *filters;
  char **rel_types;
} rel_property_filter_config_t;

typedef struct {
  node_property_filter_config_t node_config;
  rel_property_filter_config_t rel_config;
  bool has_node_config;
  bool has_rel_config;
} filter_config_t;

typedef struct {
  int64_t node_id;
  int64_t edge_id;
  struct mgp_map *map;
  bool used;
} bucket_t;

typedef struct {
  int64_t node_id;
  int64_t edge_id;
  bool valid;
} merge_identity_t;

static size_t safe_strlen(const char *str) { return str ? strlen(str) : 0; }

static bool alloc_bytes(struct mgp_memory *memory, size_t size, void **out_ptr) {
  if (!memory || !out_ptr) {
    return false;
  }
  size_t alloc_size = size == 0 ? 1 : size;
  if (alloc_size > SIZE_MAX - 63ULL) {
    return false;
  }
  alloc_size = (alloc_size + 63ULL) & ~63ULL;
  void *ptr = NULL;
  if (mgp_alloc(memory, alloc_size, &ptr) != MGP_ERROR_NO_ERROR || ptr == NULL) {
    return false;
  }
  *out_ptr = ptr;
  return true;
}

static char *dup_cstr(const char *src, struct mgp_memory *memory) {
  if (!src) {
    return NULL;
  }
  size_t len = safe_strlen(src);
  if (len == SIZE_MAX) {
    return NULL;
  }
  void *ptr = NULL;
  if (!alloc_bytes(memory, len + 1, &ptr)) {
    return NULL;
  }
  char *copy = (char *)ptr;
  memcpy(copy, src, len + 1);
  return copy;
}

static char *dup_cstr_strip_dash(const char *src, struct mgp_memory *memory) {
  if (!src || src[0] == '\0') {
    return NULL;
  }
  const char *start = (src[0] == '-') ? (src + 1) : src;
  return dup_cstr(start, memory);
}

static char *to_lowercase(const char *str, struct mgp_memory *memory) {
  if (!str) {
    return NULL;
  }
  size_t len = safe_strlen(str);
  if (len == SIZE_MAX) {
    return NULL;
  }
  void *ptr = NULL;
  if (!alloc_bytes(memory, len + 1, &ptr)) {
    return NULL;
  }
  char *lower = (char *)ptr;
  for (size_t i = 0; i < len; i++) {
    lower[i] = (char)tolower((unsigned char)str[i]);
  }
  lower[len] = '\0';
  return lower;
}

static char *join_strings(const char *str1, const char *sep, const char *str2, struct mgp_memory *memory) {
  if (!str1 || !sep || !str2) {
    return NULL;
  }
  size_t len1 = safe_strlen(str1);
  size_t len_sep = safe_strlen(sep);
  size_t len2 = safe_strlen(str2);
  if (len1 > SIZE_MAX - len_sep || (len1 + len_sep) > SIZE_MAX - len2 - 1) {
    return NULL;
  }
  size_t total = len1 + len_sep + len2 + 1;
  void *ptr = NULL;
  if (!alloc_bytes(memory, total, &ptr)) {
    return NULL;
  }
  char *out = (char *)ptr;
  memcpy(out, str1, len1);
  memcpy(out + len1, sep, len_sep);
  memcpy(out + len1 + len_sep, str2, len2 + 1);
  return out;
}

/* Inserts/replaces `key` in `map` by erasing any prior mapping first, then moving `*val`. */
static bool map_put_move_borrowed(struct mgp_map *map, const char *key, struct mgp_value *val) {
  if (!map || !key || !val) {
    return false;
  }
  if (mgp_map_erase(map, key) != MGP_ERROR_NO_ERROR) {
    return false;
  }
  return mgp_map_insert_move(map, key, val) == MGP_ERROR_NO_ERROR;
}

/* Inserts/replaces `key` in `map` by moving `*val`, then always destroys the standalone `val` wrapper. */
static bool map_put_move_owned(struct mgp_map *map, const char *key, struct mgp_value *val) {
  if (!val) {
    return false;
  }
  bool ok = map_put_move_borrowed(map, key, val);
  mgp_value_destroy(val);
  return ok;
}

/* Appends `*val` into `list` by moving its payload, then always destroys the standalone `val` wrapper. */
static bool list_append_move_owned(struct mgp_list *list, struct mgp_value *val) {
  if (!list || !val) {
    if (val) {
      mgp_value_destroy(val);
    }
    return false;
  }
  enum mgp_error err = mgp_list_append_move(list, val);
  mgp_value_destroy(val);
  return err == MGP_ERROR_NO_ERROR;
}

static inline uint64_t hash_child_identity(int64_t node_id, int64_t edge_id) {
  uint64_t h = 14695981039346656037ULL;
  h ^= (uint64_t)node_id;
  h *= 1099511628211ULL;
  h ^= (uint64_t)edge_id;
  h *= 1099511628211ULL;
  return h;
}

static inline ssize_t probe_child_bucket(bucket_t *tbl, size_t cap, int64_t node_id, int64_t edge_id, uint64_t h) {
  size_t mask = cap - 1;
  size_t pos = (size_t)h & mask;
  for (size_t probes = 0; probes < cap; ++probes) {
    if (!tbl[pos].used) {
      return (ssize_t)pos;
    }
    if (tbl[pos].node_id == node_id && tbl[pos].edge_id == edge_id) {
      return (ssize_t)pos;
    }
    pos = (pos + 1) & mask;
  }
  return -1;
}

static bool property_in_list(const char *prop_name, char *const *properties, size_t count) {
  if (!prop_name) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (properties[i] && strcmp(prop_name, properties[i]) == 0) {
      return true;
    }
  }
  return false;
}

static void init_property_filter(property_filter_t *filter) {
  filter->mode = FILTER_MODE_INVALID;
  filter->properties = NULL;
  filter->property_count = 0;
  filter->property_cap = 0;
}

static void destroy_property_filter(property_filter_t *filter, struct mgp_memory *memory) {
  if (!filter || !memory) {
    return;
  }
  if (filter->properties) {
    for (size_t i = 0; i < filter->property_count; i++) {
      if (filter->properties[i]) {
        mgp_free(memory, filter->properties[i]);
      }
    }
    mgp_free(memory, filter->properties);
  }
  init_property_filter(filter);
}

static bool append_filter_property(property_filter_t *filter, char *owned_name, struct mgp_memory *memory) {
  if (!owned_name) {
    return false;
  }
  if (filter->property_count >= filter->property_cap) {
    size_t next_cap = filter->property_cap == 0 ? INITIAL_CAPACITY : filter->property_cap * 2;
    if (next_cap < filter->property_cap || next_cap > SIZE_MAX / sizeof(char *)) {
      mgp_free(memory, owned_name);
      return false;
    }
    void *new_buf = NULL;
    if (!alloc_bytes(memory, next_cap * sizeof(char *), &new_buf)) {
      mgp_free(memory, owned_name);
      return false;
    }
    if (filter->properties) {
      memcpy(new_buf, filter->properties, filter->property_count * sizeof(char *));
      mgp_free(memory, filter->properties);
    }
    filter->properties = (char **)new_buf;
    filter->property_cap = next_cap;
  }
  filter->properties[filter->property_count++] = owned_name;
  return true;
}

static bool parse_property_filter(property_filter_t *filter, struct mgp_list *props, struct mgp_memory *memory) {
  init_property_filter(filter);

  size_t list_size = 0;
  if (mgp_list_size(props, &list_size) != MGP_ERROR_NO_ERROR) {
    return false;
  }
  if (list_size == 0) {
    return true;
  }

  for (size_t i = 0; i < list_size; i++) {
    struct mgp_value *item = NULL;
    if (mgp_list_at(props, i, &item) != MGP_ERROR_NO_ERROR || item == NULL) {
      return false;
    }
    enum mgp_value_type item_type = MGP_VALUE_TYPE_NULL;
    if (mgp_value_get_type(item, &item_type) != MGP_ERROR_NO_ERROR) {
      return false;
    }
    if (item_type != MGP_VALUE_TYPE_STRING) {
      /* Preserve invalid-filter semantics (mode stays FILTER_MODE_INVALID). */
      return true;
    }
  }

  struct mgp_value *first_item = NULL;
  if (mgp_list_at(props, 0, &first_item) != MGP_ERROR_NO_ERROR || first_item == NULL) {
    return false;
  }
  const char *first_str = NULL;
  if (mgp_value_get_string(first_item, &first_str) != MGP_ERROR_NO_ERROR || first_str == NULL) {
    return false;
  }

  if (strcmp(first_str, "*") == 0) {
    filter->mode = FILTER_MODE_WILDCARD;
    return true;
  }

  bool is_exclude = (first_str[0] == '-');
  filter->mode = is_exclude ? FILTER_MODE_EXCLUDE : FILTER_MODE_INCLUDE;

  for (size_t i = 0; i < list_size; i++) {
    struct mgp_value *item = NULL;
    if (mgp_list_at(props, i, &item) != MGP_ERROR_NO_ERROR || item == NULL) {
      destroy_property_filter(filter, memory);
      return false;
    }
    const char *val_str = NULL;
    if (mgp_value_get_string(item, &val_str) != MGP_ERROR_NO_ERROR || val_str == NULL) {
      destroy_property_filter(filter, memory);
      return false;
    }
    if (is_exclude && (val_str[0] == '\0' || (val_str[0] == '-' && val_str[1] == '\0'))) {
      continue;
    }
    char *copied = is_exclude ? dup_cstr_strip_dash(val_str, memory) : dup_cstr(val_str, memory);
    if (!copied || !append_filter_property(filter, copied, memory)) {
      destroy_property_filter(filter, memory);
      return false;
    }
  }
  return true;
}

static const property_filter_t *get_node_property_filter(const node_property_filter_config_t *config,
                                                         const char *label) {
  if (!config || !label) {
    return NULL;
  }
  for (size_t i = 0; i < config->filter_count; i++) {
    if (config->labels[i] && strcmp(config->labels[i], label) == 0) {
      return &config->filters[i];
    }
  }
  return NULL;
}

static bool should_include_node_property(const char *prop_name, const filter_config_t *filter_config,
                                         struct mgp_vertex *node, bool *out_ok) {
  *out_ok = true;
  if (!filter_config || !filter_config->has_node_config || !node) {
    return true;
  }

  size_t label_count = 0;
  if (mgp_vertex_labels_count(node, &label_count) != MGP_ERROR_NO_ERROR) {
    *out_ok = false;
    return false;
  }

  size_t valid_filter_count = 0;
  bool has_invalid_filter = false;
  bool has_exclude_filter = false;
  bool has_wildcard_filter = false;
  bool included_by_any = false;

  for (size_t label_idx = 0; label_idx < label_count; label_idx++) {
    struct mgp_label label = {.name = NULL};
    if (mgp_vertex_label_at(node, label_idx, &label) != MGP_ERROR_NO_ERROR || !label.name) {
      *out_ok = false;
      return false;
    }

    const property_filter_t *filter = get_node_property_filter(&filter_config->node_config, label.name);
    if (!filter) {
      continue;
    }
    if (filter->mode == FILTER_MODE_INVALID) {
      has_invalid_filter = true;
      continue;
    }
    valid_filter_count++;
    if (filter->mode == FILTER_MODE_EXCLUDE) {
      has_exclude_filter = true;
      if (property_in_list(prop_name, filter->properties, filter->property_count)) {
        return false;
      }
    } else if (filter->mode == FILTER_MODE_WILDCARD) {
      has_wildcard_filter = true;
    } else if (filter->mode == FILTER_MODE_INCLUDE) {
      if (property_in_list(prop_name, filter->properties, filter->property_count)) {
        included_by_any = true;
      }
    }
  }

  if (has_invalid_filter && valid_filter_count == 0) {
    return false;
  }
  if (valid_filter_count == 0) {
    return true;
  }
  if (has_exclude_filter) {
    return true;
  }
  if (has_wildcard_filter) {
    return true;
  }
  return included_by_any;
}

static void init_node_property_filter_config(node_property_filter_config_t *config) {
  config->filter_count = 0;
  config->filter_cap = 0;
  config->filters = NULL;
  config->labels = NULL;
}

static void destroy_node_property_filter_config(node_property_filter_config_t *config, struct mgp_memory *memory) {
  if (!config || !memory) {
    return;
  }
  for (size_t i = 0; i < config->filter_count; i++) {
    destroy_property_filter(&config->filters[i], memory);
    if (config->labels && config->labels[i]) {
      mgp_free(memory, config->labels[i]);
    }
  }
  if (config->filters) {
    mgp_free(memory, config->filters);
  }
  if (config->labels) {
    mgp_free(memory, config->labels);
  }
  init_node_property_filter_config(config);
}

static void init_rel_property_filter_config(rel_property_filter_config_t *config) {
  config->filter_count = 0;
  config->filter_cap = 0;
  config->filters = NULL;
  config->rel_types = NULL;
}

static void destroy_rel_property_filter_config(rel_property_filter_config_t *config, struct mgp_memory *memory) {
  if (!config || !memory) {
    return;
  }
  for (size_t i = 0; i < config->filter_count; i++) {
    destroy_property_filter(&config->filters[i], memory);
    if (config->rel_types && config->rel_types[i]) {
      mgp_free(memory, config->rel_types[i]);
    }
  }
  if (config->filters) {
    mgp_free(memory, config->filters);
  }
  if (config->rel_types) {
    mgp_free(memory, config->rel_types);
  }
  init_rel_property_filter_config(config);
}

static void init_filter_config(filter_config_t *config) {
  init_node_property_filter_config(&config->node_config);
  init_rel_property_filter_config(&config->rel_config);
  config->has_node_config = false;
  config->has_rel_config = false;
}

static void destroy_filter_config(filter_config_t *config, struct mgp_memory *memory) {
  if (!config || !memory) {
    return;
  }
  destroy_node_property_filter_config(&config->node_config, memory);
  destroy_rel_property_filter_config(&config->rel_config, memory);
  config->has_node_config = false;
  config->has_rel_config = false;
}

static bool ensure_filter_table_capacity(size_t count, size_t *cap, property_filter_t **filters, char ***names,
                                         struct mgp_memory *memory) {
  if (count < *cap) {
    return true;
  }
  size_t next_cap = (*cap == 0) ? INITIAL_CAPACITY : (*cap * 2);
  if (next_cap < *cap || next_cap > SIZE_MAX / sizeof(property_filter_t) || next_cap > SIZE_MAX / sizeof(char *)) {
    return false;
  }
  void *new_filters = NULL;
  void *new_names = NULL;
  if (!alloc_bytes(memory, next_cap * sizeof(property_filter_t), &new_filters)) {
    return false;
  }
  if (!alloc_bytes(memory, next_cap * sizeof(char *), &new_names)) {
    mgp_free(memory, new_filters);
    return false;
  }
  if (*filters) {
    memcpy(new_filters, *filters, count * sizeof(property_filter_t));
    mgp_free(memory, *filters);
  }
  if (*names) {
    memcpy(new_names, *names, count * sizeof(char *));
    mgp_free(memory, *names);
  }
  *filters = (property_filter_t *)new_filters;
  *names = (char **)new_names;
  *cap = next_cap;
  return true;
}

static bool parse_named_filter_map(struct mgp_map *filter_map, size_t *out_count, size_t *out_cap,
                                   property_filter_t **out_filters, char ***out_names, struct mgp_memory *memory) {
  if (!filter_map) {
    return true;
  }
  size_t map_sz = 0;
  if (mgp_map_size(filter_map, &map_sz) != MGP_ERROR_NO_ERROR) {
    return false;
  }
  if (map_sz == 0) {
    return true;
  }

  struct mgp_map_items_iterator *iter = NULL;
  if (mgp_map_iter_items(filter_map, memory, &iter) != MGP_ERROR_NO_ERROR || iter == NULL) {
    return false;
  }

  struct mgp_map_item *item = NULL;
  enum mgp_error iter_err = mgp_map_items_iterator_get(iter, &item);
  while (iter_err == MGP_ERROR_NO_ERROR && item != NULL) {
    const char *key_str = NULL;
    if (mgp_map_item_key(item, &key_str) != MGP_ERROR_NO_ERROR || key_str == NULL) {
      mgp_map_items_iterator_destroy(iter);
      return false;
    }

    struct mgp_value *value = NULL;
    if (mgp_map_item_value(item, &value) != MGP_ERROR_NO_ERROR || value == NULL) {
      mgp_map_items_iterator_destroy(iter);
      return false;
    }

    enum mgp_value_type value_type = MGP_VALUE_TYPE_NULL;
    if (mgp_value_get_type(value, &value_type) != MGP_ERROR_NO_ERROR) {
      mgp_map_items_iterator_destroy(iter);
      return false;
    }

    if (value_type == MGP_VALUE_TYPE_LIST) {
      struct mgp_list *props_list = NULL;
      if (mgp_value_get_list(value, &props_list) != MGP_ERROR_NO_ERROR || props_list == NULL) {
        mgp_map_items_iterator_destroy(iter);
        return false;
      }
      if (!ensure_filter_table_capacity(*out_count, out_cap, out_filters, out_names, memory)) {
        mgp_map_items_iterator_destroy(iter);
        return false;
      }
      char *copied_key = dup_cstr(key_str, memory);
      if (!copied_key) {
        mgp_map_items_iterator_destroy(iter);
        return false;
      }
      if (!parse_property_filter(&(*out_filters)[*out_count], props_list, memory)) {
        mgp_free(memory, copied_key);
        mgp_map_items_iterator_destroy(iter);
        return false;
      }
      (*out_names)[*out_count] = copied_key;
      (*out_count)++;
    }

    iter_err = mgp_map_items_iterator_next(iter, &item);
  }
  mgp_map_items_iterator_destroy(iter);
  return iter_err == MGP_ERROR_NO_ERROR;
}

static bool parse_filter_config(filter_config_t *filter_config, struct mgp_map *config, struct mgp_memory *memory) {
  init_filter_config(filter_config);
  if (!config) {
    return true;
  }

  struct mgp_value *nodes_value = NULL;
  if (mgp_map_at(config, "nodes", &nodes_value) != MGP_ERROR_NO_ERROR) {
    return false;
  }
  if (nodes_value != NULL) {
    enum mgp_value_type nodes_type = MGP_VALUE_TYPE_NULL;
    if (mgp_value_get_type(nodes_value, &nodes_type) != MGP_ERROR_NO_ERROR) {
      return false;
    }
    if (nodes_type == MGP_VALUE_TYPE_MAP) {
      struct mgp_map *nodes_map = NULL;
      if (mgp_value_get_map(nodes_value, &nodes_map) != MGP_ERROR_NO_ERROR || nodes_map == NULL) {
        return false;
      }
      if (!parse_named_filter_map(nodes_map, &filter_config->node_config.filter_count,
                                  &filter_config->node_config.filter_cap, &filter_config->node_config.filters,
                                  &filter_config->node_config.labels, memory)) {
        destroy_filter_config(filter_config, memory);
        return false;
      }
      filter_config->has_node_config = true;
    }
  }

  struct mgp_value *rels_value = NULL;
  if (mgp_map_at(config, "rels", &rels_value) != MGP_ERROR_NO_ERROR) {
    destroy_filter_config(filter_config, memory);
    return false;
  }
  if (rels_value != NULL) {
    enum mgp_value_type rels_type = MGP_VALUE_TYPE_NULL;
    if (mgp_value_get_type(rels_value, &rels_type) != MGP_ERROR_NO_ERROR) {
      destroy_filter_config(filter_config, memory);
      return false;
    }
    if (rels_type == MGP_VALUE_TYPE_MAP) {
      struct mgp_map *rels_map = NULL;
      if (mgp_value_get_map(rels_value, &rels_map) != MGP_ERROR_NO_ERROR || rels_map == NULL) {
        destroy_filter_config(filter_config, memory);
        return false;
      }
      if (!parse_named_filter_map(rels_map, &filter_config->rel_config.filter_count,
                                  &filter_config->rel_config.filter_cap, &filter_config->rel_config.filters,
                                  &filter_config->rel_config.rel_types, memory)) {
        destroy_filter_config(filter_config, memory);
        return false;
      }
      filter_config->has_rel_config = true;
    }
  }

  return true;
}

static const property_filter_t *get_rel_property_filter(const rel_property_filter_config_t *config,
                                                        const char *rel_type) {
  if (!config || !rel_type) {
    return NULL;
  }
  for (size_t i = 0; i < config->filter_count; i++) {
    if (config->rel_types[i] && strcmp(config->rel_types[i], rel_type) == 0) {
      return &config->filters[i];
    }
  }
  return NULL;
}

static bool should_include_rel_property(const char *prop_name, const filter_config_t *filter_config,
                                        const char *rel_type) {
  if (!filter_config || !filter_config->has_rel_config) {
    return true;
  }
  const property_filter_t *filter = get_rel_property_filter(&filter_config->rel_config, rel_type);
  if (!filter) {
    return true;
  }
  switch (filter->mode) {
    case FILTER_MODE_WILDCARD:
      return true;
    case FILTER_MODE_EXCLUDE:
      return !property_in_list(prop_name, filter->properties, filter->property_count);
    case FILTER_MODE_INCLUDE:
      return property_in_list(prop_name, filter->properties, filter->property_count);
    default:
      return false;
  }
}

static bool merge_trees(struct mgp_map *target, struct mgp_map *source, struct mgp_memory *memory);

static inline merge_identity_t extract_child_identity(struct mgp_map *map, const char *rel_id_key) {
  merge_identity_t out = {.node_id = 0, .edge_id = 0, .valid = false};
  if (!map || !rel_id_key) {
    return out;
  }

  struct mgp_value *id_v = NULL;
  struct mgp_value *edge_id_v = NULL;
  if (mgp_map_at(map, "_id", &id_v) != MGP_ERROR_NO_ERROR || id_v == NULL) {
    return out;
  }
  if (mgp_map_at(map, rel_id_key, &edge_id_v) != MGP_ERROR_NO_ERROR || edge_id_v == NULL) {
    return out;
  }

  enum mgp_value_type t_id = MGP_VALUE_TYPE_NULL;
  enum mgp_value_type t_edge = MGP_VALUE_TYPE_NULL;
  if (mgp_value_get_type(id_v, &t_id) != MGP_ERROR_NO_ERROR || t_id != MGP_VALUE_TYPE_INT) {
    return out;
  }
  if (mgp_value_get_type(edge_id_v, &t_edge) != MGP_ERROR_NO_ERROR || t_edge != MGP_VALUE_TYPE_INT) {
    return out;
  }

  int64_t node_id = 0;
  int64_t edge_id = 0;
  if (mgp_value_get_int(id_v, &node_id) != MGP_ERROR_NO_ERROR) {
    return out;
  }
  if (mgp_value_get_int(edge_id_v, &edge_id) != MGP_ERROR_NO_ERROR) {
    return out;
  }

  out.node_id = node_id;
  out.edge_id = edge_id;
  out.valid = true;
  return out;
}

/* Returns true iff `val` is a non-empty list of child subtrees for relationship `rel_key`. */
static bool is_rel_child_list_value(struct mgp_value *val, const char *rel_id_key, bool *out_ok) {
  *out_ok = true;
  if (!val || !rel_id_key) {
    return false;
  }
  enum mgp_value_type vt = MGP_VALUE_TYPE_NULL;
  if (mgp_value_get_type(val, &vt) != MGP_ERROR_NO_ERROR) {
    *out_ok = false;
    return false;
  }
  if (vt != MGP_VALUE_TYPE_LIST) {
    return false;
  }
  struct mgp_list *lst = NULL;
  if (mgp_value_get_list(val, &lst) != MGP_ERROR_NO_ERROR || !lst) {
    *out_ok = false;
    return false;
  }
  size_t sz = 0;
  if (mgp_list_size(lst, &sz) != MGP_ERROR_NO_ERROR) {
    *out_ok = false;
    return false;
  }
  if (sz == 0) {
    return false;
  }
  for (size_t i = 0; i < sz; ++i) {
    struct mgp_value *elem = NULL;
    if (mgp_list_at(lst, i, &elem) != MGP_ERROR_NO_ERROR || !elem) {
      *out_ok = false;
      return false;
    }
    enum mgp_value_type et = MGP_VALUE_TYPE_NULL;
    if (mgp_value_get_type(elem, &et) != MGP_ERROR_NO_ERROR) {
      *out_ok = false;
      return false;
    }
    if (et != MGP_VALUE_TYPE_MAP) {
      return false;
    }
    struct mgp_map *em = NULL;
    if (mgp_value_get_map(elem, &em) != MGP_ERROR_NO_ERROR || !em) {
      *out_ok = false;
      return false;
    }
    merge_identity_t cid = extract_child_identity(em, rel_id_key);
    if (!cid.valid) {
      return false;
    }
  }
  return true;
}

static bool index_existing_children(struct mgp_list *existing_list, const char *rel_id_key, bucket_t *tbl, size_t cap) {
  size_t n = 0;
  if (mgp_list_size(existing_list, &n) != MGP_ERROR_NO_ERROR) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    struct mgp_value *item_v = NULL;
    if (mgp_list_at(existing_list, i, &item_v) != MGP_ERROR_NO_ERROR || !item_v) {
      return false;
    }
    struct mgp_map *m = NULL;
    if (mgp_value_get_map(item_v, &m) != MGP_ERROR_NO_ERROR || !m) {
      return false;
    }
    merge_identity_t id = extract_child_identity(m, rel_id_key);
    if (!id.valid) {
      continue;
    }
    uint64_t h = hash_child_identity(id.node_id, id.edge_id);
    ssize_t pos = probe_child_bucket(tbl, cap, id.node_id, id.edge_id, h);
    if (pos < 0) {
      return false;
    }
    if (!tbl[pos].used) {
      tbl[pos].used = true;
      tbl[pos].node_id = id.node_id;
      tbl[pos].edge_id = id.edge_id;
      tbl[pos].map = m;
    }
  }
  return true;
}

static bool merge_child_lists_hashed(struct mgp_list *existing_list, struct mgp_list *new_list, const char *rel_id_key,
                                     struct mgp_memory *memory) {
  size_t exist_n = 0;
  size_t new_n = 0;
  if (mgp_list_size(existing_list, &exist_n) != MGP_ERROR_NO_ERROR) {
    return false;
  }
  if (mgp_list_size(new_list, &new_n) != MGP_ERROR_NO_ERROR) {
    return false;
  }
  if (new_n == 0) {
    return true;
  }
  if (exist_n > SIZE_MAX - new_n) {
    return false;
  }
  size_t total_possible = exist_n + new_n;
  if (mgp_list_reserve(existing_list, total_possible) != MGP_ERROR_NO_ERROR) {
    return false;
  }

  size_t cap = HASH_TABLE_MIN_CAPACITY;
  if (total_possible > (SIZE_MAX >> 2)) {
    return false;
  }
  while (cap < (total_possible << 1)) {
    cap <<= 1;
  }

  bucket_t stack_tbl[SMALL_MERGE_BUCKETS];
  bucket_t *tbl = NULL;
  bool heap_tbl = false;
  if (cap <= SMALL_MERGE_BUCKETS) {
    cap = SMALL_MERGE_BUCKETS;
    memset(stack_tbl, 0, sizeof(stack_tbl));
    tbl = stack_tbl;
  } else {
    if (cap > SIZE_MAX / sizeof(bucket_t)) {
      return false;
    }
    void *ptr = NULL;
    if (!alloc_bytes(memory, cap * sizeof(bucket_t), &ptr)) {
      return false;
    }
    tbl = (bucket_t *)ptr;
    memset(tbl, 0, cap * sizeof(bucket_t));
    heap_tbl = true;
  }

  if (!index_existing_children(existing_list, rel_id_key, tbl, cap)) {
    if (heap_tbl) {
      mgp_free(memory, tbl);
    }
    return false;
  }

  for (size_t i = 0; i < new_n; ++i) {
    struct mgp_value *new_item = NULL;
    if (mgp_list_at(new_list, i, &new_item) != MGP_ERROR_NO_ERROR || !new_item) {
      if (heap_tbl) {
        mgp_free(memory, tbl);
      }
      return false;
    }
    struct mgp_map *new_map = NULL;
    if (mgp_value_get_map(new_item, &new_map) != MGP_ERROR_NO_ERROR || !new_map) {
      if (heap_tbl) {
        mgp_free(memory, tbl);
      }
      return false;
    }
    merge_identity_t nid = extract_child_identity(new_map, rel_id_key);
    if (!nid.valid) {
      if (heap_tbl) {
        mgp_free(memory, tbl);
      }
      return false;
    }

    uint64_t h = hash_child_identity(nid.node_id, nid.edge_id);
    ssize_t pos = probe_child_bucket(tbl, cap, nid.node_id, nid.edge_id, h);
    if (pos < 0) {
      if (heap_tbl) {
        mgp_free(memory, tbl);
      }
      return false;
    }

    if (tbl[pos].used) {
      if (!merge_trees(tbl[pos].map, new_map, memory)) {
        if (heap_tbl) {
          mgp_free(memory, tbl);
        }
        return false;
      }
    } else {
      if (mgp_list_append_move(existing_list, new_item) != MGP_ERROR_NO_ERROR) {
        if (heap_tbl) {
          mgp_free(memory, tbl);
        }
        return false;
      }
      size_t cur_sz = 0;
      struct mgp_value *appended_val = NULL;
      struct mgp_map *appended_map = NULL;
      if (mgp_list_size(existing_list, &cur_sz) != MGP_ERROR_NO_ERROR || cur_sz == 0 ||
          mgp_list_at(existing_list, cur_sz - 1, &appended_val) != MGP_ERROR_NO_ERROR || !appended_val ||
          mgp_value_get_map(appended_val, &appended_map) != MGP_ERROR_NO_ERROR || !appended_map) {
        if (heap_tbl) {
          mgp_free(memory, tbl);
        }
        return false;
      }
      tbl[pos].used = true;
      tbl[pos].node_id = nid.node_id;
      tbl[pos].edge_id = nid.edge_id;
      tbl[pos].map = appended_map;
    }
  }

  if (heap_tbl) {
    mgp_free(memory, tbl);
  }
  return true;
}

static bool merge_trees(struct mgp_map *target, struct mgp_map *source, struct mgp_memory *memory) {
  if (!target || !source || !memory) {
    return false;
  }
  size_t source_sz = 0;
  if (mgp_map_size(source, &source_sz) != MGP_ERROR_NO_ERROR) {
    return false;
  }
  if (source_sz == 0) {
    return true;
  }

  struct mgp_map_items_iterator *iter = NULL;
  if (mgp_map_iter_items(source, memory, &iter) != MGP_ERROR_NO_ERROR || !iter) {
    return false;
  }

  struct mgp_map_item *item = NULL;
  enum mgp_error iter_err = mgp_map_items_iterator_get(iter, &item);
  while (iter_err == MGP_ERROR_NO_ERROR && item != NULL) {
    const char *key_str = NULL;
    if (mgp_map_item_key(item, &key_str) != MGP_ERROR_NO_ERROR || !key_str) {
      mgp_map_items_iterator_destroy(iter);
      return false;
    }

    if (strcmp(key_str, "_type") == 0 || strcmp(key_str, "_id") == 0) {
      iter_err = mgp_map_items_iterator_next(iter, &item);
      continue;
    }

    struct mgp_value *src_val = NULL;
    if (mgp_map_item_value(item, &src_val) != MGP_ERROR_NO_ERROR || !src_val) {
      mgp_map_items_iterator_destroy(iter);
      return false;
    }

    struct mgp_value *dst_val = NULL;
    if (mgp_map_at(target, key_str, &dst_val) != MGP_ERROR_NO_ERROR) {
      mgp_map_items_iterator_destroy(iter);
      return false;
    }

    if (dst_val != NULL) {
      char *rel_id_key = join_strings(key_str, ".", "_id", memory);
      if (!rel_id_key) {
        mgp_map_items_iterator_destroy(iter);
        return false;
      }
      bool ok_dst = true;
      bool ok_src = true;
      bool dst_is_rel = is_rel_child_list_value(dst_val, rel_id_key, &ok_dst);
      bool src_is_rel = is_rel_child_list_value(src_val, rel_id_key, &ok_src);
      if (!ok_dst || !ok_src) {
        mgp_free(memory, rel_id_key);
        mgp_map_items_iterator_destroy(iter);
        return false;
      }

      if (dst_is_rel && src_is_rel) {
        struct mgp_list *dst_list = NULL;
        struct mgp_list *src_list = NULL;
        if (mgp_value_get_list(dst_val, &dst_list) != MGP_ERROR_NO_ERROR ||
            mgp_value_get_list(src_val, &src_list) != MGP_ERROR_NO_ERROR || !dst_list || !src_list ||
            !merge_child_lists_hashed(dst_list, src_list, rel_id_key, memory)) {
          mgp_free(memory, rel_id_key);
          mgp_map_items_iterator_destroy(iter);
          return false;
        }
      } else if (!dst_is_rel && src_is_rel) {
        /* Relationship child list takes precedence over a colliding scalar/list node property. */
        if (!map_put_move_borrowed(target, key_str, src_val)) {
          mgp_free(memory, rel_id_key);
          mgp_map_items_iterator_destroy(iter);
          return false;
        }
      }
      mgp_free(memory, rel_id_key);
    } else {
      if (mgp_map_insert_move(target, key_str, src_val) != MGP_ERROR_NO_ERROR) {
        mgp_map_items_iterator_destroy(iter);
        return false;
      }
    }

    iter_err = mgp_map_items_iterator_next(iter, &item);
  }

  mgp_map_items_iterator_destroy(iter);
  return iter_err == MGP_ERROR_NO_ERROR;
}

static struct mgp_map *create_complete_node_map(struct mgp_vertex *node, const filter_config_t *filter_config,
                                                struct mgp_memory *memory) {
  if (!node || !memory) {
    return NULL;
  }

  struct mgp_map *node_map = NULL;
  if (mgp_unordered_map_make_empty(memory, &node_map) != MGP_ERROR_NO_ERROR || node_map == NULL) {
    return NULL;
  }

  struct mgp_properties_iterator *iter = NULL;
  if (mgp_vertex_iter_properties(node, memory, &iter) != MGP_ERROR_NO_ERROR || !iter) {
    mgp_map_destroy(node_map);
    return NULL;
  }

  struct mgp_property *prop = NULL;
  enum mgp_error prop_err = mgp_properties_iterator_get(iter, &prop);
  while (prop_err == MGP_ERROR_NO_ERROR && prop != NULL) {
    if (!prop->name || !prop->value) {
      mgp_properties_iterator_destroy(iter);
      mgp_map_destroy(node_map);
      return NULL;
    }
    if (strcmp(prop->name, "_id") != 0 && strcmp(prop->name, "_type") != 0) {
      bool filter_ok = true;
      bool include_prop = should_include_node_property(prop->name, filter_config, node, &filter_ok);
      if (!filter_ok) {
        mgp_properties_iterator_destroy(iter);
        mgp_map_destroy(node_map);
        return NULL;
      }
      if (include_prop) {
        if (!map_put_move_borrowed(node_map, prop->name, prop->value)) {
          mgp_properties_iterator_destroy(iter);
          mgp_map_destroy(node_map);
          return NULL;
        }
      }
    }
    prop_err = mgp_properties_iterator_next(iter, &prop);
  }
  mgp_properties_iterator_destroy(iter);
  if (prop_err != MGP_ERROR_NO_ERROR) {
    mgp_map_destroy(node_map);
    return NULL;
  }

  size_t label_count = 0;
  if (mgp_vertex_labels_count(node, &label_count) != MGP_ERROR_NO_ERROR) {
    mgp_map_destroy(node_map);
    return NULL;
  }
  if (label_count == 1) {
    struct mgp_label label = {.name = NULL};
    if (mgp_vertex_label_at(node, 0, &label) != MGP_ERROR_NO_ERROR || !label.name) {
      mgp_map_destroy(node_map);
      return NULL;
    }
    struct mgp_value *type_value = NULL;
    if (mgp_value_make_string(label.name, memory, &type_value) != MGP_ERROR_NO_ERROR || type_value == NULL) {
      mgp_map_destroy(node_map);
      return NULL;
    }
    if (!map_put_move_owned(node_map, "_type", type_value)) {
      mgp_map_destroy(node_map);
      return NULL;
    }
  } else if (label_count > 1) {
    struct mgp_list *label_list = NULL;
    if (mgp_list_make_empty(label_count, memory, &label_list) != MGP_ERROR_NO_ERROR || label_list == NULL) {
      mgp_map_destroy(node_map);
      return NULL;
    }
    for (size_t i = 0; i < label_count; i++) {
      struct mgp_label label = {.name = NULL};
      if (mgp_vertex_label_at(node, i, &label) != MGP_ERROR_NO_ERROR || !label.name) {
        mgp_list_destroy(label_list);
        mgp_map_destroy(node_map);
        return NULL;
      }
      struct mgp_value *label_value = NULL;
      if (mgp_value_make_string(label.name, memory, &label_value) != MGP_ERROR_NO_ERROR || label_value == NULL) {
        mgp_list_destroy(label_list);
        mgp_map_destroy(node_map);
        return NULL;
      }
      if (!list_append_move_owned(label_list, label_value)) {
        mgp_list_destroy(label_list);
        mgp_map_destroy(node_map);
        return NULL;
      }
    }
    struct mgp_value *type_value = NULL;
    if (mgp_value_make_list(label_list, &type_value) != MGP_ERROR_NO_ERROR || type_value == NULL) {
      mgp_list_destroy(label_list);
      mgp_map_destroy(node_map);
      return NULL;
    }
    if (!map_put_move_owned(node_map, "_type", type_value)) {
      mgp_map_destroy(node_map);
      return NULL;
    }
  }

  struct mgp_vertex_id id;
  if (mgp_vertex_get_id(node, &id) != MGP_ERROR_NO_ERROR) {
    mgp_map_destroy(node_map);
    return NULL;
  }
  struct mgp_value *id_value = NULL;
  if (mgp_value_make_int(id.as_int, memory, &id_value) != MGP_ERROR_NO_ERROR || id_value == NULL) {
    mgp_map_destroy(node_map);
    return NULL;
  }
  if (!map_put_move_owned(node_map, "_id", id_value)) {
    mgp_map_destroy(node_map);
    return NULL;
  }

  return node_map;
}

static struct mgp_value *build_tree_from_path_recursive(struct mgp_path *path, size_t start_index, bool lower_case_rels,
                                                        const filter_config_t *filter_config,
                                                        struct mgp_memory *memory) {
  if (!path || !memory) {
    return NULL;
  }

  struct mgp_vertex *current_node = NULL;
  if (mgp_path_vertex_at(path, start_index, &current_node) != MGP_ERROR_NO_ERROR || current_node == NULL) {
    return NULL;
  }

  struct mgp_map *node_map = create_complete_node_map(current_node, filter_config, memory);
  if (!node_map) {
    return NULL;
  }

  size_t path_size = 0;
  if (mgp_path_size(path, &path_size) != MGP_ERROR_NO_ERROR) {
    mgp_map_destroy(node_map);
    return NULL;
  }

  if (start_index >= path_size) {
    struct mgp_value *result = NULL;
    if (mgp_value_make_map(node_map, &result) != MGP_ERROR_NO_ERROR || result == NULL) {
      mgp_map_destroy(node_map);
      return NULL;
    }
    return result;
  }

  struct mgp_edge *edge = NULL;
  if (mgp_path_edge_at(path, start_index, &edge) != MGP_ERROR_NO_ERROR || edge == NULL) {
    mgp_map_destroy(node_map);
    return NULL;
  }

  struct mgp_edge_type edge_type = {.name = NULL};
  if (mgp_edge_get_type(edge, &edge_type) != MGP_ERROR_NO_ERROR || edge_type.name == NULL) {
    mgp_map_destroy(node_map);
    return NULL;
  }

  const char *rel_type_str = edge_type.name;
  char *rel_type_processed = (char *)rel_type_str;
  if (lower_case_rels) {
    rel_type_processed = to_lowercase(rel_type_str, memory);
    if (!rel_type_processed) {
      mgp_map_destroy(node_map);
      return NULL;
    }
  }

  struct mgp_value *subtree_value =
      build_tree_from_path_recursive(path, start_index + 1, lower_case_rels, filter_config, memory);
  if (!subtree_value) {
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_map_destroy(node_map);
    return NULL;
  }

  struct mgp_map *subtree_map = NULL;
  if (mgp_value_get_map(subtree_value, &subtree_map) != MGP_ERROR_NO_ERROR || subtree_map == NULL) {
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_value_destroy(subtree_value);
    mgp_map_destroy(node_map);
    return NULL;
  }

  struct mgp_properties_iterator *iter = NULL;
  if (mgp_edge_iter_properties(edge, memory, &iter) != MGP_ERROR_NO_ERROR || !iter) {
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_value_destroy(subtree_value);
    mgp_map_destroy(node_map);
    return NULL;
  }

  struct mgp_property *prop = NULL;
  enum mgp_error prop_err = mgp_properties_iterator_get(iter, &prop);
  while (prop_err == MGP_ERROR_NO_ERROR && prop != NULL) {
    if (!prop->name || !prop->value) {
      mgp_properties_iterator_destroy(iter);
      if (lower_case_rels && rel_type_processed != rel_type_str) {
        mgp_free(memory, rel_type_processed);
      }
      mgp_value_destroy(subtree_value);
      mgp_map_destroy(node_map);
      return NULL;
    }
    if (strcmp(prop->name, "_id") != 0 && should_include_rel_property(prop->name, filter_config, rel_type_processed)) {
      char *qual_key = join_strings(rel_type_processed, ".", prop->name, memory);
      if (!qual_key) {
        mgp_properties_iterator_destroy(iter);
        if (lower_case_rels && rel_type_processed != rel_type_str) {
          mgp_free(memory, rel_type_processed);
        }
        mgp_value_destroy(subtree_value);
        mgp_map_destroy(node_map);
        return NULL;
      }
      bool put_ok = map_put_move_borrowed(subtree_map, qual_key, prop->value);
      mgp_free(memory, qual_key);
      if (!put_ok) {
        mgp_properties_iterator_destroy(iter);
        if (lower_case_rels && rel_type_processed != rel_type_str) {
          mgp_free(memory, rel_type_processed);
        }
        mgp_value_destroy(subtree_value);
        mgp_map_destroy(node_map);
        return NULL;
      }
    }
    prop_err = mgp_properties_iterator_next(iter, &prop);
  }
  mgp_properties_iterator_destroy(iter);
  if (prop_err != MGP_ERROR_NO_ERROR) {
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_value_destroy(subtree_value);
    mgp_map_destroy(node_map);
    return NULL;
  }

  struct mgp_edge_id edge_id;
  if (mgp_edge_get_id(edge, &edge_id) != MGP_ERROR_NO_ERROR) {
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_value_destroy(subtree_value);
    mgp_map_destroy(node_map);
    return NULL;
  }
  char *id_key = join_strings(rel_type_processed, ".", "_id", memory);
  if (!id_key) {
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_value_destroy(subtree_value);
    mgp_map_destroy(node_map);
    return NULL;
  }
  struct mgp_value *id_value = NULL;
  if (mgp_value_make_int(edge_id.as_int, memory, &id_value) != MGP_ERROR_NO_ERROR || id_value == NULL) {
    mgp_free(memory, id_key);
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_value_destroy(subtree_value);
    mgp_map_destroy(node_map);
    return NULL;
  }
  bool id_put_ok = map_put_move_owned(subtree_map, id_key, id_value);
  mgp_free(memory, id_key);
  if (!id_put_ok) {
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_value_destroy(subtree_value);
    mgp_map_destroy(node_map);
    return NULL;
  }

  struct mgp_list *rel_list = NULL;
  if (mgp_list_make_empty(1, memory, &rel_list) != MGP_ERROR_NO_ERROR || rel_list == NULL) {
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_value_destroy(subtree_value);
    mgp_map_destroy(node_map);
    return NULL;
  }

  if (!list_append_move_owned(rel_list, subtree_value)) {
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_list_destroy(rel_list);
    mgp_map_destroy(node_map);
    return NULL;
  }

  struct mgp_value *rel_list_value = NULL;
  if (mgp_value_make_list(rel_list, &rel_list_value) != MGP_ERROR_NO_ERROR || rel_list_value == NULL) {
    if (lower_case_rels && rel_type_processed != rel_type_str) {
      mgp_free(memory, rel_type_processed);
    }
    mgp_list_destroy(rel_list);
    mgp_map_destroy(node_map);
    return NULL;
  }

  bool rel_put_ok = map_put_move_owned(node_map, rel_type_processed, rel_list_value);
  if (lower_case_rels && rel_type_processed != rel_type_str) {
    mgp_free(memory, rel_type_processed);
  }
  if (!rel_put_ok) {
    mgp_map_destroy(node_map);
    return NULL;
  }

  struct mgp_value *result = NULL;
  if (mgp_value_make_map(node_map, &result) != MGP_ERROR_NO_ERROR || result == NULL) {
    mgp_map_destroy(node_map);
    return NULL;
  }

  return result;
}

static struct mgp_value *build_simple_tree_from_path(struct mgp_path *path, bool lower_case_rels,
                                                     const filter_config_t *filter_config, struct mgp_memory *memory) {
  return build_tree_from_path_recursive(path, 0, lower_case_rels, filter_config, memory);
}

static struct mgp_value *make_empty_map_value(struct mgp_memory *memory) {
  struct mgp_map *empty_map = NULL;
  if (mgp_unordered_map_make_empty(memory, &empty_map) != MGP_ERROR_NO_ERROR || empty_map == NULL) {
    return NULL;
  }
  struct mgp_value *result = NULL;
  if (mgp_value_make_map(empty_map, &result) != MGP_ERROR_NO_ERROR || result == NULL) {
    mgp_map_destroy(empty_map);
    return NULL;
  }
  return result;
}

static void destroy_root_groups(struct mgp_value **root_groups, int64_t *root_ids, size_t count,
                                struct mgp_memory *memory) {
  if (root_groups) {
    for (size_t i = 0; i < count; i++) {
      if (root_groups[i]) {
        mgp_value_destroy(root_groups[i]);
      }
    }
    mgp_free(memory, root_groups);
  }
  if (root_ids) {
    mgp_free(memory, root_ids);
  }
}

static struct mgp_value *convert_to_tree_impl(struct mgp_value *value, bool lower_case_rels, struct mgp_map *config,
                                              struct mgp_memory *memory) {
  filter_config_t filter_config;
  if (!parse_filter_config(&filter_config, config, memory)) {
    return NULL;
  }

  if (!value) {
    destroy_filter_config(&filter_config, memory);
    return make_empty_map_value(memory);
  }

  enum mgp_value_type value_type = MGP_VALUE_TYPE_NULL;
  if (mgp_value_get_type(value, &value_type) != MGP_ERROR_NO_ERROR) {
    destroy_filter_config(&filter_config, memory);
    return NULL;
  }

  if (value_type == MGP_VALUE_TYPE_NULL) {
    destroy_filter_config(&filter_config, memory);
    return make_empty_map_value(memory);
  }

  if (value_type == MGP_VALUE_TYPE_PATH) {
    struct mgp_path *path = NULL;
    if (mgp_value_get_path(value, &path) != MGP_ERROR_NO_ERROR || path == NULL) {
      destroy_filter_config(&filter_config, memory);
      return NULL;
    }
    struct mgp_value *single_res = build_simple_tree_from_path(path, lower_case_rels, &filter_config, memory);
    destroy_filter_config(&filter_config, memory);
    return single_res;
  }

  if (value_type == MGP_VALUE_TYPE_LIST) {
    struct mgp_list *paths = NULL;
    if (mgp_value_get_list(value, &paths) != MGP_ERROR_NO_ERROR || paths == NULL) {
      destroy_filter_config(&filter_config, memory);
      return NULL;
    }

    size_t paths_size = 0;
    if (mgp_list_size(paths, &paths_size) != MGP_ERROR_NO_ERROR) {
      destroy_filter_config(&filter_config, memory);
      return NULL;
    }

    if (paths_size == 0) {
      destroy_filter_config(&filter_config, memory);
      return make_empty_map_value(memory);
    }

    struct mgp_value **root_groups = NULL;
    int64_t *root_ids = NULL;
    size_t root_group_count = 0;
    size_t root_group_cap = 0;

    for (size_t i = 0; i < paths_size; i++) {
      struct mgp_value *path_value = NULL;
      if (mgp_list_at(paths, i, &path_value) != MGP_ERROR_NO_ERROR || path_value == NULL) {
        destroy_root_groups(root_groups, root_ids, root_group_count, memory);
        destroy_filter_config(&filter_config, memory);
        return NULL;
      }

      enum mgp_value_type elem_type = MGP_VALUE_TYPE_NULL;
      if (mgp_value_get_type(path_value, &elem_type) != MGP_ERROR_NO_ERROR || elem_type != MGP_VALUE_TYPE_PATH) {
        destroy_root_groups(root_groups, root_ids, root_group_count, memory);
        destroy_filter_config(&filter_config, memory);
        return NULL;
      }

      struct mgp_path *path = NULL;
      if (mgp_value_get_path(path_value, &path) != MGP_ERROR_NO_ERROR || path == NULL) {
        destroy_root_groups(root_groups, root_ids, root_group_count, memory);
        destroy_filter_config(&filter_config, memory);
        return NULL;
      }

      struct mgp_vertex *root_node = NULL;
      if (mgp_path_vertex_at(path, 0, &root_node) != MGP_ERROR_NO_ERROR || root_node == NULL) {
        destroy_root_groups(root_groups, root_ids, root_group_count, memory);
        destroy_filter_config(&filter_config, memory);
        return NULL;
      }

      struct mgp_vertex_id root_id;
      if (mgp_vertex_get_id(root_node, &root_id) != MGP_ERROR_NO_ERROR) {
        destroy_root_groups(root_groups, root_ids, root_group_count, memory);
        destroy_filter_config(&filter_config, memory);
        return NULL;
      }

      size_t group_index = SIZE_MAX;
      for (size_t j = 0; j < root_group_count; j++) {
        if (root_ids[j] == root_id.as_int) {
          group_index = j;
          break;
        }
      }

      if (group_index == SIZE_MAX) {
        if (root_group_count >= root_group_cap) {
          size_t next_cap = root_group_cap == 0 ? INITIAL_CAPACITY : root_group_cap * 2;
          if (next_cap < root_group_cap || next_cap > SIZE_MAX / sizeof(struct mgp_value *) ||
              next_cap > SIZE_MAX / sizeof(int64_t)) {
            destroy_root_groups(root_groups, root_ids, root_group_count, memory);
            destroy_filter_config(&filter_config, memory);
            return NULL;
          }
          void *new_groups = NULL;
          void *new_ids = NULL;
          if (!alloc_bytes(memory, next_cap * sizeof(struct mgp_value *), &new_groups)) {
            destroy_root_groups(root_groups, root_ids, root_group_count, memory);
            destroy_filter_config(&filter_config, memory);
            return NULL;
          }
          if (!alloc_bytes(memory, next_cap * sizeof(int64_t), &new_ids)) {
            mgp_free(memory, new_groups);
            destroy_root_groups(root_groups, root_ids, root_group_count, memory);
            destroy_filter_config(&filter_config, memory);
            return NULL;
          }
          if (root_groups) {
            memcpy(new_groups, root_groups, root_group_count * sizeof(struct mgp_value *));
            mgp_free(memory, root_groups);
          }
          if (root_ids) {
            memcpy(new_ids, root_ids, root_group_count * sizeof(int64_t));
            mgp_free(memory, root_ids);
          }
          root_groups = (struct mgp_value **)new_groups;
          root_ids = (int64_t *)new_ids;
          root_group_cap = next_cap;
        }
        group_index = root_group_count;
        root_ids[group_index] = root_id.as_int;
        root_groups[group_index] = NULL;
        root_group_count++;
      }

      struct mgp_value *tree_value = build_simple_tree_from_path(path, lower_case_rels, &filter_config, memory);
      if (!tree_value) {
        destroy_root_groups(root_groups, root_ids, root_group_count, memory);
        destroy_filter_config(&filter_config, memory);
        return NULL;
      }

      if (root_groups[group_index] == NULL) {
        root_groups[group_index] = tree_value;
      } else {
        struct mgp_map *existing_map = NULL;
        struct mgp_map *tree_map = NULL;
        if (mgp_value_get_map(root_groups[group_index], &existing_map) != MGP_ERROR_NO_ERROR || !existing_map ||
            mgp_value_get_map(tree_value, &tree_map) != MGP_ERROR_NO_ERROR || !tree_map ||
            !merge_trees(existing_map, tree_map, memory)) {
          mgp_value_destroy(tree_value);
          destroy_root_groups(root_groups, root_ids, root_group_count, memory);
          destroy_filter_config(&filter_config, memory);
          return NULL;
        }
        mgp_value_destroy(tree_value);
      }
    }

    destroy_filter_config(&filter_config, memory);

    if (root_group_count == 0) {
      destroy_root_groups(root_groups, root_ids, 0, memory);
      return make_empty_map_value(memory);
    }

    if (root_group_count == 1) {
      struct mgp_value *single_root = root_groups[0];
      root_groups[0] = NULL;
      destroy_root_groups(root_groups, root_ids, 1, memory);
      return single_root;
    }

    struct mgp_list *tree_list = NULL;
    if (mgp_list_make_empty(root_group_count, memory, &tree_list) != MGP_ERROR_NO_ERROR || tree_list == NULL) {
      destroy_root_groups(root_groups, root_ids, root_group_count, memory);
      return NULL;
    }

    for (size_t i = 0; i < root_group_count; i++) {
      struct mgp_value *root_val = root_groups[i];
      root_groups[i] = NULL;
      if (!list_append_move_owned(tree_list, root_val)) {
        mgp_list_destroy(tree_list);
        destroy_root_groups(root_groups, root_ids, root_group_count, memory);
        return NULL;
      }
    }
    destroy_root_groups(root_groups, root_ids, root_group_count, memory);

    struct mgp_value *result = NULL;
    if (mgp_value_make_list(tree_list, &result) != MGP_ERROR_NO_ERROR || result == NULL) {
      mgp_list_destroy(tree_list);
      return NULL;
    }
    return result;
  }

  destroy_filter_config(&filter_config, memory);

  struct mgp_value *result = NULL;
  if (mgp_value_copy(value, memory, &result) != MGP_ERROR_NO_ERROR || result == NULL) {
    return NULL;
  }
  return result;
}

static void to_tree(struct mgp_list *args, struct mgp_graph *graph, struct mgp_result *result,
                    struct mgp_memory *memory) {
  (void)graph;
  size_t args_size = 0;
  if (mgp_list_size(args, &args_size) != MGP_ERROR_NO_ERROR) {
    mgp_result_set_error_msg(result, "Failed to get arguments size");
    return;
  }

  if (args_size == 0) {
    mgp_result_set_error_msg(result, "to_tree requires at least one argument");
    return;
  }

  struct mgp_value *input = NULL;
  if (mgp_list_at(args, 0, &input) != MGP_ERROR_NO_ERROR || input == NULL) {
    mgp_result_set_error_msg(result, "Failed to get input argument");
    return;
  }

  bool lower_case_rels = true;
  if (args_size > 1) {
    struct mgp_value *lower_case_arg = NULL;
    if (mgp_list_at(args, 1, &lower_case_arg) == MGP_ERROR_NO_ERROR && lower_case_arg != NULL) {
      enum mgp_value_type arg_type = MGP_VALUE_TYPE_NULL;
      if (mgp_value_get_type(lower_case_arg, &arg_type) == MGP_ERROR_NO_ERROR && arg_type == MGP_VALUE_TYPE_BOOL) {
        int bool_val = 0;
        if (mgp_value_get_bool(lower_case_arg, &bool_val) == MGP_ERROR_NO_ERROR) {
          lower_case_rels = (bool_val != 0);
        }
      }
    }
  }

  struct mgp_map *config = NULL;
  if (args_size > 2) {
    struct mgp_value *config_arg = NULL;
    if (mgp_list_at(args, 2, &config_arg) == MGP_ERROR_NO_ERROR && config_arg != NULL) {
      enum mgp_value_type arg_type = MGP_VALUE_TYPE_NULL;
      if (mgp_value_get_type(config_arg, &arg_type) == MGP_ERROR_NO_ERROR && arg_type == MGP_VALUE_TYPE_MAP) {
        if (mgp_value_get_map(config_arg, &config) != MGP_ERROR_NO_ERROR || config == NULL) {
          mgp_result_set_error_msg(result, "Failed to get config map");
          return;
        }
        size_t size = 0;
        if (mgp_map_size(config, &size) == MGP_ERROR_NO_ERROR && size == 0) {
          config = NULL;
        }
      }
    }
  }

  struct mgp_value *result_value = convert_to_tree_impl(input, lower_case_rels, config, memory);
  if (!result_value) {
    mgp_result_set_error_msg(result, "Failed to convert input to tree");
    return;
  }

  enum mgp_value_type result_type = MGP_VALUE_TYPE_NULL;
  if (mgp_value_get_type(result_value, &result_type) != MGP_ERROR_NO_ERROR) {
    mgp_value_destroy(result_value);
    mgp_result_set_error_msg(result, "Failed to get result type");
    return;
  }

  if (result_type == MGP_VALUE_TYPE_LIST) {
    struct mgp_list *result_list = NULL;
    if (mgp_value_get_list(result_value, &result_list) != MGP_ERROR_NO_ERROR || result_list == NULL) {
      mgp_value_destroy(result_value);
      mgp_result_set_error_msg(result, "Failed to get result list");
      return;
    }

    size_t list_size = 0;
    if (mgp_list_size(result_list, &list_size) != MGP_ERROR_NO_ERROR) {
      mgp_value_destroy(result_value);
      mgp_result_set_error_msg(result, "Failed to get list size");
      return;
    }

    for (size_t i = 0; i < list_size; i++) {
      struct mgp_value *tree_value = NULL;
      if (mgp_list_at(result_list, i, &tree_value) != MGP_ERROR_NO_ERROR || tree_value == NULL) {
        mgp_value_destroy(result_value);
        mgp_result_set_error_msg(result, "Failed to get result tree entry");
        return;
      }

      struct mgp_result_record *record = NULL;
      if (mgp_result_new_record(result, &record) != MGP_ERROR_NO_ERROR || record == NULL) {
        mgp_value_destroy(result_value);
        mgp_result_set_error_msg(result, "Failed to create result record");
        return;
      }

      if (mgp_result_record_insert(record, kReturnValue, tree_value) != MGP_ERROR_NO_ERROR) {
        mgp_value_destroy(result_value);
        mgp_result_set_error_msg(result, "Failed to insert result value");
        return;
      }
    }
    mgp_value_destroy(result_value);
  } else {
    struct mgp_result_record *record = NULL;
    if (mgp_result_new_record(result, &record) != MGP_ERROR_NO_ERROR || record == NULL) {
      mgp_value_destroy(result_value);
      mgp_result_set_error_msg(result, "Failed to create result record");
      return;
    }

    if (mgp_result_record_insert(record, kReturnValue, result_value) != MGP_ERROR_NO_ERROR) {
      mgp_value_destroy(result_value);
      mgp_result_set_error_msg(result, "Failed to insert result value");
      return;
    }
    mgp_value_destroy(result_value);
  }
}

int mgp_init_module(struct mgp_module *module, struct mgp_memory *memory) {
  struct mgp_proc *proc = NULL;
  if (mgp_module_add_read_procedure(module, kProcedureToTree, to_tree, &proc) != MGP_ERROR_NO_ERROR || proc == NULL) {
    return 1;
  }

  struct mgp_type *any_type = NULL;
  if (mgp_type_any(&any_type) != MGP_ERROR_NO_ERROR || any_type == NULL) {
    return 1;
  }

  struct mgp_type *nullable_any_type = NULL;
  if (mgp_type_nullable(any_type, &nullable_any_type) != MGP_ERROR_NO_ERROR || nullable_any_type == NULL) {
    return 1;
  }

  if (mgp_proc_add_arg(proc, kParameterPaths, nullable_any_type) != MGP_ERROR_NO_ERROR) {
    return 1;
  }

  struct mgp_type *bool_type = NULL;
  if (mgp_type_bool(&bool_type) != MGP_ERROR_NO_ERROR || bool_type == NULL) {
    return 1;
  }

  struct mgp_value *true_value = NULL;
  if (mgp_value_make_bool(1, memory, &true_value) != MGP_ERROR_NO_ERROR || true_value == NULL) {
    return 1;
  }

  if (mgp_proc_add_opt_arg(proc, kParameterLowerCaseRels, bool_type, true_value) != MGP_ERROR_NO_ERROR) {
    mgp_value_destroy(true_value);
    return 1;
  }
  mgp_value_destroy(true_value);

  struct mgp_type *map_type = NULL;
  if (mgp_type_map(&map_type) != MGP_ERROR_NO_ERROR || map_type == NULL) {
    return 1;
  }

  struct mgp_map *empty_map = NULL;
  if (mgp_unordered_map_make_empty(memory, &empty_map) != MGP_ERROR_NO_ERROR || empty_map == NULL) {
    return 1;
  }

  struct mgp_value *empty_map_value = NULL;
  if (mgp_value_make_map(empty_map, &empty_map_value) != MGP_ERROR_NO_ERROR || empty_map_value == NULL) {
    mgp_map_destroy(empty_map);
    return 1;
  }

  if (mgp_proc_add_opt_arg(proc, kParameterConfig, map_type, empty_map_value) != MGP_ERROR_NO_ERROR) {
    mgp_value_destroy(empty_map_value);
    return 1;
  }
  mgp_value_destroy(empty_map_value);

  if (mgp_proc_add_result(proc, kReturnValue, map_type) != MGP_ERROR_NO_ERROR) {
    return 1;
  }

  return 0;
}

int mgp_shutdown_module() { return 0; }
