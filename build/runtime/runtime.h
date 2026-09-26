// Shared declarations for the runtime that nio links into every binary. The
// rt_* symbols that codegen emits must match the definitions here.
//
// Value representation, shared with codegen: every value is passed and
// computed as one 8-byte unit. The integers, bool, DateTime and Duration are
// int64_t. The floats are double. All other values are pointers. A stored
// value takes its own width (§5.7). C converts between the two forms only
// through rt_arr_get/set, rt_rec_get/set, rt_map_key_at/val_at and
// rt_box/rt_box_get, which take and return the unit.
//
// The TypeDesc kind codes must agree with codegen.
//
// runtime.c is the core. Each built-in module has a file under lib/, linked
// only when a program imports it.

#ifndef NIO_RUNTIME_H
#define NIO_RUNTIME_H

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
// The MSVC CRT marks getenv, fopen and sscanf deprecated in favour of its own
// _s variants. The runtime uses the standard functions on all platforms.
#define _CRT_SECURE_NO_WARNINGS

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// ---- panic / memory ----

void rt_panic(const char *msg);
// rt_error_abort stops the program with the message of an uncaught Error
// (§2.9).
void rt_error_abort(void *err);
// rt_error_new builds the two-slot Error record (§2.9). A fallible runtime
// function stores one through its `void **err` out-parameter and does not
// panic. `code` is a NIO_ERR_* value, never a raw errno.
void *rt_error_new(const char *msg, int64_t code);

// Values for the `code` field of an Error. The ErrorCode enums that the
// built-in modules export must use the same numbers.
enum {
    NIO_ERR_NONE = 0, // the code of Error("...") with no code argument
    NIO_ERR_OTHER = 1,
    NIO_ERR_NOT_FOUND = 2,
    NIO_ERR_PERMISSION = 3,
    NIO_ERR_EXISTS = 4,
    NIO_ERR_NOT_DIRECTORY = 5,
    NIO_ERR_IS_DIRECTORY = 6,
    NIO_ERR_NOT_EMPTY = 7,
    NIO_ERR_INVALID = 8, // malformed input: string.toInt, json.parse
    NIO_ERR_IO = 9,
    NIO_ERR_NO_SPACE = 10,
    NIO_ERR_TOO_MANY_FILES = 11,
    NIO_ERR_NAME_TOO_LONG = 12,
    NIO_ERR_INTERRUPTED = 13,
    NIO_ERR_END_OF_FILE = 14,
    NIO_ERR_LOOP = 15,
    NIO_ERR_READ_ONLY = 16,
    // The network codes (§6.15) start here. fs, string and net share this
    // set.
    NIO_ERR_CONNECTION_REFUSED = 17,
    NIO_ERR_CONNECTION_RESET = 18,
    NIO_ERR_CONNECTION_ABORTED = 19,
    NIO_ERR_NOT_CONNECTED = 20,
    NIO_ERR_ALREADY_CONNECTED = 21,
    NIO_ERR_ADDRESS_IN_USE = 22,
    NIO_ERR_ADDRESS_NOT_AVAILABLE = 23,
    NIO_ERR_NETWORK_UNREACHABLE = 24,
    NIO_ERR_HOST_UNREACHABLE = 25,
    NIO_ERR_BROKEN_PIPE = 26,
    NIO_ERR_MESSAGE_TOO_LONG = 27,
    // Not an errno. The library raises it when an operation reaches its
    // `timeout` (§6.15).
    NIO_ERR_TIMED_OUT = 28,
    // Not an errno. A message that did not authenticate (§6.17). It is
    // separate from INVALID because a handler treats a forgery and
    // unreadable input differently.
    NIO_ERR_AUTHENTICATION = 29
};

// rt_err_from_errno maps a platform errno onto the NIO_ERR_* set.
int64_t rt_err_from_errno(int e);
void *rt_alloc(int64_t n);

// ---- strings (immutable, length-prefixed) ----

typedef struct {
    int64_t len;
    char data[];
} Str;

Str *rt_str_alloc(int64_t len);
// Decodes the UTF-8 sequence at byte offset off. The caller keeps off in
// range. It returns the code point and writes its width through width. A
// byte that starts no valid sequence decodes as U+FFFD, one byte wide, so a
// walk visits each byte once and always ends. It is in the core because
// forEach over a String (§4.7) needs no import.
int64_t rt_str_decode(Str *s, int64_t off, int64_t *width);
Str *rt_str_concat(Str *a, Str *b);

// N-ary concatenation for a chain of `+`. `parts` must point to n consecutive
// slots of the GC frame of the caller (genConcatChain does this). Thus the
// operands are roots already, and this function pushes no frame. Operands in
// a C-local array are not visible to the collector.
Str *rt_str_concat_n(Str **parts, int64_t n);
int64_t rt_str_eq(Str *a, Str *b);
int64_t rt_str_cmp(Str *a, Str *b);
Str *rt_str_from_c(const char *c);

// s[i] (§3.5): the byte at i, zero-extended (`byte` is uint8). An index out
// of range panics. It is a call so that the bounds check stays in one basic
// block for --coverage. -flto inlines it into the caller.
int64_t rt_str_byte(Str *s, int64_t i);

// ---- arrays, with packed elements ----
//
// The header does not move while the array lives. A push can replace the
// element block. Only the first len elements hold values. Tracing, printing
// and serialization must not read past len.
//
// An element takes its own width (§5.7). `width` is in the header because
// rt_arr_slot has no descriptor. C reads and writes an element through
// rt_arr_get/rt_arr_set, which take and return the 8-byte unit.

typedef struct {
    int64_t len;
    int64_t cap;    // elements allocated; len <= cap
    int64_t width;  // bytes per element: 1, 2, 4 or 8
    void *data;     // cap * width bytes
} Arr;

// rt_arr_width returns the width that §5.7 gives to a TypeDesc kind.
int64_t rt_arr_width(int64_t elem_kind);

Arr *rt_arr_new(int64_t len, int64_t width);
int64_t rt_arr_len(Arr *a);

// rt_arr_slot returns the address of element i after a bounds check. It is a
// call for the same reason as rt_str_byte.
void *rt_arr_slot(Arr *a, int64_t i);

