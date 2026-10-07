#include "config/cdsp_yaml.h"

#include <cjson/cJSON.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <yaml.h>

#define MAX_YAML_DEPTH 128

/* --- Dynamic Buffer for libyaml Emitter Output --- */

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} yaml_buffer_t;

static int yaml_buf_write_handler(void *data, unsigned char *buffer,
                                  size_t size) {
  yaml_buffer_t *b = (yaml_buffer_t *)data;
  if (!b)
    return 0;
  if (b->size + size + 1 > b->capacity) {
    size_t new_cap = (b->capacity + size + 256) * 2;
    char *new_data = (char *)realloc(b->data, new_cap);
    if (!new_data)
      return 0;
    b->data = new_data;
    b->capacity = new_cap;
  }
  memcpy(b->data + b->size, buffer, size);
  b->size += size;
  b->data[b->size] = '\0';
  return 1;
}

/* --- YAML Document to cJSON Tree --- */

/**
 * Keys whose values are strings (or arrays of strings) in the config schema
 * (every parse_json_str_strict / parse_labels_array_strict field). Upstream
 * yaml_serde accepts any plain scalar for a String field, so under these keys
 * a plain scalar such as `title: 12345`, `name: 1` or `names: [1, 2]` is kept
 * as its verbatim text instead of being inferred as a number/boolean (which
 * the strict JSON parser would reject). Nulls stay null (Option<String>).
 * JSON input is unaffected and stays as strict as upstream serde_json.
 */
static bool yaml_key_is_string_field(const char *key) {
  static const char *const keys[] = {"autoconnect_to",
                                     "description",
                                     "device",
                                     "filename",
                                     "format",
                                     "interpolation",
                                     "labels",
                                     "link_mute_control",
                                     "link_volume_control",
                                     "name",
                                     "names",
                                     "node_description",
                                     "node_group_name",
                                     "node_name",
                                     "title",
                                     "window",
                                     NULL};
  for (size_t i = 0; keys[i]; i++) {
    if (strcmp(key, keys[i]) == 0)
      return true;
  }
  return false;
}

static cJSON *yaml_node_to_json_ctx(yaml_document_t *doc, yaml_node_t *node,
                                    int depth, bool string_field,
                                    char **out_err);

static cJSON *yaml_node_to_json(yaml_document_t *doc, yaml_node_t *node,
                                int depth, char **out_err) {
  return yaml_node_to_json_ctx(doc, node, depth, false, out_err);
}

static cJSON *yaml_node_to_json_ctx(yaml_document_t *doc, yaml_node_t *node,
                                    int depth, bool string_field,
                                    char **out_err) {
  if (!node)
    return cJSON_CreateNull();

  if (depth > MAX_YAML_DEPTH) {
    if (out_err && !*out_err)
      *out_err = strdup("YAML document nesting depth exceeded maximum depth");
    return NULL;
  }

  switch (node->type) {
  case YAML_SCALAR_NODE: {
    const char *val = (const char *)node->data.scalar.value;

    // Explicitly quoted or block scalars are always strings
    if (node->data.scalar.style != YAML_PLAIN_SCALAR_STYLE) {
      return cJSON_CreateString(val ? val : "");
    }

    // YAML nulls: empty, ~ or case-insensitive null
    if (!val || !*val || strcmp(val, "~") == 0 ||
        strcasecmp(val, "null") == 0) {
      return cJSON_CreateNull();
    }

    // String-typed schema field: keep the plain scalar's text verbatim.
    if (string_field) {
      return cJSON_CreateString(val);
    }

    // Case-insensitive booleans
    if (strcasecmp(val, "true") == 0) {
      return cJSON_CreateTrue();
    }
    if (strcasecmp(val, "false") == 0) {
      return cJSON_CreateFalse();
    }

    // YAML special floats (.inf, +.inf, -.inf, .nan)
    if (strcasecmp(val, ".inf") == 0 || strcasecmp(val, "+.inf") == 0) {
      return cJSON_CreateNumber(INFINITY);
    }
    if (strcasecmp(val, "-.inf") == 0) {
      return cJSON_CreateNumber(-INFINITY);
    }
    if (strcasecmp(val, ".nan") == 0) {
      return cJSON_CreateNumber(NAN);
    }

    // Rely on cJSON to parse numbers and standard literals
    cJSON *literal = cJSON_ParseWithOpts(val, NULL, 1);
    if (literal) {
      return literal;
    }

    // Unquoted plain string fallback
    return cJSON_CreateString(val);
  }

  case YAML_SEQUENCE_NODE: {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
      if (out_err && !*out_err)
        *out_err = strdup("Out of memory during YAML parsing");
      return NULL;
    }

    for (yaml_node_item_t *item = node->data.sequence.items.start;
         item < node->data.sequence.items.top; item++) {
      yaml_node_t *child = yaml_document_get_node(doc, *item);
      cJSON *child_json =
          yaml_node_to_json_ctx(doc, child, depth + 1, string_field, out_err);
      if (!child_json) {
        cJSON_Delete(arr);
        return NULL;
      }
      cJSON_AddItemToArray(arr, child_json);
    }
    return arr;
  }

  case YAML_MAPPING_NODE: {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
      if (out_err && !*out_err)
        *out_err = strdup("Out of memory during YAML parsing");
      return NULL;
    }

    for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; pair++) {
      yaml_node_t *key_node = yaml_document_get_node(doc, pair->key);
      yaml_node_t *val_node = yaml_document_get_node(doc, pair->value);

      if (!key_node || key_node->type != YAML_SCALAR_NODE) {
        cJSON_Delete(obj);
        if (out_err && !*out_err)
          *out_err = strdup("YAML mapping keys must be scalars");
        return NULL;
      }

      const char *key_str = (const char *)key_node->data.scalar.value;
      if (!key_str)
        key_str = "";

      if (cJSON_GetObjectItemCaseSensitive(obj, key_str) != NULL) {
        cJSON_Delete(obj);
        if (out_err && !*out_err) {
          char err_buf[256];
          snprintf(err_buf, sizeof(err_buf),
                   "Duplicate YAML key '%s' in mapping", key_str);
          *out_err = strdup(err_buf);
        }
        return NULL;
      }

      cJSON *val_json = yaml_node_to_json_ctx(
          doc, val_node, depth + 1, yaml_key_is_string_field(key_str), out_err);
      if (!val_json) {
        cJSON_Delete(obj);
        return NULL;
      }
      cJSON_AddItemToObject(obj, key_str, val_json);
    }
    return obj;
  }

  case YAML_NO_NODE:
  default:
    return cJSON_CreateNull();
  }
}

