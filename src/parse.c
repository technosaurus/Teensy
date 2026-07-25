/**
 * @file parse.c
 * @brief High-performance, dependency-free C HTML/XHTML to Flat AST Parser.
 *
 * This program reads a stream of HTML/XHTML from standard input (stdin)
 * and outputs a tab-separated flat-file Abstract Syntax Tree (AST) to
 * standard output (stdout).
 *
 * Each output line represents a single AST node (Element, Text, Comment,
 * or DocType) with its hierarchy (index, parent, depth) preserved.
 * All tabs, newlines, and backslashes in strings are escaped to ensure
 * each record maps strictly to a single line, making it fast and simple
 * for downstream tools (awk, grep, Python, etc.) to process.
 *
 * Compilation:
 *   gcc -O3 -Wall -Wextra parser.c -o html_parser
 *
 * Usage:
 *   ./html_parser < index.html
 *   cat index.html | ./html_parser --no-headers
 *
 * Options:
 *   -h, --help                 Show this help message.
 *   -n, --no-headers           Do not output the TSV header row.
 *   -w, --preserve-whitespace  Do not ignore whitespace-only text nodes.
 */
#include "layout.h"  // Include your global structure contract
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#define MAX_STACK_DEPTH 1024
#define MAX_TOKEN_LEN 1048576  // 1MB max token length for large texts or attributes
#define MAX_NAME_LEN 256

/* --- Output TSV Columns ---
 * Index        : 0-based index of the node
 * Parent       : Index of the parent element (-1 if root)
 * Depth        : Nesting depth (0 is root level)
 * Type         : ELEMENT | TEXT | COMMENT | DOCTYPE
 * TagName      : HTML tag name (or '-' if not an element)
 * Attributes   : Space-separated key="value" pairs (or '-' if none or not an element)
 * Content      : Text/comment/doctype value (or '-' if not applicable)
 */

typedef enum {
    STATE_TEXT,
    STATE_TAG_OPEN,             // after '<'
    STATE_TAG_NAME,             // reading tag name
    STATE_TAG_CLOSE_OPEN,       // after '</'
    STATE_TAG_CLOSE_NAME,       // reading close tag name
    STATE_TAG_CLOSE_SPACES,     // spaces in close tag before '>'
    STATE_ATTR_SPACES,          // spaces before/between attributes
    STATE_ATTR_NAME,            // reading attribute name
    STATE_ATTR_NAME_END,        // space after attribute name but before '='
    STATE_ATTR_EQUAL,           // after '='
    STATE_ATTR_VAL_DQ,          // double quoted value
    STATE_ATTR_VAL_SQ,          // single quoted value
    STATE_ATTR_VAL_UQ,          // unquoted value
    STATE_BANG,                 // after '<!'
    STATE_BANG_DASH,            // after '<!-'
    STATE_COMMENT,              // inside comment
    STATE_COMMENT_CLOSE_1,      // after '-' inside comment
    STATE_COMMENT_CLOSE_2,      // after '--' inside comment
    STATE_DOCTYPE,              // inside <!DOCTYPE ... >
    STATE_CDATA,                // inside CDATA
    STATE_CDATA_CLOSE_1,        // after ']' inside CDATA
    STATE_CDATA_CLOSE_2,        // after ']]' inside CDATA
    STATE_SELF_CLOSE,           // after '/' in start tag
} ParserState;

typedef struct {
    int index;
    char name[MAX_NAME_LEN];
} StackNode;

/* Stack to maintain element hierarchy */
static StackNode tag_stack[MAX_STACK_DEPTH];
static int stack_top = -1;

/* Pushback buffer for stream lookahead */
static int pushback_buf[512];
static int pushback_top = 0;

/* Output configurations */
static bool opt_headers = true;
static bool opt_preserve_whitespace = false;

/* HTML5 void elements that are implicitly self-closing */
static const char *void_elements[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr"
};
#define NUM_VOID_ELEMENTS (sizeof(void_elements) / sizeof(void_elements[0]))