// Two views of an element block, for C code that knows the width: units for
// 8-byte elements, bytes for a byte[]. Neither view checks the width. A call
// site that cannot know the view must use rt_arr_get.
static inline int64_t *rt_arr_units(Arr *a) { return (int64_t *)a->data; }
static inline uint8_t *rt_arr_bytes(Arr *a) { return (uint8_t *)a->data; }

// ---- maps, which keep insertion order ----
//
// The header does not move while the map lives. The three blocks it points to
// can change as the map grows. `keys` and `vals` hold the live entries in
// insertion order, each at the width of its type (§5.7). `index` is a
// power-of-two table of entry positions + 1 (0 is empty), with linear
// probing by key hash. A removal compacts the entry blocks, keeps the order,
// and rebuilds the index. Nothing must read past len.
//
// A Map<K, V> TypeDesc (TD_MAP) holds the key and value descriptors in
// field_types[0] and [1]. String keys hash and compare by content. All other
// keys are int64 scalars that compare by value.
//
// The core holds new, len, get and set. lib/map.c adds the module functions.

typedef struct {
    int64_t len;    // live entries: keys[0..len) and vals[0..len)
    int64_t cap;    // entry-block capacity, in entries; 0 until the first insert
    void *keys;     // keys, insertion order, each at the key type's width
    void *vals;     // values, same order, each at the value type's width
    int64_t icap;   // index-table size, a power of two; 0 while cap is 0
    int64_t *index; // per slot: entry position + 1, or 0 if empty
} Map;

// ---- type descriptors, which must agree with codegen ----

typedef struct TypeDesc {
    int64_t kind;
    struct TypeDesc *elem; // array element type, or optional inner type
    int64_t nfields;
    const char **field_names;
    struct TypeDesc **field_types;
    // TD_RECORD only: 1 + the index of the `json*` field (§6.3), or 0 if
    // none. The JSON reader collects undeclared keys into that field. The
    // JSON writer writes them beside the declared fields.
    int64_t rest;
    // TD_RECORD only: the byte offset of each field, and the total block
    // size, from recOffset/recSizeOf in src/codegen.nio. The first field is
    // at offset 8. Each next field starts at the end of the previous one,
    // rounded up to a multiple of its own width. The total is rounded up to a
    // multiple of 8. A record TypeDesc written by hand in C must fill both.
    // rt_rec_new checks them.
    const int64_t *field_offsets;
    int64_t size;
    // TD_RECORD only: the TD_UNION descriptor if this record is a union
    // member (§2.11), else NULL. The JSON writer then writes only the payload
    // (field 0), so the generated `$v` field does not appear.
    struct TypeDesc *uni;
} TypeDesc;

// ---- record layout, which must agree with codegen.nio ----
//
// Each record payload starts with a pointer to its own TypeDesc. The declared
// fields follow, each at its own width at field_offsets[i]. The collector
// traces the descriptor in the object and ignores the static one at the root.
// Thus the extra reference fields of an extending type stay traced in a
// base-typed slot (§2.4). An extending type puts the base fields first, so
// the base offsets are also correct for it.
//
// The descriptor slot is NULL only while the runtime fills in a block. Every
// walk must read NULL there as "not yet a record" and stop.
//
// C reads and writes a field through rt_rec_get/rt_rec_set, which take and
// return the 8-byte unit.
#define REC_TD(p)     (*(TypeDesc **)(p))

// rt_rec_new allocates a cleared record with the shape of td and writes td
// into slot [0]. All records that C builds go through it.
void *rt_rec_new(TypeDesc *td);

enum {
    TD_INT8 = 0,
    TD_INT = 1,
    TD_FLOAT = 2,
    TD_STRING = 3,
    TD_BOOL = 4,
    TD_DATETIME = 5,
    TD_ARRAY = 6,
    TD_OPTIONAL = 7,
    TD_RECORD = 8,
    // Internal to the runtime. Codegen does not emit it. An opaque heap block
    // that the collector keeps alive and does not walk.
    TD_BLOCK = 9,
    // A function value: NULL (the zero value, a call panics) or a closure
    // block of 8-byte slots: [0] the code pointer, [1] a TypeDesc* for the
    // captures, [2..] one box pointer for each captured variable. Captures
    // are by reference. The block describes itself, so the static
    // rt_td_func covers all function types.
    TD_FUNC = 10,
    // A future (Future<T>): NULL (the zero value, an await panics) or a
    // Future* (below). It describes itself, so the static rt_td_future
    // covers all future types.
    TD_FUTURE = 11,
    // An enum value: an int64 scalar, never a pointer. The descriptor uses
    // the record slots for the member table: nfields is the member count,
    // field_names holds the member names, and field_types points to an
    // [nfields x int64_t] array of the member values. print shows the member
    // name. JSON writes the number.
    TD_ENUM = 12,
    // The narrow signed numbers. Each has the representation of the 64-bit
    // type of its family. The compiler keeps the value in range. The JSON
    // reader checks the range. print shows a float32 at binary32 precision.
    TD_INT16 = 13,
    TD_INT32 = 14,
    TD_FLOAT32 = 15,
    // A map (Map<K, V>): a pointer to a Map header. field_types[0] is the key
    // descriptor and [1] the value descriptor. nfields is 2. elem and
    // field_names are not used.
    TD_MAP = 16,
    // A Duration: a signed int64 count of milliseconds, with no fixed origin.
    // print and JSON use the number as it is.
    TD_DURATION = 17,
    // A Json (§6.3): NULL (read as MISSING) or a JsonNode* (below). The node
    // kind tells which slots hold pointers. Thus the static rt_td_json covers
    // all Json values, and the trace case in gc.c dispatches on the node.
    TD_JSON = 18,
    // A RegExp (§6.14): NULL (use panics) or a compiled Regexp block with no
    // pointers. The collector only keeps it alive, as for TD_BLOCK.
    TD_REGEXP = 19,
    // A block of nfields 8-byte slots from offset 0, described from outside
    // by field_types. An async frame has this shape. Its slot [0] is the
    // resume state and is not a descriptor, so nothing must trace such a
    // block as a record. Every new block of slots described from outside
    // must use TD_SLOTS and never TD_RECORD.
    TD_SLOTS = 20,
    // The unsigned integers (§2.1). Codegen zero-extends each unsigned result
    // into its 8-byte unit, so the runtime reads the unit with no mask. print
    // and JSON write them with no sign. The JSON reader checks the range 0 to
    // 2^n-1. TD_UINT is the 64-bit kind and also covers `uint`.
    TD_UINT8 = 21,
    TD_UINT16 = 22,
    TD_UINT32 = 23,
    TD_UINT = 24,
    // A Socket or a Listener (§6.15): NULL (use raises an Error) or a
    // NetSock* (below). The block holds an OS descriptor and two C pointers
    // into memory that lib/net.c owns. The collector must keep it alive and
    // never walk it. The weak hook closes it when nothing points to it. One
    // code covers both types. Only the checker makes them different.
    TD_SOCKET = 25,
    // The base type of a union (§2.11). It uses the record slots as TD_ENUM
    // does: nfields is the member count, field_names holds the member tags,
    // and field_types holds the TD_RECORD descriptors of the members. Each
    // member descriptor points back through `uni`. A value of this kind
    // always points to the record of a member, so the collector traces it as
    // TD_RECORD. lib/json.c uses this kind to select the member from the kind
    // of the document value.
    TD_UNION = 26
};