cJSON *cdsp_yaml_to_json(const char *yaml_str, char **out_err) {
  if (out_err)
    *out_err = NULL;

  if (!yaml_str) {
    if (out_err)
      *out_err = strdup("Null input YAML string");
    return NULL;
  }

  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    if (out_err)
      *out_err = strdup("Failed to initialize libyaml parser");
    return NULL;
  }

  yaml_parser_set_input_string(&parser, (const unsigned char *)yaml_str,
                               strlen(yaml_str));

  yaml_document_t document;
  if (!yaml_parser_load(&parser, &document)) {
    if (out_err) {
      char err_buf[512];
      if (parser.problem) {
        if (parser.context) {
          snprintf(err_buf, sizeof(err_buf),
                   "YAML parse error: %s (line %zu, column %zu, while %s at "
                   "line %zu, column %zu)",
                   parser.problem, parser.problem_mark.line + 1,
                   parser.problem_mark.column + 1, parser.context,
                   parser.context_mark.line + 1,
                   parser.context_mark.column + 1);
        } else {
          snprintf(err_buf, sizeof(err_buf),
                   "YAML parse error: %s (line %zu, column %zu)",
                   parser.problem, parser.problem_mark.line + 1,
                   parser.problem_mark.column + 1);
        }
      } else {
        snprintf(err_buf, sizeof(err_buf), "Unknown YAML syntax error");
      }
      *out_err = strdup(err_buf);
    }
    yaml_parser_delete(&parser);
    return NULL;
  }

  yaml_node_t *root = yaml_document_get_root_node(&document);
  if (!root) {
    yaml_document_delete(&document);
    yaml_parser_delete(&parser);
    if (out_err)
      *out_err = strdup("Empty or invalid YAML document");
    return NULL;
  }

  // Check for multiple documents in stream
  yaml_document_t doc2;
  if (yaml_parser_load(&parser, &doc2)) {
    if (yaml_document_get_root_node(&doc2) != NULL) {
      yaml_document_delete(&doc2);
      yaml_document_delete(&document);
      yaml_parser_delete(&parser);
      if (out_err)
        *out_err =
            strdup("YAML syntax error: multiple documents not supported");
      return NULL;
    }
    yaml_document_delete(&doc2);
  }

  cJSON *root_json = yaml_node_to_json(&document, root, 0, out_err);
  yaml_document_delete(&document);
  yaml_parser_delete(&parser);

  return root_json;
}

/* --- cJSON Tree to YAML String --- */

