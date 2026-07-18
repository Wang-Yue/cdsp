#include "Config/cdsp_yaml.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Dynamic String Buffer --- */

typedef struct {
  char* data;
  size_t size;
  size_t capacity;
} buf_t;

static void buf_init(buf_t* b) {
  b->capacity = 256;
  b->size = 0;
  b->data = (char*)malloc(b->capacity);
  if (b->data) b->data[0] = '\0';
}

static void buf_append_str(buf_t* b, const char* str) {
  if (!str || !b->data) return;
  size_t len = strlen(str);
  if (b->size + len + 1 > b->capacity) {
    size_t new_cap = (b->capacity + len + 256) * 2;
    char* new_data = (char*)realloc(b->data, new_cap);
    if (!new_data) return;
    b->data = new_data;
    b->capacity = new_cap;
  }
  memcpy(b->data + b->size, str, len);
  b->size += len;
  b->data[b->size] = '\0';
}

static void buf_append_indent(buf_t* b, int indent) {
  for (int i = 0; i < indent; ++i) {
    buf_append_str(b, " ");
  }
}

static char* buf_detach(buf_t* b) {
  char* res = b->data;
  b->data = NULL;
  b->size = 0;
  b->capacity = 0;
  return res;
}

/* --- JSON to YAML Emitter --- */

static bool needs_quotes(const char* str) {
  if (!str || !*str) return true;
  if (strcmp(str, "true") == 0 || strcmp(str, "false") == 0 ||
      strcmp(str, "yes") == 0 || strcmp(str, "no") == 0 ||
      strcmp(str, "null") == 0 || strcmp(str, "~") == 0) {
    return true;
  }
  for (const char* p = str; *p; ++p) {
    if (strchr(":{}[]#,&\n*?|-<>=!%@`'\"", *p)) return true;
    if (isspace((unsigned char)*p)) return true;
  }
  char* endptr = NULL;
  strtod(str, &endptr);
  if (endptr && *endptr == '\0') return true;
  return false;
}

static void emit_string_scalar(buf_t* b, const char* str) {
  if (!str) {
    buf_append_str(b, "null");
    return;
  }
  if (!needs_quotes(str)) {
    buf_append_str(b, str);
    return;
  }
  buf_append_str(b, "\"");
  for (const char* p = str; *p; ++p) {
    if (*p == '"') {
      buf_append_str(b, "\\\"");
    } else if (*p == '\\') {
      buf_append_str(b, "\\\\");
    } else if (*p == '\n') {
      buf_append_str(b, "\\n");
    } else if (*p == '\r') {
      buf_append_str(b, "\\r");
    } else if (*p == '\t') {
      buf_append_str(b, "\\t");
    } else {
      char tmp[2] = {*p, '\0'};
      buf_append_str(b, tmp);
    }
  }
  buf_append_str(b, "\"");
}

static void emit_yaml_node(const cJSON* item, int indent, buf_t* b,
                           bool is_array_element) {
  if (!item) return;

  if (cJSON_IsObject(item)) {
    if (!item->child) {
      buf_append_str(b, "{}\n");
      return;
    }
    if (is_array_element) {
      // First line of array element is already indented with '- '
    }
    bool first = true;
    for (const cJSON* child = item->child; child; child = child->next) {
      if (!first || !is_array_element) {
        buf_append_indent(b, indent);
      }
      first = false;
      if (child->string) {
        if (needs_quotes(child->string)) {
          emit_string_scalar(b, child->string);
        } else {
          buf_append_str(b, child->string);
        }
        buf_append_str(b, ": ");
      }
      if (cJSON_IsObject(child)) {
        buf_append_str(b, "\n");
        emit_yaml_node(child, indent + 2, b, false);
      } else if (cJSON_IsArray(child)) {
        if (!child->child) {
          buf_append_str(b, "[]\n");
        } else {
          buf_append_str(b, "\n");
          emit_yaml_node(child, indent + 2, b, false);
        }
      } else {
        emit_yaml_node(child, 0, b, false);
        buf_append_str(b, "\n");
      }
    }
  } else if (cJSON_IsArray(item)) {
    if (!item->child) {
      buf_append_str(b, "[]\n");
      return;
    }
    for (const cJSON* child = item->child; child; child = child->next) {
      buf_append_indent(b, indent);
      buf_append_str(b, "- ");
      if (cJSON_IsObject(child)) {
        emit_yaml_node(child, indent + 2, b, true);
      } else if (cJSON_IsArray(child)) {
        buf_append_str(b, "\n");
        emit_yaml_node(child, indent + 2, b, false);
      } else {
        emit_yaml_node(child, 0, b, false);
        buf_append_str(b, "\n");
      }
    }
  } else if (cJSON_IsString(item)) {
    emit_string_scalar(b, item->valuestring);
  } else if (cJSON_IsNumber(item)) {
    char num_buf[64];
    if (item->valuedouble == (double)item->valueint) {
      snprintf(num_buf, sizeof(num_buf), "%d", item->valueint);
    } else {
      snprintf(num_buf, sizeof(num_buf), "%.15g", item->valuedouble);
    }
    buf_append_str(b, num_buf);
  } else if (cJSON_IsTrue(item)) {
    buf_append_str(b, "true");
  } else if (cJSON_IsFalse(item)) {
    buf_append_str(b, "false");
  } else if (cJSON_IsNull(item)) {
    buf_append_str(b, "null");
  }
}