// Whether a kind is an unsigned integer kind. The kind codes are not in a
// meaningful order, so code must use this test and not a range comparison.
#define TD_IS_UNSIGNED(k) \
    ((k) == TD_UINT8 || (k) == TD_UINT16 || (k) == TD_UINT32 || (k) == TD_UINT)

// A new kind that can hold a pointer must also go into TD_POINTER_KINDS in
// gc.c. The collector does not trace a kind that is not in that mask, and
// sweeps its referents while they are still reachable. The mask limits the
// codes to 31. gc.c asserts this.

// ---- Json (§6.3) ----

// The kinds of a Json value. MISSING is 0, so a NULL JsonNode* reads as
// MISSING with no allocation. MISSING and NULL are different because JSON
// makes an absent key different from a null one.
enum {
    JSON_MISSING = 0,
    JSON_NULL = 1,
    JSON_BOOL = 2,
    JSON_NUMBER = 3,
    JSON_STRING = 4,
    JSON_ARRAY = 5,
    JSON_OBJECT = 6
};

// A growable slot vector. It holds the elements of a Json array, and the
// parallel key and value vectors of a Json object. Its slots are 8-byte units
// that the kind of the owner node describes. Growth replaces the block, so a
// copy of the vector pointer is not valid after an append. Always read the
// pointer from the node.
typedef struct JsonVec {
    int64_t len;
    int64_t cap;
    int64_t slots[];
} JsonVec;

// One JSON value. All kinds use the same slots, so a node can change its kind
// in place when a program assigns over it:
//
//   kind    num                     a                     b
//   BOOL    0 or 1                  unused                unused
//   NUMBER  the bits of the double  unused                unused
//   STRING  unused                  Str *                 unused
//   ARRAY   unused                  JsonVec * of JsonNode unused
//   OBJECT  unused                  JsonVec * of Str      JsonVec * of JsonNode
//
// An object keeps its keys and its values in two parallel vectors, in
// document order.
//
// `isint` keeps a large integer exact. A NUMBER node records which of three
// forms it holds, and converts only when a program asks for a different form:
// 0 is a double in the bits of `num`, 1 is a signed int64, and 2 is an
// unsigned int64 (JN_INT and JN_UINT in lib/json.c). The unsigned form keeps
// a value above the signed range, such as 2^64-1, unchanged.
typedef struct JsonNode {
    int64_t kind;
    int64_t num;
    void *a;
    void *b;
    int64_t isint;
} JsonNode;

// The descriptors for the types whose shape does not change between programs.
// Codegen also refers to these, and emits %TD constants for all other types.
extern TypeDesc rt_td_int8;
extern TypeDesc rt_td_int16;
extern TypeDesc rt_td_int32;
extern TypeDesc rt_td_int;
extern TypeDesc rt_td_uint8;
extern TypeDesc rt_td_uint16;
extern TypeDesc rt_td_uint32;
extern TypeDesc rt_td_uint;
extern TypeDesc rt_td_float32;
extern TypeDesc rt_td_float;
extern TypeDesc rt_td_string;
extern TypeDesc rt_td_bool;
extern TypeDesc rt_td_datetime;
extern TypeDesc rt_td_duration;
extern TypeDesc rt_td_block;
extern TypeDesc rt_td_func;
extern TypeDesc rt_td_future;
extern TypeDesc rt_td_error;
extern TypeDesc rt_td_json;
extern TypeDesc rt_td_regexp;
extern TypeDesc rt_td_socket;

// ---- Json operations (lib/json.c) ----
//
// Each function accepts a NULL node and reads it as MISSING, so a call site
// needs no guard for a Json that has no assigned value.

// json.getType(v) returns the JSON_* kind, as a json.Type enum value.
int64_t rt_json_kind(JsonNode *v);
// Navigation. Both return NULL (MISSING) and do not fail, so a chain such as
// raw["a"]["b"] does not stop at an absent key.
JsonNode *rt_json_get(JsonNode *v, Str *key);
JsonNode *rt_json_at(JsonNode *v, int64_t i);
// Assignment. Each function stores a deep copy of the node the caller built.
// Thus no two places in a tree share a node, and `raw["a"] = raw` makes no
// cycle for the serializer. A write into a value of the wrong kind (not an
// OBJECT, or not an ARRAY), and a write through a MISSING, panic.
void rt_json_set(JsonNode *v, Str *key, JsonNode *val);
void rt_json_set_at(JsonNode *v, int64_t i, JsonNode *val);
void rt_json_push(JsonNode *v, JsonNode *val);
void rt_json_remove(JsonNode *v, Str *key);
// json.keys(v) returns the keys of an object in insertion order, and an empty
// array for all other kinds. json.length(v) returns the size of an array or
// an object, and 0 for all other kinds.
Arr *rt_json_keys(JsonNode *v);
int64_t rt_json_length(JsonNode *v);
// Extraction. Each function stores an Error through err when the node is not
// the requested kind. It uses the out-parameter convention of lib/fs.c.
int64_t rt_json_as_int(JsonNode *v, void **err);
double rt_json_as_float(JsonNode *v, void **err);
Str *rt_json_as_text(JsonNode *v, void **err);
int64_t rt_json_as_bool(JsonNode *v, void **err);
JsonNode *rt_json_of(int64_t raw, TypeDesc *td);
JsonNode *rt_json_empty(int64_t kind);
// The two forms of `as T`: from text, and from a tree that the program holds.
int64_t rt_json_parse_typed(Str *text, TypeDesc *td, int64_t strict, void **err);
int64_t rt_json_from_node(JsonNode *v, TypeDesc *td, int64_t strict, void **err);
JsonNode *rt_json_parse_value(Str *text, void **err);
// The byte[] forms of both parses. They read the Arr data block in place, so
// a document that arrived as bytes needs no copy.
int64_t rt_json_parse_typed_b(Arr *bytes, TypeDesc *td, int64_t strict, void **err);
JsonNode *rt_json_parse_value_b(Arr *bytes, void **err);