static bool is_void_element(const char *name) {
    for (size_t i = 0; i < NUM_VOID_ELEMENTS; i++) {
        #ifdef _WIN32
        if (_stricmp(void_elements[i], name) == 0) return true;
        #else
        if (strcasecmp(void_elements[i], name) == 0) return true;
        #endif
    }
    return false;
}

/* Stack Operations */
static void push_tag(int index, const char *name) {
    if (stack_top < MAX_STACK_DEPTH - 1) {
        stack_top++;
        tag_stack[stack_top].index = index;
        strncpy(tag_stack[stack_top].name, name, MAX_NAME_LEN - 1);
        tag_stack[stack_top].name[MAX_NAME_LEN - 1] = '\0';
    }
}

static int pop_tag(const char *name) {
    for (int i = stack_top; i >= 0; i--) {
        #ifdef _WIN32
        bool match = (_stricmp(tag_stack[i].name, name) == 0);
        #else
        bool match = (strcasecmp(tag_stack[i].name, name) == 0);
        #endif
        if (match) {
            int popped_idx = tag_stack[i].index;
            stack_top = i - 1;
            return popped_idx;
        }
    }
    return -1;
}

static int get_parent_index() {
    return (stack_top >= 0) ? tag_stack[stack_top].index : -1;
}

static int get_current_depth() {
    return stack_top + 1;
}

/* Streaming Input Utilities */
static int next_char() {
    if (pushback_top > 0) {
        return pushback_buf[--pushback_top];
    }
    return fgetc(stdin);
}

static void push_char(int c) {
    if (c != EOF && pushback_top < 512) {
        pushback_buf[pushback_top++] = c;
    }
}

/* String Trimming and Whitespace Check */
static bool is_string_whitespace(const char *str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        if (!isspace((unsigned char)str[i])) return false;
    }
    return true;
}

/* Escaped Output Helper
 * Translates tabs, newlines, and backslashes to literal '\t', '\n', '\\'
 * to preserve single-line TSV output structure.
 */
static void print_escaped(const char *str) {
    if (!str || str[0] == '\0') {
        putchar('-');
        return;
    }
    for (size_t i = 0; str[i] != '\0'; i++) {
        char c = str[i];
        if (c == '\t') {
            printf("\\t");
        } else if (c == '\n') {
            printf("\\n");
        } else if (c == '\r') {
            printf("\\r");
        } else if (c == '\\') {
            printf("\\\\");
        } else {
            putchar(c);
        }
    }
}

/* Buffer Appender
 * Appends ' key="val"' to buf with proper escaping.
 */
static void append_attribute(char *buf, size_t max_len, const char *key, const char *val) {
    size_t cur_len = strlen(buf);
    size_t key_len = strlen(key);

    if (cur_len + key_len + 5 >= max_len) return;

    if (cur_len > 0) {
        strcat(buf, " ");
        cur_len++;
    }

    strcat(buf, key);
    cur_len += key_len;
    strcat(buf, "=\"");
    cur_len += 2;

    for (size_t i = 0; val[i] != '\0' && cur_len < max_len - 3; i++) {
        char c = val[i];
        if (c == '"') {
            if (cur_len + 2 < max_len - 3) {
                buf[cur_len++] = '\\';
                buf[cur_len++] = '"';
            }
        } else if (c == '\\') {
            if (cur_len + 2 < max_len - 3) {
                buf[cur_len++] = '\\';
                buf[cur_len++] = '\\';
            }
        } else if (c == '\t') {
            if (cur_len + 2 < max_len - 3) {
                buf[cur_len++] = '\\';
                buf[cur_len++] = 't';
            }
        } else if (c == '\n') {
            if (cur_len + 2 < max_len - 3) {
                buf[cur_len++] = '\\';
                buf[cur_len++] = 'n';
            }
        } else if (c == '\r') {
            if (cur_len + 2 < max_len - 3) {
                buf[cur_len++] = '\\';
                buf[cur_len++] = 'r';
            }
        } else {
            buf[cur_len++] = c;
        }
    }
    buf[cur_len++] = '"';
    buf[cur_len] = '\0';
}