char* cdsp_json_to_yaml(const cJSON* json) {
  if (!json) return NULL;
  buf_t b;
  buf_init(&b);
  emit_yaml_node(json, 0, &b, false);
  return buf_detach(&b);
}

/* --- YAML to JSON Parser --- */

static char* trim_str(char* str) {
  while (isspace((unsigned char)*str)) str++;
  if (*str == 0) return str;
  char* end = str + strlen(str) - 1;
  while (end > str && isspace((unsigned char)*end)) end--;
  end[1] = '\0';
  return str;
}

static cJSON* parse_scalar_val(const char* str) {
  if (!str || !*str || strcmp(str, "~") == 0 || strcasecmp(str, "null") == 0) {
    return cJSON_CreateNull();
  }
  if (*str == '[' || *str == '{') {
    cJSON* parsed = cJSON_Parse(str);
    if (parsed) return parsed;
  }
  if (strcasecmp(str, "true") == 0 || strcasecmp(str, "yes") == 0) {
    return cJSON_CreateTrue();
  }
  if (strcasecmp(str, "false") == 0 || strcasecmp(str, "no") == 0) {
    return cJSON_CreateFalse();
  }

  size_t len = strlen(str);
  if (len >= 2 && ((str[0] == '"' && str[len - 1] == '"') ||
                   (str[0] == '\'' && str[len - 1] == '\''))) {
    char* unquoted = (char*)malloc(len);
    if (!unquoted) return NULL;
    size_t out_idx = 0;
    for (size_t i = 1; i < len - 1; ++i) {
      if (str[i] == '\\' && i + 1 < len - 1) {
        i++;
        if (str[i] == 'n')
          unquoted[out_idx++] = '\n';
        else if (str[i] == 't')
          unquoted[out_idx++] = '\t';
        else if (str[i] == 'r')
          unquoted[out_idx++] = '\r';
        else
          unquoted[out_idx++] = str[i];
      } else {
        unquoted[out_idx++] = str[i];
      }
    }
    unquoted[out_idx] = '\0';
    cJSON* node = cJSON_CreateString(unquoted);
    free(unquoted);
    return node;
  }

  char* endptr = NULL;
  double dval = strtod(str, &endptr);
  if (endptr && *endptr == '\0') {
    return cJSON_CreateNumber(dval);
  }

  return cJSON_CreateString(str);
}

typedef struct {
  int indent;
  cJSON* node;
  bool is_array;
} stack_frame_t;