// ---- generic operations over TypeDescs ----

// Element i as its 8-byte unit, and the reverse. `elem` tells how the stored
// bits become the unit: sign extension, zero extension for the unsigned
// family (§2.1), or widening from binary32. An 8-byte element is already the
// unit.
int64_t rt_arr_get(Arr *a, int64_t i, TypeDesc *elem);
void rt_arr_set(Arr *a, int64_t i, int64_t raw, TypeDesc *elem);

// Field i of a record as its 8-byte unit, and the reverse.
//
// `td` is the descriptor of the record, not of the field. It gives the offset
// and the field type. For a value of an extending type, a caller can pass the
// base descriptor to access the base fields, because the two layouts have the
// same prefix.
int64_t rt_rec_get(void *rec, int64_t i, TypeDesc *td);
void rt_rec_set(void *rec, int64_t i, int64_t raw, TypeDesc *td);

// A box holds one value of type `inner` at its own width (§5.7). It is the
// cell behind a present optional and behind a captured variable.
//
// rt_box allocates before it stores, so the caller must root a heap pointer
// that it passes in. rt_box_get returns the unit.
void *rt_box(int64_t raw, TypeDesc *inner);
int64_t rt_box_get(const void *box, const TypeDesc *inner);

int64_t rt_eq(int64_t a, int64_t b, TypeDesc *td);

// rt_print writes one value and nothing more. print and printInline take any
// number of arguments, and codegen emits the separators: a space between two
// arguments, and for print a newline after the last one.
void rt_print(int64_t raw, TypeDesc *td);
void rt_print_space(void);
void rt_print_newline(void);

// string.from returns the text that rt_print writes. It is in the core so
// that a DateTime has its text when lib/time.c is not linked.
Str *rt_scalar_to_text(int64_t raw, TypeDesc *td);

// Writes the shortest text for v that reads back as v at its own width. When
// f32 is set, v came from a binary32, so 9 significant digits are sufficient.
// lib/json.c also uses it, so JSON numbers match printed numbers.
void rt_fmt_double(char *buf, size_t n, double v, int f32);

// ---- map operations (core; the Map struct is with the arrays above) ----

Map *rt_map_new(void);
int64_t rt_map_len(Map *m);
void *rt_map_get(Map *m, int64_t key, TypeDesc *map_td); // NULL, or a box of V
void rt_map_set(Map *m, int64_t key, int64_t val, TypeDesc *map_td);

// For lib/map.c: the entry position of a key, or -1 if the key is absent.
// rt_map_reindex rebuilds the index after a removal moves the positions.
int64_t rt_map_find(Map *m, int64_t key, TypeDesc *ktd);
void rt_map_reindex(Map *m, TypeDesc *ktd);

// The key and the value of entry i as 8-byte units. They take the descriptor
// of the component, not of the map, because the environment code in
// lib/process.c holds a Map with String keys and no map descriptor.
int64_t rt_map_key_at(Map *m, int64_t i, TypeDesc *ktd);
int64_t rt_map_val_at(Map *m, int64_t i, TypeDesc *vtd);

// ---- futures (async/await) ----
//
// An async function compiles to a state machine. Codegen moves the body into
// a step function whose locals are in a heap frame, and splits it at its
// await points. A call allocates the frame, stores the arguments, and queues
// a task. The body does not run yet.
//
// The scheduler steps ready tasks in arrival order. A step completes the
// body, or suspends it on a pending future and puts the task on the waiter
// list of that future. Completion moves the waiters back to the ready queue.
// There is one thread, so tasks interleave at await points and are never
// preempted.
//
// Codegen shares this protocol. Change both together:
//   - frame layout, 8-byte slots:
//       [0] resume state (0 = start)   [1] result, after the body returns
//       [2] the future being awaited   [3] Error raised, else NULL
//       [4..] parameters, locals, temps
//   - step(frame) returns 0 when the body completed (result in slot 1, and
//     an Error in slot 3 if it raised one), and 1 when it suspended on the
//     future in slot 2.
//   - Future layout: generated code reads `state` at offset 0 and `result`
//     at offset 8 directly (the await fast path and resume). The error of a
//     fallible body goes through rt_future_error, which also marks it
//     observed.
//
// This code is in the core, not in lib/: an async function and an await need
// no import (only async.run is in lib/async.c), and every generated main ends
// with rt_async_drain. The TD_FUTURE trace case in gc.c walks the Future and
// the frame, and must change with these structs.

// A completion callback on a future. `clos` selects one of two forms. If
// `clos` is set, it is a function value under the closure contract of
// codegen (async.run registers these). If `clos` is NULL, the node belongs to
// a race that waits for the first of its arguments to finish
// (rt_async_race). The node keeps the race alive after the program drops it.
typedef struct FutCB {
    void *clos;          // a Nio closure (TD_FUNC layout), or NULL for a race
    struct Future *race; // the race to complete, when clos is NULL
    int64_t index;       // this future's position in the race's arguments
    struct FutCB *next;
} FutCB;

typedef struct Future {
    int64_t state;           // FUT_PENDING / FUT_DONE; offset 0 (codegen)
    int64_t result;          // valid once done; offset 8 (codegen)
    void *error;             // Error raised by the body, else NULL. An
                             // await takes it through rt_future_error and
                             // raises it again (§2.9).
    TypeDesc *result_td;     // result type; NULL for a void async function
    int64_t (*step)(void *); // the compiled state machine
    void *frame;             // its frame; NULL once done
    TypeDesc *frame_td;      // record-shaped frame descriptor
    FutCB *cbs;              // completion callbacks, in registration order
    struct Future *waiters;  // tasks suspended on this future (linked by qnext)
    struct Future *qnext;    // ready-queue / waiter-list link
    int64_t deadline;        // timer future: absolute monotonic ms
    int64_t seq;             // timer creation order; breaks deadline ties
} Future;