/* Inline Character Appender */
static void append_char(char *buf, int *len, int max_len, char c) {
    if (*len < max_len - 1) {
        buf[*len] = c;
        (*len)++;
        buf[*len] = '\0';
    }
}

/* Lookahead to verify if raw text section (script/style) should terminate.
 * If match found, returns true and keeps stream pointers at the '<' position
 * to allow regular close tag parsing.
 */
static bool check_raw_text_end(const char *tag) {
    size_t tag_len = strlen(tag);
    int read_count = 0;
    int buf[256];

    // Read '/'
    int c = next_char();
    if (c == EOF) return false;
    buf[read_count++] = c;
    if (c != '/') {
        for (int i = read_count - 1; i >= 0; i--) push_char(buf[i]);
        return false;
    }

    // Read tag characters
    for (size_t i = 0; i < tag_len; i++) {
        c = next_char();
        if (c == EOF) {
            for (int j = read_count - 1; j >= 0; j--) push_char(buf[j]);
            return false;
        }
        buf[read_count++] = c;
        if (tolower(c) != tolower(tag[i])) {
            for (int j = read_count - 1; j >= 0; j--) push_char(buf[j]);
            return false;
        }
    }

    // Read separator
    c = next_char();
    if (c == EOF) {
        for (int j = read_count - 1; j >= 0; j--) push_char(buf[j]);
        return false;
    }
    buf[read_count++] = c;

    if (isspace(c) || c == '/' || c == '>') {
        // Complete match! Push back all characters to let the parser read it normally
        for (int i = read_count - 1; i >= 0; i--) {
            push_char(buf[i]);
        }
        return true;
    }

    // No match
    for (int i = read_count - 1; i >= 0; i--) {
        push_char(buf[i]);
    }
    return false;
}

/* Outputs an AST node to stdout in flat TSV format */
static void emit_node(int index, int parent, int depth, const char *type,
                      const char *name, const char *attrs, const char *content) {
    printf("%d\t%d\t%d\t%s\t", index, parent, depth, type);
    print_escaped(name);
    putchar('\t');
    print_escaped(attrs);
    putchar('\t');
    print_escaped(content);
    putchar('\n');
}

/* Helper to emit a text node if it contains non-whitespace content (or if whitespaces are preserved) */
static void emit_text_node(int *node_counter, char *token_buf, int *token_len) {
    if (*token_len > 0) {
        token_buf[*token_len] = '\0';
        if (opt_preserve_whitespace || !is_string_whitespace(token_buf)) {
            emit_node(*node_counter, get_parent_index(), get_current_depth(),
                      "TEXT", "-", "-", token_buf);
            (*node_counter)++;
        }
        *token_len = 0;
    }
}

/* Helper to emit an element, push it onto stack if needed, and handle script/style raw text logic */
static void emit_element(int *node_counter, const char *tag_name, const char *attrs,
                         bool *in_raw_text, char *raw_text_tag) {
    int parent_id = get_parent_index();
    int depth = get_current_depth();
    emit_node(*node_counter, parent_id, depth, "ELEMENT", tag_name,
              (attrs && attrs[0] != '\0') ? attrs : "-", "-");

    bool void_tag = is_void_element(tag_name);
    if (!void_tag) {
        push_tag(*node_counter, tag_name);
        #ifdef _WIN32
        bool is_raw = (_stricmp(tag_name, "script") == 0 || _stricmp(tag_name, "style") == 0);
        #else
        bool is_raw = (strcasecmp(tag_name, "script") == 0 || strcasecmp(tag_name, "style") == 0);
        #endif
        if (is_raw) {
            *in_raw_text = true;
            strncpy(raw_text_tag, tag_name, MAX_NAME_LEN - 1);
            raw_text_tag[MAX_NAME_LEN - 1] = '\0';
        }
    }
    (*node_counter)++;
}

/* Show usage help */
static void print_usage(const char *prog) {
    fprintf(stderr, "HTML/XHTML to Flat AST Parser\n");
    fprintf(stderr, "Usage: %s [options] < input.html\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -h, --help                 Show this help message.\n");
    fprintf(stderr, "  -n, --no-headers           Do not output the TSV header row.\n");
    fprintf(stderr, "  -w, --preserve-whitespace  Do not ignore whitespace-only text nodes.\n");
}