static bool yaml_scalar_conflicts_with_literal(const char *str) {
  if (!str || !*str)
    return true;
  if (strcmp(str, "~") == 0 || strcasecmp(str, "null") == 0 ||
      strcasecmp(str, "true") == 0 || strcasecmp(str, "false") == 0 ||
      strcasecmp(str, ".inf") == 0 || strcasecmp(str, "+.inf") == 0 ||
      strcasecmp(str, "-.inf") == 0 || strcasecmp(str, ".nan") == 0) {
    return true;
  }
  cJSON *lit = cJSON_ParseWithOpts(str, NULL, 1);
  if (lit) {
    cJSON_Delete(lit);
    return true;
  }
  return false;
}

static void configure_yaml_styles(yaml_document_t *doc, yaml_node_t *node) {
  if (!node)
    return;

  if (node->type == YAML_MAPPING_NODE) {
    if (node->data.mapping.pairs.start == node->data.mapping.pairs.top) {
      node->data.mapping.style = YAML_FLOW_MAPPING_STYLE;
      return;
    }
    node->data.mapping.style = YAML_BLOCK_MAPPING_STYLE;
    for (yaml_node_pair_t *p = node->data.mapping.pairs.start;
         p < node->data.mapping.pairs.top; p++) {
      yaml_node_t *k = yaml_document_get_node(doc, p->key);
      yaml_node_t *v = yaml_document_get_node(doc, p->value);

      if (k && k->type == YAML_SCALAR_NODE) {
        const char *k_str = (const char *)k->data.scalar.value;
        if (!yaml_scalar_conflicts_with_literal(k_str)) {
          k->data.scalar.style = YAML_PLAIN_SCALAR_STYLE;
        }
      }
      if (v && v->type == YAML_SCALAR_NODE) {
        // Leave numbers, booleans, and plain strings unquoted; quote string
        // literals that collide with JSON/YAML literals (e.g. "44100", "true")
        const char *v_str = (const char *)v->data.scalar.value;
        if (!yaml_scalar_conflicts_with_literal(v_str)) {
          v->data.scalar.style = YAML_PLAIN_SCALAR_STYLE;
        }
      }
      configure_yaml_styles(doc, v);
    }
  } else if (node->type == YAML_SEQUENCE_NODE) {
    if (node->data.sequence.items.start == node->data.sequence.items.top) {
      node->data.sequence.style = YAML_FLOW_SEQUENCE_STYLE;
      return;
    }
    node->data.sequence.style = YAML_BLOCK_SEQUENCE_STYLE;
    for (yaml_node_item_t *it = node->data.sequence.items.start;
         it < node->data.sequence.items.top; it++) {
      yaml_node_t *v = yaml_document_get_node(doc, *it);
      if (v && v->type == YAML_SCALAR_NODE) {
        const char *v_str = (const char *)v->data.scalar.value;
        if (!yaml_scalar_conflicts_with_literal(v_str)) {
          v->data.scalar.style = YAML_PLAIN_SCALAR_STYLE;
        }
      }
      configure_yaml_styles(doc, v);
    }
  }
}

char *cdsp_json_to_yaml(const cJSON *json) {
  if (!json)
    return NULL;

  char *json_str = cJSON_PrintUnformatted(json);
  if (!json_str)
    return NULL;

  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    free(json_str);
    return NULL;
  }

  yaml_parser_set_input_string(&parser, (const unsigned char *)json_str,
                               strlen(json_str));

  yaml_document_t doc;
  if (!yaml_parser_load(&parser, &doc)) {
    yaml_parser_delete(&parser);
    free(json_str);
    return NULL;
  }
  yaml_parser_delete(&parser);
  free(json_str);

  configure_yaml_styles(&doc, yaml_document_get_root_node(&doc));

  yaml_buffer_t buf = {
      .data = (char *)malloc(256),
      .size = 0,
      .capacity = 256,
  };
  if (!buf.data) {
    yaml_document_delete(&doc);
    return NULL;
  }
  buf.data[0] = '\0';

  yaml_emitter_t emitter;
  if (!yaml_emitter_initialize(&emitter)) {
    free(buf.data);
    yaml_document_delete(&doc);
    return NULL;
  }

  yaml_emitter_set_output(&emitter, yaml_buf_write_handler, &buf);
  yaml_emitter_set_indent(&emitter, 2);
  yaml_emitter_set_unicode(&emitter, 1);

  if (!yaml_emitter_dump(&emitter, &doc)) {
    free(buf.data);
    yaml_emitter_delete(&emitter);
    return NULL;
  }

  yaml_emitter_delete(&emitter);
  return buf.data;
}