enum { FUT_PENDING = 0, FUT_DONE = 1 };

// The ready queue of the scheduler, linked through qnext, and the pending
// timers. The timers are a binary heap ordered by deadline and then by seq,
// so two equal deadlines complete in creation order.
//
// Both are GC roots, and gc.c traces them at each collection. A suspended
// task stays reachable through the waiter list of the future it waits on.
//
// A timer future (from time.sleep) has no step and no frame. The scheduler
// completes it when its deadline passes. The heap array is malloc memory, not
// a GC block.
extern Future *rt_async_ready;
extern Future **rt_async_timer_heap;
extern int64_t rt_async_timer_count;

Future *rt_async_spawn(int64_t (*step)(void *), void *frame,
                       TypeDesc *frame_td, TypeDesc *result_td);
int64_t rt_await(Future *fu);
// rt_future_error takes the Error that a fallible async body raised and marks
// it observed, so rt_async_drain does not report it as an error that no await
// took (§2.9).
void *rt_future_error(Future *fu);
void rt_async_drain(void);

// A timer future that completes ms from now. It is in the core because the
// scheduler must know about each timer. lib/time.c wraps it as time.sleep.
Future *rt_async_timer_new(int64_t ms);

// Milliseconds on a monotonic clock. All deadlines in the runtime use it, so
// a change to the wall clock cannot make a sleep end early or a timeout end
// late.
int64_t rt_mono_ms(void);

// Calls one completion callback with the result of fu, which must be complete.
void rt_future_call_cb(Future *fu, void *clos);
// Appends one callback of either form. Callbacks run in registration order.
void rt_future_add_cb(Future *fu, FutCB *node);

// ---- futures completed from outside the scheduler ----
//
// A child process (lib/process.c) and a socket (lib/net.c) complete when the
// operating system reports them. Such a future has no step and no frame.
//
// The core owns the list of these futures, because the collector must trace
// them and the scheduler must know that work is outstanding. Without the
// list, the scheduler reports a deadlock for a program that waits on a child.
// The library that knows the system call does the waiting.
//
// Each future names its waiter, and that function pointer identifies the
// owner library. The core keeps a table of the different waiters, with a
// count of outstanding futures for each:
//
//   - one waiter with work: it gets the full budget and can block.
//   - more than one: none can block, because a socket cannot wake a waiter
//     that is blocked on a child. The scheduler polls each in turn, with a
//     short sleep between rounds.
//
// A waiter waits up to budget_ms for one of its futures (-1 blocks, 0 polls),
// completes the ready ones, and returns nonzero if it completed any. The
// scheduler calls it only when no task is ready, and limits the budget to the
// next timer deadline.
typedef int (*RtExternWait)(int64_t budget_ms);

// One unit of work outside the scheduler: the future, the waiter that owns
// it, and an optional value that must stay alive while the work is
// outstanding.
//
// `aux` keeps the object of the operation reachable. For example, the weak
// sweep below must not collect and close a socket with a read in progress
// when the program drops its last reference (lib/net.c). `aux_td` tells the
// collector how to trace it. Both are NULL when the waiter needs nothing
// kept.
typedef struct {
    Future *fu;
    RtExternWait wait;
    void *aux;
    TypeDesc *aux_td;
} ExternWork;

extern ExternWork *rt_async_extern;
extern int64_t rt_async_extern_count;

// Makes a future for work outside the scheduler and adds it to the list under
// `wait`.
Future *rt_async_extern_new(RtExternWait wait, void *aux, TypeDesc *aux_td);
// Completes one such future with a result (result_td describes it for the
// collector) or with an Error, and removes it from the list.
void rt_async_extern_done(Future *fu, int64_t result, TypeDesc *result_td, void *error);

// async.race(f1, f2, ...) (§6.8) returns a future of the index of the first
// argument that completes. It is in the core because its completion needs a
// callback in C, and the closure form of FutCB cannot hold one.
//
// The losing futures continue to run. The race cancels nothing, and §2.9
// still applies to a fallible future that nothing takes. An operation that
// must stop when it loses needs its own timeout (§6.15).
Future *rt_async_race(Arr *futures);

// ---- garbage collector (gc.c) ----
//
// A precise mark-sweep collector that does not move objects. It uses chunks of
// one size class each. It traces from roots through their TypeDesc, so a heap
// block needs no shape tag.
//
// Each block from rt_alloc has a GCObj immediately before its payload, so the
// header of a language pointer p is at (GCObj *)p - 1.
//
// A string literal is a static constant, not a heap block. Codegen emits it
// with the same header and GC_STATIC set, so tracing treats all pointer
// values the same. Only strings can be static, and a string has no children,
// so marking treats GC_STATIC as "already visited".
//
// GC_LARGE marks a block too large for a chunk. Such a block has its own list
// link. gc.c describes the chunk layout.
//
// This struct must stay one word: codegen emits the same header before each
// static string, and both sides must agree on the offset to the payload.

typedef struct GCObj {
    uint64_t info; // payload size << GC_SIZE_SHIFT | flags
} GCObj;

#define GC_MARK 1u
#define GC_STATIC 2u
#define GC_LARGE 4u
#define GC_SIZE_SHIFT 3

// A shadow-stack frame. Generated code pushes one for each function that
// holds pointer roots. slots points to the [nroots x i64] root storage of the
// function, which the code clears before it pushes the frame. tds points to a
// constant array that describes that storage.
//
// name is the source name of the function, for the call chain that rt_panic
// prints. It is a C string constant, and the collector does not read it. The
// field order must match `%GCFrame` in src/codegen.nio. Change both together.
typedef struct GCFrame {
    struct GCFrame *prev;
    int64_t nroots;
    TypeDesc **tds;
    int64_t *slots;
    const char *name;
} GCFrame;

// A root for a global variable: its address and its type.
typedef struct {
    int64_t *addr;
    TypeDesc *td;
} GCGlobal;

extern GCFrame *rt_gc_top; // generated code pushes and pops this directly

void rt_gc_globals(GCGlobal *table, int64_t n);
void rt_gc_collect(void);

// The scheduler calls these when it is about to wait with no task to run. A
// collection at that point delays no request, and it is the only collection
// that lowers the chunk reserve.
//
// The hint has a rate limit, and does nothing when there was no allocation
// since the last collection. rt_gc_idle_owed tells whether a collection is
// still due, and how long a blocking wait can be before that collection runs.
// See gc.c.
void rt_gc_idle_hint(void);
int64_t rt_gc_idle_owed(void);