int main(int argc, char **argv) {
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-headers") == 0) {
            opt_headers = false;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--preserve-whitespace") == 0) {
            opt_preserve_whitespace = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (opt_headers) {
        printf("Index\tParent\tDepth\tType\tTagName\tAttributes\tContent\n");
    }

    ParserState state = STATE_TEXT;

    /* Re-usable buffers */
    char *token_buf = malloc(MAX_TOKEN_LEN);
    char *attr_val = malloc(MAX_TOKEN_LEN);
    char *attr_list = malloc(MAX_TOKEN_LEN);
    if (!token_buf || !attr_val || !attr_list) {
        fprintf(stderr, "Fatal error: Out of memory\n");
        return 1;
    }

    int token_len = 0;
    char tag_name[MAX_NAME_LEN] = "";
    char attr_name[MAX_NAME_LEN] = "";
    int attr_val_len = 0;

    int node_counter = 0;

    /* Raw tag handling (e.g. script, style) */
    bool in_raw_text_element = false;
    char raw_text_tag[MAX_NAME_LEN] = "";

    int c;
    while ((c = next_char()) != EOF) {
        switch (state) {
            case STATE_TEXT:
                switch (c) {
                    case '<': // 60
                        if (in_raw_text_element) {
                            if (check_raw_text_end(raw_text_tag)) {
                                emit_text_node(&node_counter, token_buf, &token_len);
                                in_raw_text_element = false;
                                raw_text_tag[0] = '\0';
                                state = STATE_TAG_OPEN;
                            } else {
                                append_char(token_buf, &token_len, MAX_TOKEN_LEN, '<');
                            }
                        } else {
                            emit_text_node(&node_counter, token_buf, &token_len);
                            state = STATE_TAG_OPEN;
                        }
                        break;
                    default:
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                        break;
                }
                break;

            case STATE_TAG_OPEN:
                switch (c) {
                    case '!': // 33
                        state = STATE_BANG;
                        break;
                    case '/': // 47
                        state = STATE_TAG_CLOSE_OPEN;
                        break;
                    case '?': // 63
                        state = STATE_DOCTYPE;
                        break;
                    default:
                        if (isalpha(c) || c == ':' || c == '_') {
                            tag_name[0] = '\0';
                            int tag_len = 0;
                            append_char(tag_name, &tag_len, MAX_NAME_LEN, c);
                            attr_list[0] = '\0';
                            state = STATE_TAG_NAME;
                        } else {
                            append_char(token_buf, &token_len, MAX_TOKEN_LEN, '<');
                            append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                            state = STATE_TEXT;
                        }
                        break;
                }
                break;

            case STATE_TAG_NAME:
                switch (c) {
                    case '\t': // 9
                    case '\n': // 10
                    case '\v': // 11
                    case '\f': // 12
                    case '\r': // 13
                    case ' ':  // 32
                        state = STATE_ATTR_SPACES;
                        break;
                    case '/':  // 47
                        state = STATE_SELF_CLOSE;
                        break;
                    case '>':  // 62
                        emit_element(&node_counter, tag_name, "", &in_raw_text_element, raw_text_tag);
                        state = STATE_TEXT;
                        break;
                    default: {
                        int len = (int)strlen(tag_name);
                        append_char(tag_name, &len, MAX_NAME_LEN, c);
                        break;
                    }
                }
                break;

            case STATE_TAG_CLOSE_OPEN:
                if (isalpha(c) || c == ':' || c == '_') {
                    tag_name[0] = '\0';
                    int tag_len = 0;
                    append_char(tag_name, &tag_len, MAX_NAME_LEN, c);
                    state = STATE_TAG_CLOSE_NAME;
                } else {
                    // Malformed closing tag, revert to text
                    append_char(token_buf, &token_len, MAX_TOKEN_LEN, '<');
                    append_char(token_buf, &token_len, MAX_TOKEN_LEN, '/');
                    append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                    state = STATE_TEXT;
                }
                break;

            case STATE_TAG_CLOSE_NAME:
                switch (c) {
                    case '\t': // 9
                    case '\n': // 10
                    case '\v': // 11
                    case '\f': // 12
                    case '\r': // 13
                    case ' ':  // 32
                        state = STATE_TAG_CLOSE_SPACES;
                        break;
                    case '>':  // 62
                        pop_tag(tag_name);
                        state = STATE_TEXT;
                        break;
                    default: {
                        int len = (int)strlen(tag_name);
                        append_char(tag_name, &len, MAX_NAME_LEN, c);
                        break;
                    }
                }
                break;

            case STATE_TAG_CLOSE_SPACES:
                switch (c) {
                    case '>': // 62
                        pop_tag(tag_name);
                        state = STATE_TEXT;
                        break;
                    default:
                        // Skip malformed contents in closing tags
                        break;
                }
                break;

            case STATE_ATTR_SPACES:
                switch (c) {
                    case '\t': // 9
                    case '\n': // 10
                    case '\v': // 11
                    case '\f': // 12
                    case '\r': // 13
                    case ' ':  // 32
                        // skip spaces
                        break;
                    case '/':  // 47
                        state = STATE_SELF_CLOSE;
                        break;
                    case '>':  // 62
                        emit_element(&node_counter, tag_name, attr_list, &in_raw_text_element, raw_text_tag);
                        state = STATE_TEXT;
                        break;
                    default:
                        attr_name[0] = '\0';
                        int attr_len = 0;
                        append_char(attr_name, &attr_len, MAX_NAME_LEN, c);
                        state = STATE_ATTR_NAME;
                        break;
                }
                break;

            case STATE_ATTR_NAME:
                switch (c) {
                    case '\t': // 9
                    case '\n': // 10
                    case '\v': // 11
                    case '\f': // 12
                    case '\r': // 13
                    case ' ':  // 32
                        state = STATE_ATTR_NAME_END;
                        break;
                    case '/':  // 47
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, "");
                        state = STATE_SELF_CLOSE;
                        break;
                    case '=':  // 61
                        state = STATE_ATTR_EQUAL;
                        break;
                    case '>':  // 62
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, "");
                        emit_element(&node_counter, tag_name, attr_list, &in_raw_text_element, raw_text_tag);
                        state = STATE_TEXT;
                        break;
                    default: {
                        int len = (int)strlen(attr_name);
                        append_char(attr_name, &len, MAX_NAME_LEN, c);
                        break;
                    }
                }
                break;

            case STATE_ATTR_NAME_END:
                switch (c) {
                    case '\t': // 9
                    case '\n': // 10
                    case '\v': // 11
                    case '\f': // 12
                    case '\r': // 13
                    case ' ':  // 32
                        // keep skipping spaces
                        break;
                    case '/':  // 47
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, "");
                        state = STATE_SELF_CLOSE;
                        break;
                    case '=':  // 61
                        state = STATE_ATTR_EQUAL;
                        break;
                    case '>':  // 62
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, "");
                        emit_element(&node_counter, tag_name, attr_list, &in_raw_text_element, raw_text_tag);
                        state = STATE_TEXT;
                        break;
                    default:
                        // Previous attribute has no value, start of next attribute
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, "");
                        attr_name[0] = '\0';
                        int attr_len = 0;
                        append_char(attr_name, &attr_len, MAX_NAME_LEN, c);
                        state = STATE_ATTR_NAME;
                        break;
                }
                break;

            case STATE_ATTR_EQUAL:
                switch (c) {
                    case '\t': // 9
                    case '\n': // 10
                    case '\v': // 11
                    case '\f': // 12
                    case '\r': // 13
                    case ' ':  // 32
                        // skip spaces after '='
                        break;
                    case '"':  // 34
                        attr_val[0] = '\0';
                        attr_val_len = 0;
                        state = STATE_ATTR_VAL_DQ;
                        break;
                    case '\'': // 39
                        attr_val[0] = '\0';
                        attr_val_len = 0;
                        state = STATE_ATTR_VAL_SQ;
                        break;
                    case '>':  // 62
                        // Malformed empty value at end of tag
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, "");
                        emit_element(&node_counter, tag_name, attr_list, &in_raw_text_element, raw_text_tag);
                        state = STATE_TEXT;
                        break;
                    default:
                        attr_val[0] = '\0';
                        attr_val_len = 0;
                        append_char(attr_val, &attr_val_len, MAX_TOKEN_LEN, c);
                        state = STATE_ATTR_VAL_UQ;
                        break;
                }
                break;

            case STATE_ATTR_VAL_DQ:
                switch (c) {
                    case '"': // 34
                        attr_val[attr_val_len] = '\0';
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, attr_val);
                        state = STATE_ATTR_SPACES;
                        break;
                    default:
                        append_char(attr_val, &attr_val_len, MAX_TOKEN_LEN, c);
                        break;
                }
                break;

            case STATE_ATTR_VAL_SQ:
                switch (c) {
                    case '\'': // 39
                        attr_val[attr_val_len] = '\0';
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, attr_val);
                        state = STATE_ATTR_SPACES;
                        break;
                    default:
                        append_char(attr_val, &attr_val_len, MAX_TOKEN_LEN, c);
                        break;
                }
                break;

            case STATE_ATTR_VAL_UQ:
                switch (c) {
                    case '\t': // 9
                    case '\n': // 10
                    case '\v': // 11
                    case '\f': // 12
                    case '\r': // 13
                    case ' ':  // 32
                        attr_val[attr_val_len] = '\0';
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, attr_val);
                        state = STATE_ATTR_SPACES;
                        break;
                    case '/':  // 47
                        attr_val[attr_val_len] = '\0';
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, attr_val);
                        state = STATE_SELF_CLOSE;
                        break;
                    case '>':  // 62
                        attr_val[attr_val_len] = '\0';
                        append_attribute(attr_list, MAX_TOKEN_LEN, attr_name, attr_val);
                        emit_element(&node_counter, tag_name, attr_list, &in_raw_text_element, raw_text_tag);
                        state = STATE_TEXT;
                        break;
                    default:
                        append_char(attr_val, &attr_val_len, MAX_TOKEN_LEN, c);
                        break;
                }
                break;

            case STATE_SELF_CLOSE:
                switch (c) {
                    case '>': // 62
                        emit_node(node_counter, get_parent_index(), get_current_depth(), "ELEMENT", tag_name,
                                  (attr_list[0] != '\0') ? attr_list : "-", "-");
                        node_counter++;
                        state = STATE_TEXT;
                        break;
                    default:
                        // Slash wasn't immediately followed by '>'
                        // Treat as normal spaces search, push back c
                        push_char(c);
                        state = STATE_ATTR_SPACES;
                        break;
                }
                break;

            case STATE_BANG:
                switch (c) {
                    case '-': // 45
                        state = STATE_BANG_DASH;
                        break;
                    case '[': { // 91
                        // Check CDATA prefix (CDATA[)
                        int read_count = 0;
                        int buf[10];
                        const char *cdata_prefix = "CDATA[";
                        bool match = true;
                        for (int i = 0; i < 6; i++) {
                            int ch = next_char();
                            if (ch == EOF) {
                                match = false;
                                break;
                            }
                            buf[read_count++] = ch;
                            if (ch != cdata_prefix[i]) {
                                match = false;
                                break;
                            }
                        }
                        if (match) {
                            token_len = 0;
                            state = STATE_CDATA;
                        } else {
                            // Revert lookahead
                            for (int i = read_count - 1; i >= 0; i--) {
                                push_char(buf[i]);
                            }
                            // Treat as doctype or raw declaration
                            token_len = 0;
                            append_char(token_buf, &token_len, MAX_TOKEN_LEN, '!');
                            append_char(token_buf, &token_len, MAX_TOKEN_LEN, '[');
                            state = STATE_DOCTYPE;
                        }
                        break;
                    }
                    default:
                        token_len = 0;
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                        state = STATE_DOCTYPE;
                        break;
                }
                break;

            case STATE_BANG_DASH:
                switch (c) {
                    case '-': // 45
                        token_len = 0;
                        state = STATE_COMMENT;
                        break;
                    default:
                        token_len = 0;
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, '!');
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, '-');
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                        state = STATE_DOCTYPE;
                        break;
                }
                break;

            case STATE_COMMENT:
                switch (c) {
                    case '-': // 45
                        state = STATE_COMMENT_CLOSE_1;
                        break;
                    default:
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                        break;
                }
                break;

            case STATE_COMMENT_CLOSE_1:
                switch (c) {
                    case '-': // 45
                        state = STATE_COMMENT_CLOSE_2;
                        break;
                    default:
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, '-');
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                        state = STATE_COMMENT;
                        break;
                }
                break;

            case STATE_COMMENT_CLOSE_2:
                switch (c) {
                    case '-': // 45
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, '-');
                        break;
                    case '>': // 62
                        token_buf[token_len] = '\0';
                        emit_node(node_counter, get_parent_index(), get_current_depth(),
                                  "COMMENT", "-", "-", token_buf);
                        node_counter++;
                        token_len = 0;
                        state = STATE_TEXT;
                        break;
                    default:
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, '-');
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, '-');
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                        state = STATE_COMMENT;
                        break;
                }
                break;

            case STATE_DOCTYPE:
                switch (c) {
                    case '>': // 62
                        token_buf[token_len] = '\0';
                        emit_node(node_counter, get_parent_index(), get_current_depth(),
                                  "DOCTYPE", "-", "-", token_buf);
                        node_counter++;
                        token_len = 0;
                        state = STATE_TEXT;
                        break;
                    default:
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                        break;
                }
                break;

            case STATE_CDATA:
                switch (c) {
                    case ']': // 93
                        state = STATE_CDATA_CLOSE_1;
                        break;
                    default:
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                        break;
                }
                break;

            case STATE_CDATA_CLOSE_1:
                switch (c) {
                    case ']': // 93
                        state = STATE_CDATA_CLOSE_2;
                        break;
                    default:
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, ']');
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                        state = STATE_CDATA;
                        break;
                }
                break;

            case STATE_CDATA_CLOSE_2:
                switch (c) {
                    case '>': // 62
                        // CDATA completed, treat as text
                        emit_text_node(&node_counter, token_buf, &token_len);
                        state = STATE_TEXT;
                        break;
                    case ']': // 93
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, ']');
                        break;
                    default:
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, ']');
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, ']');
                        append_char(token_buf, &token_len, MAX_TOKEN_LEN, c);
                        state = STATE_CDATA;
                        break;
                }
                break;
        }
    }

    /* Emit any remaining text at EOF */
    emit_text_node(&node_counter, token_buf, &token_len);

    free(token_buf);
    free(attr_val);
    free(attr_list);
    return 0;
}