cJSON* cdsp_yaml_to_json(const char* yaml_str, char** out_err) {
  if (!yaml_str) {
    if (out_err) *out_err = strdup("Null input YAML string");
    return NULL;
  }

  const char* p = yaml_str;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '{' || *p == '[') {
    cJSON* json = cJSON_Parse(yaml_str);
    if (!json && out_err) {
      *out_err = strdup("Failed to parse inline JSON/YAML string");
    }
    return json;
  }

  cJSON* root = NULL;
  stack_frame_t stack[64];
  int depth = 0;

  char line[4096];
  const char* cur = yaml_str;

  while (*cur) {
    const char* next_line = strchr(cur, '\n');
    size_t line_len = next_line ? (size_t)(next_line - cur) : strlen(cur);
    if (line_len >= sizeof(line)) {
      if (out_err)
        *out_err =
            strdup("YAML line exceeds maximum length of 4095 characters");
      if (root) cJSON_Delete(root);
      return NULL;
    }
    memcpy(line, cur, line_len);
    line[line_len] = '\0';
    cur = next_line ? next_line + 1 : cur + line_len;

    int indent = 0;
    while (line[indent] == ' ') indent++;
    char* content = line + indent;

    char* comment = strchr(content, '#');
    if (comment) {
      bool in_quote = false;
      char q_char = 0;
      for (char* s = content; s < comment; ++s) {
        if ((*s == '"' || *s == '\'') && (!in_quote || q_char == *s)) {
          in_quote = !in_quote;
          q_char = in_quote ? *s : 0;
        }
      }
      if (!in_quote) *comment = '\0';
    }

    content = trim_str(content);
    if (*content == '\0') continue;

    bool is_list_item =
        (content[0] == '-' && (content[1] == ' ' || content[1] == '\0'));

#define PUSH_STACK(node_ptr, indent_val, array_flag)                           \
  do {                                                                         \
    if (depth >= 64) {                                                         \
      if (out_err)                                                             \
        *out_err = strdup(                                                     \
            "YAML document nesting depth exceeds maximum of 63 levels");       \
      if (root) cJSON_Delete(root);                                            \
      return NULL;                                                             \
    }                                                                          \
    stack[depth] = (stack_frame_t){                                            \
        .indent = (indent_val), .node = (node_ptr), .is_array = (array_flag)}; \
    depth++;                                                                   \
  } while (0)

#define CHECK_OOM(ptr)                                                     \
  do {                                                                     \
    if (!(ptr)) {                                                          \
      if (out_err) *out_err = strdup("Out of memory during YAML parsing"); \
      if (root) cJSON_Delete(root);                                        \
      return NULL;                                                         \
    }                                                                      \
  } while (0)

    if (depth == 0) {
      if (is_list_item) {
        root = cJSON_CreateArray();
        CHECK_OOM(root);
        stack[0] =
            (stack_frame_t){.indent = indent, .node = root, .is_array = true};
      } else {
        root = cJSON_CreateObject();
        CHECK_OOM(root);
        stack[0] =
            (stack_frame_t){.indent = indent, .node = root, .is_array = false};
      }
      depth = 1;
    } else {
      while (depth > 1 && indent < stack[depth - 1].indent) {
        depth--;
      }
    }

    stack_frame_t* top = &stack[depth - 1];

    if (is_list_item) {
      if (top->node && (top->node->type & 0xFF) == cJSON_Object &&
          top->node->child == NULL) {
        top->node->type = cJSON_Array;
        top->is_array = true;
      }
      char* item_val = trim_str(content + 1);
      if (*item_val == '\0') {
        cJSON* new_obj = cJSON_CreateObject();
        CHECK_OOM(new_obj);
        cJSON_AddItemToArray(top->node, new_obj);
        PUSH_STACK(new_obj, indent + 2, false);
      } else {
        char* colon = strchr(item_val, ':');
        char* first_q = strpbrk(item_val, "\"'");
        if (colon && (!first_q || colon < first_q)) {
          cJSON* new_obj = cJSON_CreateObject();
          CHECK_OOM(new_obj);
          cJSON_AddItemToArray(top->node, new_obj);
          *colon = '\0';
          char* key = trim_str(item_val);
          char* val = trim_str(colon + 1);
          if (*val == '\0') {
            cJSON* child = cJSON_CreateObject();
            CHECK_OOM(child);
            cJSON_AddItemToObject(new_obj, key, child);
            PUSH_STACK(new_obj, indent + 2, false);
            PUSH_STACK(child, indent + 4, false);
          } else {
            cJSON* scalar = parse_scalar_val(val);
            CHECK_OOM(scalar);
            cJSON_AddItemToObject(new_obj, key, scalar);
            PUSH_STACK(new_obj, indent + 2, false);
          }
        } else {
          cJSON* scalar = parse_scalar_val(item_val);
          CHECK_OOM(scalar);
          cJSON_AddItemToArray(top->node, scalar);
        }
      }
    } else {
      char* colon = strchr(content, ':');
      if (colon) {
        *colon = '\0';
        char* key = trim_str(content);
        if ((key[0] == '"' || key[0] == '\'') &&
            key[strlen(key) - 1] == key[0]) {
          key[strlen(key) - 1] = '\0';
          key++;
        }
        char* val = trim_str(colon + 1);

        if (*val == '\0') {
          cJSON* placeholder = cJSON_CreateObject();
          CHECK_OOM(placeholder);
          cJSON_AddItemToObject(top->node, key, placeholder);
          PUSH_STACK(placeholder, indent + 2, false);
        } else {
          cJSON* scalar = parse_scalar_val(val);
          CHECK_OOM(scalar);
          cJSON_AddItemToObject(top->node, key, scalar);
        }
      }
    }
  }

#undef PUSH_STACK
#undef CHECK_OOM

  if (!root) {
    if (out_err) *out_err = strdup("Empty or invalid YAML document");
    return NULL;
  }

  return root;
}