// These two enclose runtime code that holds heap pointers the collector cannot
// see. A C local or a C parameter is not a root.
void rt_gc_disable(void);
void rt_gc_enable(void);

// ---- weak tables ----
//
// The collector has no finalizers. A socket needs one: it stands for a
// descriptor that the operating system owns, so a program that drops it would
// leak the descriptor until the process exits.
//
// lib/net.c keeps the blocks it gave out in a table that is not a root. Thus
// the table does not keep a block alive, and the hook registered here closes
// the descriptor of each collected block.
//
// A hook runs between the mark phase and the sweep phase. At that time
// GC_MARK still tells what survived, and nothing is reclaimed. The hook calls
// rt_gc_marked for each block it holds. It must not allocate.
#define GC_WEAK_HOOKS 4
void rt_gc_weak_hook(void (*fn)(void));
// Whether the mark phase of this cycle reached p. Only a weak hook can call
// it. For other callers, the collector has already cleared the bit.
int rt_gc_marked(void *p);

// Diagnostics for the collector tests. A chunk is in use (carved into cells),
// pooled for the next cycle, or released to the operating system.
int64_t rt_gc_live_bytes(void);
int64_t rt_gc_live_objects(void);
int64_t rt_gc_collections(void);
int64_t rt_gc_chunks_in_use(void);
int64_t rt_gc_chunks_pooled(void);
int64_t rt_gc_chunks_released(void);

// ---- JSON serialization (lib/json.c) ----

Str *rt_json_totext(int64_t raw, TypeDesc *td);

// Reads text into a value of the type td describes, and returns it as one
// 8-byte unit. It skips an object key that the type does not declare. A
// malformed document, a missing field that is not optional, and a value of
// the wrong shape are runtime errors, because this function has no err
// out-parameter.
int64_t rt_json_parse(Str *text, TypeDesc *td);

// ---- array library (lib/array.c) ----
//
// Every function except pop takes the TypeDesc of the array. It uses it to
// root its arguments across its allocations. indexOf and the natural-order
// sort also use it to compare two elements. The checker makes sure that push
// and pop get only growable arrays.
//
// push is in runtime.c, because lib/json.c grows arrays through it and one
// library cannot depend on another.

void rt_arr_push(Arr *a, int64_t v, TypeDesc *arr_td);
int64_t rt_arr_pop(Arr *a, TypeDesc *arr_td);
Arr *rt_arr_copy(Arr *a, TypeDesc *arr_td);
// The first index whose value equals v (by rt_eq), or -1 if no element is
// equal.
int64_t rt_arr_index_of(Arr *a, int64_t v, TypeDesc *arr_td);
// A new growable array of the elements from start up to end. A pair outside
// 0 <= start <= end <= len panics.
Arr *rt_arr_slice(Arr *a, int64_t start, int64_t end, TypeDesc *arr_td);
// Both sort in place, in ascending order, and keep the order of equal
// elements. rt_arr_sort gets the order from the element descriptor.
// rt_arr_sort_by gets it from `cmp`, a function value under the closure
// contract of codegen that returns true if its first argument comes before
// its second. The callback can allocate on each call, and this decides the
// algorithm (see lib/array.c).
void rt_arr_sort(Arr *a, TypeDesc *arr_td);
void rt_arr_sort_by(Arr *a, void *cmp, TypeDesc *arr_td);

// ---- string library (lib/string.c) ----
//
// Every function that allocates roots its own Str parameters, so generated
// code does not root the arguments it passes. Generated code still protects
// an earlier argument while it evaluates a later one.
//
// string.length and string.append have no function here: codegen reads the
// Str header directly and uses rt_str_concat.

Arr *rt_string_to_byte_array(Str *s);   // byte[]: one element per byte
Str *rt_string_from_byte_array(Arr *a); // its inverse: the low byte of each element
Str *rt_string_upper(Str *s);           // ASCII a-z only
Str *rt_string_lower(Str *s);           // ASCII A-Z only
Arr *rt_string_split(Str *s, Str *sep);
Str *rt_string_join(Arr *parts, Str *sep); // one allocation, sep between parts
Str *rt_string_trim(Str *s);
int64_t rt_string_contains(Str *s, Str *sub);
Str *rt_string_copy(Str *s);
Str *rt_string_replace(Str *s, Str *old, Str *new);
Str *rt_string_replace_all(Str *s, Str *old, Str *new);
int64_t rt_string_find(Str *s, Str *sub);
Str *rt_string_substring(Str *s, int64_t start, int64_t end); // bytes [start, end); out of range panics

// The three parsers are fallible, with the `void **err` convention of the fs
// module below. Text that is not a number, or a number that does not fit,
// stores an Error. The return value then has no meaning.

int64_t rt_string_to_int(Str *s, void **err);  // [+-]?digits, 64-bit range
int64_t rt_string_to_uint(Str *s, void **err); // +?digits, unsigned 64-bit range
double rt_string_to_float(Str *s, void **err); // sign, digits, fraction, exponent

// ---- map library (lib/map.c) ----
//
// A function that allocates roots its own parameters, as in the array and
// string libraries. has and remove do not allocate. remove compacts the entry
// arrays, so it is O(n). The signatures must match mapCallType in the checker
// and genMapCall in codegen.

int64_t rt_map_has(Map *m, int64_t key, TypeDesc *map_td);
int64_t rt_map_remove(Map *m, int64_t key, TypeDesc *map_td);
Arr *rt_map_keys(Map *m, TypeDesc *map_td);   // insertion order, a new array
Arr *rt_map_values(Map *m, TypeDesc *map_td); // same order as keys
Map *rt_map_copy(Map *m, TypeDesc *map_td);

// ---- path library (lib/path.c) ----
//
// join and localize operate on text only and do not read the file system. On
// all platforms both '/' and '\' are separators, and both functions write the
// separator of the host.
//
// join takes its first element separately, because its signature in the
// language is variadic with one fixed parameter: `path.join()` with no
// element is a compile error.

Str *rt_path_join(Str *first, Arr *rest); // separators localized, then cleaned
Str *rt_path_localize(Str *s);            // separators localized, nothing else
Str *rt_path_getcwd(void);                // panics if the OS cannot give it