void stream_tlv_to_file(int socket_fd, SharedPageBuffer *page) {
    uint8_t header_buf[4]; // 1-Byte Tag, 1-Byte Reserved, 2-Byte Length

    // Loop continuously until the network stream ends
    while (read(socket_fd, header_buf, 4) == 4) {
        uint8_t tag     = header_buf[0];
        uint16_t length = (header_buf[2] << 8) | header_buf[3];

        // Ensure we avoid heap allocations entirely by streaming bytes straight into our mapped array zones
        if (tag == 0x01) { // E.g., Tag 0x01: A raw CSS or text node string chunk
            if (page->string_arena_tail + length < 65536) {
                // Point the node's offset to the current tail address
                uint32_t current_offset = page->string_arena_tail;

                // Stream the remaining value bytes directly from the socket into the pre-allocated string arena
                read(socket_fd, &page->string_arena[current_offset], length);
                
                // Finalize entry formatting
                page->string_arena[current_offset + length] = '\0';
                page->string_arena_tail += (length + 1);
            }
        } 
        else if (tag == 0x02) { // E.g., Tag 0x02: An explicit Layout Node control struct
            if (page->node_count < MAX_NODES) {
                // Populate the next array slot layout !#!!#geometry directly from the stream
                read(socket_fd, &page->nodes[page->node_count], sizeof(LayoutNode));
                page->node_count++;
            }
        }
    }
}