// ---- file system library (lib/fs.c) ----
//
// Every function except rt_fs_exists takes a trailing `void **err`
// out-parameter and stores an Error through it. It does not call rt_panic.
// The slot is a GC root of the caller, cleared before the call. The branch on
// it in codegen (genFSCall) turns a non-NULL Error into a normal raise. The
// return value of a failed call is unspecified.
//
// The {T, ptr} pair that a fallible Nio function returns does not cross this
// boundary. clang lowers a C struct return of two pointers to an LLVM
// `[2 x i64]`, which is different from the `{ptr, ptr}` that codegen would
// declare. An out-parameter has the same lowering on all targets.
//
// A path is a byte string with no NUL terminator, so each function copies it
// into a C string first. A path that contains a NUL is an error and is not
// truncated.

Arr *rt_fs_read_dir(Str *path, void **err);  // String[]: entry names, "." and ".." omitted
Arr *rt_fs_read_file(Str *path, void **err); // byte[]: the file's bytes
void rt_fs_write_file(Str *path, Arr *content, void **err); // creates or truncates
void rt_fs_create_dir(Str *path, void **err); // one directory; the parent must exist
// Makes a directory named <system temp>/<prefix> plus six random characters,
// and returns its path. The name is chosen and the directory is created in
// one step.
// The runtime does not remove it. Use fs.delete with { recursive: true }.
Str *rt_fs_create_temp_dir(Str *prefix, void **err);
int64_t rt_fs_exists(Str *path);             // no error; unreachable is false
// Removes a file, an empty directory, or, when the options set `recursive`, a
// directory and all its contents. `opts` is an fs.DeleteOptions record with
// one field, as in types.DeleteOptionsT: [0] recursive, a `bool?`. It is NULL
// when the call gave no options.
void rt_fs_delete(Str *path, int64_t *opts, void **err);
// Returns an fs.Stat record with seven fields in declaration order, as in
// types.StatT: [0] name, [1] extension, [2] size, [3] isDirectory,
// [4] isFile, [5] permissions and [6] modified.
void *rt_fs_stat(Str *path, void **err);

// ---- process library (lib/process.c) ----
//
// The command line, the exit status, the three standard streams, and child
// processes.
//
// The writes cannot fail, as rt_print cannot. The stdin reads are fallible,
// with the `void **err` convention of fs. At the end of the input, read
// returns an empty byte[] and readLine returns NULL. These are not errors.
//
// The signatures must match processCallType and processNsCallType in the
// checker, and genProcessCall and genProcessNsCall in codegen.
//
// The core saves the command line in rt_args_init (runtime.c), which each
// generated main calls first. The command line exists only as the parameters
// of main, and a build can omit this file.

extern int rt_argc;
extern char **rt_argv;
void rt_args_init(int argc, char **argv); // core (runtime.c): main calls it first

void rt_process_exit(int64_t code); // never returns; stdio is flushed
Arr *rt_process_get_args(void);     // String[]: the arguments after the program name
void rt_process_stdout_write(Str *s);
void rt_process_stderr_write(Str *s);
// These write out the data that the C library buffers. A program that never
// exits needs them to make its output visible.
void rt_process_stdout_flush(void);
void rt_process_stderr_flush(void);
Arr *rt_process_stdin_read(void **err); // byte[]: everything to end of input
// Up to n bytes. The result is shorter only at the end of the input, so a
// short result tells the caller that the stream ended.
Arr *rt_process_stdin_read_bytes(int64_t n, void **err);
// NULL at the end of the input, else a String in a box (the representation of
// a String?). The string holds the bytes up to the next '\n'. The reader
// removes the '\n' and a '\r' before it.
void *rt_process_stdin_read_line(void **err);
// Starts a child and returns the future of its process.ChildRunResult: four
// fields in declaration order, as in types.ChildRunResultT: [0] the stdout
// byte[], [1] the stderr byte[], [2] the exit code (128 plus the signal
// number when a signal stopped the child), and [3] a process.ChildUsage?
// with the resources that the operating system charged to it.
//
// `opts` is a process.ChildRunOptions record with five optional fields, as in
// types.ChildRunOptionsT: [0] stdin `byte[]?`, [1] cwd `String?`,
// [2] env `Map<String, String>?`, [3] clearEnv `bool?` and [4] inherit
// `bool?`. It is NULL when the call gave no options. The defaults are empty
// input, the directory and environment of this program, and collected output.
//
// `inherit` gives the child the three standard streams of this program, and
// the output is not collected. The two byte[] fields of the result are then
// empty, and the runtime ignores a `stdin` given with it (§6.12).
//
// The call cannot fail. Each failure, including a failure to start the
// program, completes the future with an Error, so the caller gets it at the
// await. Several children can be outstanding at the same time.
Future *rt_process_run(Str *cmd, Arr *args, int64_t *opts);

// ---- regular expression library (lib/regexp.c) ----
//
// A RegExp is a compiled program: one GC block with a header, an instruction
// array, and the class bitmaps that the instructions refer to. It contains no
// pointers, so TD_REGEXP only keeps the block alive. The runtime builds the
// block in scratch memory and copies it in with one allocation, so the
// collector never traces a partly filled RegExp.
//
// The engine is a Thompson NFA simulation (the Pike VM). It follows all
// alternatives at the same time, one byte at a time, so matching time is
// linear and no pattern backtracks exponentially. A pattern often comes from
// outside the program, and a pattern that stops the program is a denial of
// service.
//
// Patterns and subjects are bytes, as in the rest of the string library: `.`
// matches one byte, and case folding is ASCII only.

// The header of the compiled block. The instructions follow it, then the class
// bitmaps, then, when `skip` requires it, a bitmap of the bytes that can start
// a match. rx_prog, rx_classes and rx_first_set in lib/regexp.c compute the
// three offsets. Only lib/regexp.c reads the block.
typedef struct {
    int64_t nprog;    // instructions
    int64_t nclass;   // character-class bitmaps, 32 bytes each
    int64_t flags;    // NIO_RX_* below, less the ones compiled away
    int64_t skip;     // RX_SKIP_* in lib/regexp.c: how an idle search moves
    int64_t skipbyte; // under RX_SKIP_BYTE, the only byte that starts a match
    int64_t prefpc;   // the start of the literal run after skipbyte
    int64_t npref;    // the number of instructions in that run
} Regexp;

// The flags that regexp.create takes. `i` is compiled into the program and is
// not stored here: a case-insensitive byte becomes a class of two bytes.
enum {
    NIO_RX_MULTILINE = 1, // `m`: ^ and $ also match at line boundaries
    NIO_RX_DOTALL = 2     // `s`: . also matches a newline
};

// Compiles a pattern. `flags` can be NULL when the call gave none. A pattern
// that does not compile stores an Error with NIO_ERR_INVALID through err (the
// out-parameter convention of the fs module) and returns NULL.
void *rt_regexp_create(Str *pattern, Str *flags, void **err);
// The byte offset of the first match in s, or -1 if there is none.
// string.find uses it for a pattern.
int64_t rt_regexp_find(Str *s, void *re);
// Whether the pattern matches at any position in s. string.contains uses it
// for a pattern.
int64_t rt_regexp_match(Str *s, void *re);

// ---- network library (lib/net.c) ----
//
// Sockets over TCP, UDP and Unix domain (§6.15). Every descriptor is
// non-blocking, and every operation that can wait returns a future. There is
// one thread, so a blocking read would stop the full program.
//
// A Socket and a Listener are one block (NetSock below) but two types for the
// checker, so the compiler refuses an operation on the wrong one.
//
// The block holds no pointers, so it traces like a RegExp. It holds the
// descriptor, so a close is visible through all copies at once: `fd` becomes
// -1, and an operation on a stale handle raises NOT_CONNECTED. Thus it cannot
// reach a descriptor number that the OS gave to a different resource later.
// The weak sweep (rt_gc_weak_hook) closes a descriptor that the program no
// longer refers to.
//
// `rwait` and `wwait` hold the records of lib/net.c for the operations that
// wait on this socket, in malloc memory. This is safe because TD_SOCKET is
// opaque. They also let a readiness event find its operation with a load and
// no search. This is important on Windows, where a SOCKET is a handle and not
// a small dense integer.
typedef struct {
    int64_t fd;     // -1 once closed
    int64_t proto;  // NIO_NET_TCP / NIO_NET_UDP / NIO_NET_UNIX
    int64_t role;   // NIO_NET_CONN / NIO_NET_LISTENER
    int64_t want;   // the operations that wait now: NIO_NET_R | NIO_NET_W
    int64_t rwait;  // the pending read-side operation (a Wait *), or 0
    int64_t wwait;  // the pending write-side operation, or 0
    int64_t polled; // what the kernel poller holds; NIO_NET_R stays until close
} NetSock;

enum {
    NIO_NET_TCP = 1,
    NIO_NET_UDP = 2,
    NIO_NET_UNIX = 3,

    NIO_NET_CONN = 0,
    NIO_NET_LISTENER = 1,

    NIO_NET_R = 1,
    NIO_NET_W = 2
};

// `opts` is a net.Options record with three optional fields, as in
// types.NetOptionsT: [0] timeout `Duration?`, [1] backlog `int?` and
// [2] noDelay `bool?`. It is NULL when the call gave none. Each function
// reads the fields that apply to it and ignores the others (§6.15).
//
// The two functions that return at once are fallible, with the out-parameter
// convention of lib/fs.c. Each function that waits returns a future and
// completes it with the Error, as process.child.run does, so the failure
// arrives at the await.

// Binds and listens. proto is TCP or UNIX. `addr` is "host:port" for TCP and a
// file system path for UNIX.
void *rt_net_listen(int64_t proto, Str *addr, int64_t *opts, void **err);
// Binds a datagram socket. It needs no listen and is ready at once.
void *rt_net_udp_bind(Str *addr, void **err);
// Connects. The future holds the connected Socket.
Future *rt_net_connect(int64_t proto, Str *addr, int64_t *opts);
// The next connection on a listener.
Future *rt_net_accept(void *l, int64_t *opts);
// Up to n bytes. A byte[] shorter than the request is not an error. An empty
// byte[] means that the peer closed its end.
Future *rt_net_read(void *s, int64_t n, int64_t *opts);
// Writes all bytes of data, with as many writes as necessary.
Future *rt_net_write(void *s, Arr *data, int64_t *opts);
// One datagram, as a net.Datagram record: two fields as in types.DatagramT,
// [0] address `String` and [1] data `byte[]`.
Future *rt_net_receive(void *s, int64_t n, int64_t *opts);
// Sends one datagram to `addr`. A datagram goes whole or not at all, so this
// call never sends part of one.
Future *rt_net_send(void *s, Str *addr, Arr *data, int64_t *opts);
// Closes the socket and fails all operations that wait on it with
// NOT_CONNECTED. It cannot fail, and a second call is safe. A Unix listener
// also removes its path, so a server can start again with no cleanup from the
// last run.
void rt_net_close(void *h);
// The address of this end, and the address of the peer, in the text form that
// the calls above accept. `address` on a listener bound to port 0 tells the
// program which port the operating system selected.
Str *rt_net_address(void *h, void **err);
Str *rt_net_peer(void *s, void **err);

// ---- date support ----
//
// The formatting is in the core, because a program prints and serializes the
// built-in `DateTime` type with no import of the time module. lib/time.c
// holds the other functions of that module.

void rt_date_tm(int64_t ms, struct tm *out);
Str *rt_date_to_text(int64_t ms);

// Reads "YYYY-MM-DD" or "YYYY-MM-DDTHH:MM:SSZ" from the first len bytes of buf
// into *out, as milliseconds from the epoch. It returns 0 and does not write
// *out when the text has neither form. It is in the core because lib/json.c
// parses dates for programs that do not import the time module.
int rt_date_parse_iso(const char *buf, int64_t len, int64_t *out);

// ---- statement coverage ----
//
// coverage.c is linked only into a build made with nio --coverage. The layout
// must match the coverage code in codegen, which emits both tables.
//
// counts holds one counter for each basic block, and generated code
// increments it. lines maps each instrumented source line to the block whose
// counter tells how many times that line ran. A line can occur more than
// once, and its counts are added. rt_cov_init registers an atexit handler that
// writes them as LCOV, so a run that ends in a panic or in process.exit still
// writes its report.

typedef struct {
    const char *file;
    int64_t line;
    int64_t block; // index into counts
} CovLine;

void rt_cov_init(const int64_t *counts, const CovLine *lines, int64_t nlines);

#endif // NIO_RUNTIME_H
