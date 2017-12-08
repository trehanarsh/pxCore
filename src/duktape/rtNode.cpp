/*

 rtCore Copyright 2005-2017 John Robinson

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

// rtNode.cpp

#if defined WIN32
#include <Windows.h>
#include <direct.h>
#define __PRETTY_FUNCTION__ __FUNCTION__
#else
#include <unistd.h>
#endif

#include <stdio.h>
#include <errno.h>

#include <string>
#include <fstream>

#include <iostream>
#include <sstream>

extern "C" {
#include "duv.h"
}

#include "pxTimer.h"

#ifdef _WIN32
# include <io.h>
# ifndef S_IRUSR
#  define S_IRUSR _S_IREAD
# endif
# ifndef S_IWUSR
#  define S_IWUSR _S_IWRITE
# endif
#endif

#if defined(__unix__) || defined(__POSIX__) || \
    defined(__APPLE__) || defined(_AIX)
#include <unistd.h> /* unlink, rmdir, etc. */
#else
# include <direct.h>
# include <io.h>
# define unlink _unlink
# define rmdir _rmdir
# define open _open
# define write _write
# define close _close
# ifndef stat
#  define stat _stati64
# endif
# ifndef lseek
#   define lseek _lseek
# endif
#endif

#ifndef WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

//#include "node.h"
//#include "node_javascript.h"
//#include "node_contextify_mods.h"

//#include "env.h"
//#include "env-inl.h"

#include "jsbindings/duktape/rtWrapperUtils.h"
#include "jsbindings/duktape/rtJsModules.h"

#ifndef WIN32
#pragma GCC diagnostic pop
#endif

#include "rtNode.h"

#ifndef RUNINMAIN
extern uv_loop_t *nodeLoop;
#endif

#ifdef RUNINMAIN

//#include "pxEventLoop.h"
//extern pxEventLoop* gLoop;

#define ENTERSCENELOCK()
#define EXITSCENELOCK()
#else
#define ENTERSCENELOCK() rtWrapperSceneUpdateEnter();
#define EXITSCENELOCK()  rtWrapperSceneUpdateExit();
#endif

#ifdef ENABLE_DEBUG_MODE
int g_argc = 0;
char** g_argv;
#endif
#ifndef ENABLE_DEBUG_MODE
extern args_t *s_gArgs;
#endif
namespace node
{
extern bool use_debug_agent;
#ifdef HAVE_INSPECTOR
extern bool use_inspector;
#endif
extern bool debug_wait_connect;
}

static int exec_argc;
static const char** exec_argv;
static rtAtomic sNextId = 100;


args_t *s_gArgs;

#ifdef RUNINMAIN
//extern rtNode script;
#endif
rtNodeContexts  mNodeContexts;

bool nodeTerminated = false;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef RUNINMAIN
#ifdef WIN32
static DWORD __rt_main_thread__;
#else
static pthread_t __rt_main_thread__;
#endif

// rtIsMainThread() - Previously:  identify the MAIN thread of 'node' which running JS code.
//
// rtIsMainThread() - Currently:   identify BACKGROUND thread which running JS code.
//
bool rtIsMainThread()
{
  // Since this is single threaded version we're always on the js thread
  return true;
}
#endif
#if 0
static inline bool file_exists(const char *file)
{
  struct stat buffer;
  return (stat (file, &buffer) == 0);
}
#endif

rtNodeContext::rtNodeContext() :
     js_file(NULL), mRefCount(0), mContextifyContext(NULL), uvLoop(NULL)
{
  mId = rtAtomicInc(&sNextId);

  createEnvironment();
}

#ifdef USE_CONTEXTIFY_CLONES
rtNodeContext::rtNodeContext(rtNodeContextRef clone_me) :
      js_file(NULL), mRefCount(0), mContextifyContext(NULL), uvLoop(NULL)
{
  mId = rtAtomicInc(&sNextId);

  clonedEnvironment(clone_me);
}
#endif

void rtNodeContext::createEnvironment()
{
  rtLogInfo(__FUNCTION__);
}

#ifdef USE_CONTEXTIFY_CLONES

void rtNodeContext::clonedEnvironment(rtNodeContextRef clone_me)
{
  rtLogInfo(__FUNCTION__);
  duk_idx_t thr_idx = duk_push_thread(clone_me->dukCtx);
  
  duk_dup(clone_me->dukCtx, -1);
  rtDukPutIdentToGlobal(clone_me->dukCtx);

  dukCtx = duk_get_context(clone_me->dukCtx, thr_idx);

  uv_loop_t *dukLoop = new uv_loop_t();
  uv_loop_init(dukLoop);
  dukLoop->data = dukCtx;
  uvLoop = dukLoop;

  {
    duk_push_thread_stash(dukCtx, dukCtx);
    duk_push_pointer(dukCtx, (void*)dukLoop);
    duk_bool_t rc = duk_put_prop_string(dukCtx, -2, "__duk_loop");
    assert(rc);
    duk_pop(dukCtx);
  }

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
}

#endif // USE_CONTEXTIFY_CLONES

rtNodeContext::~rtNodeContext()
{
  rtLogInfo(__FUNCTION__);
  //Make sure node is not destroyed abnormally
  Release();
  // NOTE: 'mIsolate' is owned by rtNode.  Don't destroy here !
}

rtError rtNodeContext::add(const char *name, rtValue const& val)
{
  rt2duk(dukCtx, val);
  rtDukPutIdentToGlobal(dukCtx, name);
  
  return RT_OK;
}

rtValue rtNodeContext::get(std::string name)
{
  return get( name.c_str() );
}

rtValue rtNodeContext::get(const char *name)
{
  // unimplemented
  assert(0);

  return rtValue();
}

bool rtNodeContext::has(std::string name)
{
  return has(name.c_str());
}

bool rtNodeContext::has(const char *name)
{
  // unimplemented
  assert(0);

  return false;
}

rtError rtNodeContext::runScript(const char* script, rtValue* retVal /*= NULL*/, const char *args /*= NULL*/)
{
  if(script == NULL)
  {
    rtLogError(" %s  ... no script given.",__PRETTY_FUNCTION__);

    return RT_FAIL;
  }

  // rtLogDebug(" %s  ... Running...",__PRETTY_FUNCTION__);

  return runScript(std::string(script), retVal, args);
}

// Sync readfile using libuv APIs as an API function.
static duk_ret_t duv_loadfile(duk_context *ctx) {
  const char* path = duk_require_string(ctx, 0);
  uv_fs_t req;
  int fd = 0;
  uint64_t size;
  char* chunk;
  uv_buf_t buf;

  duk_push_thread_stash(ctx, ctx);
  duk_bool_t rc = duk_get_prop_string(ctx, -1, "__duk_loop");
  assert(rc);
  uv_loop_t *dukLoop = (uv_loop_t *)duk_get_pointer(ctx, -1);
  duk_pop(ctx);
  duk_pop(ctx);

  if (uv_fs_open(dukLoop, &req, path, O_RDONLY, 0644, NULL) < 0) goto fail;
  uv_fs_req_cleanup(&req);
  fd = req.result;
  if (uv_fs_fstat(dukLoop, &req, fd, NULL) < 0) goto fail;
  uv_fs_req_cleanup(&req);
  size = req.statbuf.st_size;
  chunk = (char*)duk_alloc(ctx, size);
  buf = uv_buf_init(chunk, size);
  if (uv_fs_read(dukLoop, &req, fd, &buf, 1, 0, NULL) < 0) {
    duk_free(ctx, chunk);
    goto fail;
  }
  uv_fs_req_cleanup(&req);
  duk_push_lstring(ctx, chunk, size);
  duk_free(ctx, chunk);
  uv_fs_close(dukLoop, &req, fd, NULL);
  uv_fs_req_cleanup(&req);

  return 1;

fail:
  uv_fs_req_cleanup(&req);
  if (fd) uv_fs_close(dukLoop, &req, fd, NULL);
  uv_fs_req_cleanup(&req);
  duk_error(ctx, DUK_ERR_ERROR, "%s: %s: %s", uv_err_name(req.result), uv_strerror(req.result), path);
}

struct duv_list {
  const char* part;
  int offset;
  int length;
  struct duv_list* next;
};
typedef struct duv_list duv_list_t;

static duv_list_t* duv_list_node(const char* part, int start, int end, duv_list_t* next) {
  duv_list_t *node = (duv_list_t *)malloc(sizeof(*node));
  node->part = part;
  node->offset = start;
  node->length = end - start;
  node->next = next;
  return node;
}

static duk_ret_t duv_path_join(duk_context *ctx) {
  duv_list_t *list = NULL;
  int absolute = 0;

  // Walk through all the args and split into a linked list
  // of segments
  {
    // Scan backwards looking for the the last absolute positioned path.
    int top = duk_get_top(ctx);
    int i = top - 1;
    while (i > 0) {
      const char* part = duk_require_string(ctx, i);
      if (part[0] == '/') break;
      i--;
    }
    for (; i < top; ++i) {
      const char* part = duk_require_string(ctx, i);
      int j;
      int start = 0;
      int length = strlen(part);
      if (part[0] == '/') {
        absolute = 1;
      }
      while (start < length && part[start] == 0x2f) { ++start; }
      for (j = start; j < length; ++j) {
        if (part[j] == 0x2f) {
          if (start < j) {
            list = duv_list_node(part, start, j, list);
            start = j;
            while (start < length && part[start] == 0x2f) { ++start; }
          }
        }
      }
      if (start < j) {
        list = duv_list_node(part, start, j, list);
      }
    }
  }

  // Run through the list in reverse evaluating "." and ".." segments.
  {
    int skip = 0;
    duv_list_t *prev = NULL;
    while (list) {
      duv_list_t *node = list;

      // Ignore segments with "."
      if (node->length == 1 &&
        node->part[node->offset] == 0x2e) {
        goto skip;
      }

      // Ignore segments with ".." and grow the skip count
      if (node->length == 2 &&
        node->part[node->offset] == 0x2e &&
        node->part[node->offset + 1] == 0x2e) {
        ++skip;
        goto skip;
      }

      // Consume the skip count
      if (skip > 0) {
        --skip;
        goto skip;
      }

      list = node->next;
      node->next = prev;
      prev = node;
      continue;

    skip:
      list = node->next;
      free(node);
    }
    list = prev;
  }

  // Merge the list into a single `/` delimited string.
  // Free the remaining list nodes.
  {
    int count = 0;
    if (absolute) {
      duk_push_string(ctx, "/");
      ++count;
    }
    while (list) {
      duv_list_t *node = list;
      duk_push_lstring(ctx, node->part + node->offset, node->length);
      ++count;
      if (node->next) {
        duk_push_string(ctx, "/");
        ++count;
      }
      list = node->next;
      free(node);
    }
    duk_concat(ctx, count);
  }
  return 1;
}

static duk_ret_t duv_require(duk_context *ctx) {
  int is_main = 0;

  const duv_schema_entry schema[] = {
    { "id", duk_is_string },
    { NULL }
  };

  dschema_check(ctx, schema);

  // push Duktape
  duk_get_global_string(ctx, "Duktape");

  // id = Duktape.modResolve(this, id);
  duk_get_prop_string(ctx, -1, "modResolve");
  duk_push_this(ctx);
  {
    // Check if we're in main
    duk_get_prop_string(ctx, -1, "exports");
    if (duk_is_undefined(ctx, -1)) { is_main = 1; }
    duk_pop(ctx);
  }
  duk_dup(ctx, 0);
  duk_call_method(ctx, 1);
  duk_replace(ctx, 0);

  // push Duktape.modLoaded
  duk_get_prop_string(ctx, -1, "modLoaded");

  // push Duktape.modLoaded[id];
  duk_dup(ctx, 0);
  duk_get_prop(ctx, -2);

  // if (typeof Duktape.modLoaded[id] === 'object') {
  //   return Duktape.modLoaded[id].exports;
  // }
  if (duk_is_object(ctx, -1)) {
    duk_get_prop_string(ctx, -1, "exports");
    return 1;
  }

  // pop Duktape.modLoaded[id]
  duk_pop(ctx);

  // push module = { id: id, exports: {} }
  duk_push_object(ctx);
  duk_dup(ctx, 0);
  duk_put_prop_string(ctx, -2, "id");
  duk_push_object(ctx);
  duk_put_prop_string(ctx, -2, "exports");

  // Set module.main = true if we're the first script
  if (is_main) {
    duk_push_boolean(ctx, 1);
    duk_put_prop_string(ctx, -2, "main");
  }

  // Or set module.parent = parent if we're a child.
  else {
    duk_push_this(ctx);
    duk_put_prop_string(ctx, -2, "parent");
  }

  // Set the prototype for the module to access require.
  duk_push_global_stash(ctx);
  duk_get_prop_string(ctx, -1, "modulePrototype");
  duk_set_prototype(ctx, -3);
  duk_pop(ctx);

  // Duktape.modLoaded[id] = module
  duk_dup(ctx, 0);
  duk_dup(ctx, -2);
  duk_put_prop(ctx, -4);

  // remove Duktape.modLoaded
  duk_remove(ctx, -2);

  // push Duktape.modLoad(module)
  duk_get_prop_string(ctx, -2, "modLoad");
  duk_dup(ctx, -2);
  duk_call_method(ctx, 0);

  // if ret !== undefined module.exports = ret;
  if (duk_is_undefined(ctx, -1)) {
    duk_pop(ctx);
  }
  else {
    duk_put_prop_string(ctx, -2, "exports");
  }

  duk_get_prop_string(ctx, -1, "exports");

  return 1;
}

// Default implementation for modResolve
// Duktape.modResolve = function (parent, id) {
//   return pathJoin(parent.id, "..", id);
// };
static duk_ret_t duv_mod_resolve(duk_context *ctx) {
  const duv_schema_entry schema[] = {
    { "id", duk_is_string },
    { NULL }
  };

  dschema_check(ctx, schema);
  duk_dup(ctx, 0);

  return 1;
}

// Default Duktape.modLoad implementation
// return Duktape.modCompile.call(module, loadFile(module.id));
//     or load shared libraries using Duktape.loadlib.
static duk_ret_t duv_mod_load(duk_context *ctx) {
  const char* id;
  const char* ext;

  const duv_schema_entry schema[] = {
    { NULL }
  };

  dschema_check(ctx, schema);

  duk_get_global_string(ctx, "Duktape");
  duk_push_this(ctx);
  duk_get_prop_string(ctx, -1, "id");
  id = duk_get_string(ctx, -1);
  if (!id) {
    duk_error(ctx, DUK_ERR_ERROR, "Missing id in module");
    return 0;
  }

  // calculate the extension to know which compiler to use.
  ext = id + strlen(id);
  while (ext > id && ext[0] != '/' && ext[0] != '.') { --ext; }

  if (strcmp(ext, ".js") != 0) {
    duk_pop(ctx);
    std::string s = std::string(id) + ".js";
    duk_push_string(ctx, s.c_str());
  }

  // Stack: [Duktape, this, id]
  duk_push_c_function(ctx, duv_loadfile, 1);
  // Stack: [Duktape, this, id, loadfile]
  duk_insert(ctx, -2);
  // Stack: [Duktape, this, loadfile, id]
  duk_call(ctx, 1);
  // Stack: [Duktape, this, data]
  duk_get_prop_string(ctx, -3, "modCompile");
  // Stack: [Duktape, this, data, modCompile]
  duk_insert(ctx, -3);
  // Stack: [Duktape, modCompile, this, data]
  duk_call_method(ctx, 1);
  // Stack: [Duktape, exports]
  return 1;
}

// Load a duktape C function from a shared library by path and name.
static duk_ret_t duv_loadlib(duk_context *ctx) {
  const char *name, *path;
  uv_lib_t lib;
  duk_c_function fn;

  // Check the args
  const duv_schema_entry schema[] = {
    { "path", duk_is_string },
    { "name", duk_is_string },
    { NULL }
  };

  dschema_check(ctx, schema);

  path = duk_get_string(ctx, 0);
  name = duk_get_string(ctx, 1);

  if (uv_dlopen(path, &lib)) {
    duk_error(ctx, DUK_ERR_ERROR, "Cannot load shared library %s", path);
    return 0;
  }
  if (uv_dlsym(&lib, name, (void**)&fn)) {
    duk_error(ctx, DUK_ERR_ERROR, "Unable to find %s in %s", name, path);
    return 0;
  }
  duk_push_c_function(ctx, fn, 0);
  return 1;
}

// Given a module and js code, compile the code and execute as CJS module
// return the result of the compiled code ran as a function.
static duk_ret_t duv_mod_compile(duk_context *ctx) {
  // Check the args
  const duv_schema_entry schema[] = {
    { "code", dschema_is_data },
    { NULL }
  };

  dschema_check(ctx, schema);

  duk_to_string(ctx, 0);

  duk_push_this(ctx);
  duk_get_prop_string(ctx, -1, "id");

  // Wrap the code
  duk_push_string(ctx, "function(){var module=this,exports=this.exports,require=this.require.bind(this);");
  duk_dup(ctx, 0);
  duk_push_string(ctx, "}");
  duk_concat(ctx, 3);
  duk_insert(ctx, -2);

  // Compile to a function
  duk_compile(ctx, DUK_COMPILE_FUNCTION);

  duk_push_this(ctx);
  duk_call_method(ctx, 0);

  return 1;
}

static duk_ret_t duv_main(duk_context *ctx) {

  duk_require_string(ctx, 0);

  {
    duk_push_global_object(ctx);
    duk_dup(ctx, -1);
    duk_put_prop_string(ctx, -2, "global");

    duk_push_boolean(ctx, 1);
    duk_put_prop_string(ctx, -2, "dukluv");

    // [global]

    // Load duv module into global uv
    duk_push_c_function(ctx, dukopen_uv, 0);
    duk_call(ctx, 0);

    // [global obj]
    duk_put_prop_string(ctx, -2, "uv");

    // Replace the module loader with Duktape 2.x polyfill.
    duk_get_prop_string(ctx, -1, "Duktape");
    duk_del_prop_string(ctx, -1, "modSearch");
    duk_push_c_function(ctx, duv_mod_compile, 1);
    duk_put_prop_string(ctx, -2, "modCompile");
    duk_push_c_function(ctx, duv_mod_resolve, 1);
    duk_put_prop_string(ctx, -2, "modResolve");
    duk_push_c_function(ctx, duv_mod_load, 0);
    duk_put_prop_string(ctx, -2, "modLoad");
    duk_push_c_function(ctx, duv_loadlib, 2);
    duk_put_prop_string(ctx, -2, "loadlib");
    duk_pop(ctx);


    // Put in some quick globals to test things.
    duk_push_c_function(ctx, duv_path_join, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "pathJoin");

    duk_push_c_function(ctx, duv_loadfile, 1);
    duk_put_prop_string(ctx, -2, "loadFile");

    // require.call({id:uv.cwd()+"/main.c"}, path);
    duk_push_c_function(ctx, duv_require, 1);

    {
      // Store this require function in the module prototype
      duk_push_global_stash(ctx);
      duk_push_object(ctx);
      duk_dup(ctx, -3);
      duk_put_prop_string(ctx, -2, "require");
      duk_put_prop_string(ctx, -2, "modulePrototype");
      duk_pop(ctx);
    }
  }

  {
    duk_push_object(ctx);
    duk_push_c_function(ctx, duv_cwd, 0);
    duk_call(ctx, 0);
    duk_push_string(ctx, "/main.c");
    duk_concat(ctx, 2);
    duk_put_prop_string(ctx, -2, "id");
  }

  duk_dup(ctx, 0);

  //[arg.js global duv_require obj arg.js]

  duk_int_t ret = duk_pcall_method(ctx, 1);
  if (ret) {
    duv_dump_error(ctx, -1);
    return 0;
  }

  return 0;
}

static duk_int_t myload_code(duk_context *ctx, const char *code)
{
  static int reqid = 0;
  char buf[1024];
  uv_fs_t open_req1;
  sprintf(buf, "file%d.js", reqid++);

  unlink(buf);

  duk_push_thread_stash(ctx, ctx);
  duk_bool_t rc = duk_get_prop_string(ctx, -1, "__duk_loop");
  assert(rc);
  uv_loop_t *dukLoop = (uv_loop_t *)duk_get_pointer(ctx, -1);
  duk_pop(ctx);
  duk_pop(ctx);

  int r;
  r = uv_fs_open(dukLoop, &open_req1, buf, O_WRONLY | O_CREAT,
    S_IWUSR | S_IRUSR, NULL);
  assert(r >= 0);
  assert(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  uv_buf_t iov = uv_buf_init((char *)code, strlen(code));
  uv_fs_t write_req;
  r = uv_fs_write(NULL, &write_req, open_req1.result, &iov, 1, -1, NULL);
  assert(r >= 0);
  assert(write_req.result >= 0);
  uv_fs_req_cleanup(&write_req);

  uv_fs_t close_req;
  uv_fs_close(dukLoop, &close_req, open_req1.result, NULL);

  duk_push_c_function(ctx, duv_main, 1);
  duk_push_string(ctx, buf);
  if (duk_pcall(ctx, 1)) {
    duv_dump_error(ctx, -1);
    //uv_loop_close(dukLoop);
    duk_destroy_heap(ctx);
    return 1;
  }

  return 0;
}

rtError rtNodeContext::runScript(const std::string &script, rtValue* retVal /*= NULL*/, const char* /* args = NULL*/)
{
  rtLogInfo(__FUNCTION__);
  if(script.empty())
  {
    rtLogError(" %s  ... no script given.",__PRETTY_FUNCTION__);

    return RT_FAIL;
  }

  if (myload_code(dukCtx, script.c_str())) {
    return RT_FAIL;
  }

  return RT_OK;
}

std::string readFile(const char *file)
{
  std::ifstream       src_file(file);
  std::stringstream   src_script;

  src_script << src_file.rdbuf();

  std::string s = src_script.str();

  return s;
}

rtError rtNodeContext::runFile(const char *file, rtValue* retVal /*= NULL*/, const char* args /*= NULL*/)
{
  if(file == NULL)
  {
    rtLogError(" %s  ... no script given.",__PRETTY_FUNCTION__);

    return RT_FAIL;
  }

  // Read the script file
  js_file   = file;
  js_script = readFile(file);
  
  if( js_script.empty() ) // load error
  {
    rtLogError(" %s  ... load error / not found.",__PRETTY_FUNCTION__);
     
    return RT_FAIL;
  }

  return runScript(js_script, retVal, args);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

rtNode::rtNode()
#ifndef RUNINMAIN
#ifdef USE_CONTEXTIFY_CLONES
: mRefContext(), mNeedsToEnd(false), node_is_initialized(false)
#else
: mNeedsToEnd(false)
#endif
#endif
{
  rtLogInfo(__FUNCTION__);
  mTestGc = false;
  initializeNode();
}

rtNode::rtNode(bool initialize)
#ifndef RUNINMAIN
#ifdef USE_CONTEXTIFY_CLONES
: mRefContext(), mNeedsToEnd(false), node_is_initialized(false)
#else
: mNeedsToEnd(false)
#endif
#endif
{
  rtLogInfo(__FUNCTION__);
  mTestGc = false;
  if (true == initialize)
  {
    initializeNode();
  }
}

void rtNode::initializeNode()
{
  rtLogInfo(__FUNCTION__);
  char const* s = getenv("RT_TEST_GC");
  if (s && strlen(s) > 0)
    mTestGc = true;
  else
    mTestGc = false;

  if (mTestGc)
    rtLogWarn("*** PERFORMANCE WARNING *** : gc being invoked in render thread");

// TODO Please make this better... less hard coded...

                              //0123456 789ABCDEF012 345 67890ABCDEF
#if ENABLE_V8_HEAP_PARAMS
#ifdef ENABLE_NODE_V_6_9
  static const char *args2   = "rtNode\0-e\0console.log(\"rtNode Initalized\");\0\0";
  static const char *argv2[] = {&args2[0], &args2[7], &args2[10], NULL};
#else
  rtLogWarn("v8 old heap space configured to 64mb\n");
  static const char *args2   = "rtNode\0--expose-gc\0--max_old_space_size=64\0-e\0console.log(\"rtNode Initalized\");\0\0";
  static const char *argv2[] = {&args2[0], &args2[7], &args2[19], &args2[43], &args2[46], NULL};
#endif // ENABLE_NODE_V_6_9
#else
#ifdef ENABLE_NODE_V_6_9
#ifndef ENABLE_DEBUG_MODE
   static const char *args2   = "rtNode\0-e\0console.log(\"rtNode Initalized\");\0\0";
   static const char *argv2[] = {&args2[0], &args2[7], &args2[10], NULL};
#endif //!ENABLE_DEBUG_MODE
#else
  static const char *args2   = "rtNode\0--expose-gc\0-e\0console.log(\"rtNode Initalized\");\0\0";
  static const char *argv2[] = {&args2[0], &args2[7], &args2[19], &args2[22], NULL};
#endif // ENABLE_NODE_V_6_9
#endif //ENABLE_V8_HEAP_PARAMS
#ifndef ENABLE_DEBUG_MODE
  int          argc   = sizeof(argv2)/sizeof(char*) - 1;

  static args_t aa(argc, (char**)argv2);

  s_gArgs = &aa;


  char **argv = aa.argv;
#endif

#ifdef RUNINMAIN
#ifdef WIN32
  __rt_main_thread__ = GetCurrentThreadId();
#else
  __rt_main_thread__ = pthread_self(); //  NB
#endif
#endif
  nodePath();


#ifdef ENABLE_NODE_V_6_9
  rtLogWarn("rtNode::rtNode() calling init \n");
#ifdef ENABLE_DEBUG_MODE
  init();
#else
  init(argc, argv);
#endif
#else
#ifdef ENABLE_DEBUG_MODE
  init();
#else
  init(argc, argv);
#endif
#endif // ENABLE_NODE_V_6_9
}

rtNode::~rtNode()
{
  // rtLogInfo(__FUNCTION__);
  term();
}

void rtNode::pump()
{
#ifndef RUNINMAIN
  return;
#else
  for (int i = 0; i < uvLoops.size(); ++i) {
    uv_run(uvLoops[i], UV_RUN_NOWAIT);
  }
#endif // RUNINMAIN
}

void rtNode::garbageCollect()
{
}

#if 0
rtNode::forceGC()
{
  mIsolate->RequestGarbageCollectionForTesting(Isolate::kFullGarbageCollection);
}
#endif

void rtNode::nodePath()
{
  const char* NODE_PATH = ::getenv("NODE_PATH");

  if(NODE_PATH == NULL)
  {
    char cwd[1024] = {};

    if (getcwd(cwd, sizeof(cwd)) != NULL)
    {
#ifdef WIN32
	  _putenv_s("NODE_PATH", cwd);
#else
	  ::setenv("NODE_PATH", cwd, 1); // last arg is 'overwrite' ... 0 means DON'T !
#endif
	  rtLogInfo("NODE_PATH=%s", cwd);
    }
    else
    {
      rtLogError(" - failed to set NODE_PATH");
    }
  }
}

duk_ret_t my_print(duk_context *ctx) 
{
    printf("%s\n", duk_get_string(ctx, -1));
    return 0;
}

#ifndef RUNINMAIN
bool rtNode::isInitialized()
{
  //rtLogDebug("rtNode::isInitialized returning %d\n",node_is_initialized);
  return node_is_initialized;
}
#endif
#ifdef ENABLE_DEBUG_MODE
void rtNode::init()
#else
void rtNode::init(int argc, char** argv)
#endif
{
  // Hack around with the argv pointer. Used for process.title = "blah".
#ifdef ENABLE_DEBUG_MODE
  g_argv = uv_setup_args(g_argc, g_argv);
#else
  argv = uv_setup_args(argc, argv);
#endif

  rtLogInfo(__FUNCTION__);

#if 0
#warning Using DEBUG AGENT...
  use_debug_agent = true; // JUNK
#endif

  if(node_is_initialized == false)
  {
    node_is_initialized = true;

    uv_loop_t *dukLoop = new uv_loop_t();
    uv_loop_init(dukLoop);
    //uv_loop_t dukLoop = uv_default_loop();
    dukCtx = duk_create_heap(NULL, NULL, NULL, dukLoop, NULL);
    if (!dukCtx) {
	    rtLogWarn("Problem initiailizing duktape heap\n");
	    return;
    }

    duk_module_duktape_init(dukCtx);

    duk_push_c_function(dukCtx, &my_print, 1);
    duk_put_global_string(dukCtx, "print");

    dukLoop->data = dukCtx;
    uvLoops.push_back(dukLoop);

    {
      duk_push_thread_stash(dukCtx, dukCtx);
      duk_push_pointer(dukCtx, (void*)dukLoop);
      duk_bool_t rc = duk_put_prop_string(dukCtx, -2, "__duk_loop");
      assert(rc);
      duk_pop(dukCtx);
    }

    rtSetupJsModuleBindings(dukCtx);
  }
}

void rtNode::term()
{
  rtLogInfo(__FUNCTION__);
  nodeTerminated = true;

  //uv_loop_close(dukLoop);
  duk_destroy_heap(dukCtx);

#ifdef USE_CONTEXTIFY_CLONES
  if( mRefContext.getPtr() )
  {
    mRefContext->Release();
  }
#endif
}

std::string rtNode::name() const
{
  return "duktape";
}


inline bool fileExists(const std::string& name)
{
  struct stat buffer;
  return (stat (name.c_str(), &buffer) == 0);
}

rtNodeContextRef rtNode::getGlobalContext() const
{
  return rtNodeContextRef();
}

rtNodeContextRef rtNode::createContext(bool ownThread)
{
  UNUSED_PARAM(ownThread);    // not implemented yet.

  rtNodeContextRef ctxref;

#ifdef USE_CONTEXTIFY_CLONES
  if(mRefContext.getPtr() == NULL)
  {
    mRefContext = new rtNodeContext();
    ctxref = mRefContext;

    mRefContext->dukCtx = dukCtx;
    assert(uvLoops.size() == 1);
    mRefContext->uvLoop = uvLoops[0];

    static std::string sandbox_path;

    if(sandbox_path.empty()) // only once.
    {
      char *nodePath = ::getenv("NODE_PATH");
      if (NULL != nodePath)
      {
        const std::string NODE_PATH = nodePath;
        sandbox_path = NODE_PATH + "/" + SANDBOX_JS;
      }
    }

    // Populate 'sandbox' vars in JS...
    if(fileExists(sandbox_path.c_str()))
    {
      mRefContext->runFile(sandbox_path.c_str());
    }
    else
    {
      rtLogError("## ERROR:   Could not find \"%s\" ...", sandbox_path.c_str());
    }
    // !CLF: TODO Why is ctxref being reassigned from the mRefContext already assigned?
    //ctxref = new rtNodeContext(mIsolate, mRefContext);
  }
  else
  {
    // rtLogInfo("\n createContext()  >>  CLONE CREATED !!!!!!");
    ctxref = new rtNodeContext(mRefContext); // CLONE !!!
    assert(ctxref->uvLoop != NULL);
    uvLoops.push_back(mRefContext->uvLoop);
  }
#else
    ctxref = new rtNodeContext(mIsolate,mPlatform);

#endif

  // TODO: Handle refs in map ... don't leak !
  // mNodeContexts[ ctxref->getContextId() ] = ctxref;  // ADD to map

  return ctxref;
}

unsigned long rtNodeContext::Release()
{
    long l = rtAtomicDec(&mRefCount);
    if (l == 0)
    {
     delete this;
    }
    return l;
}
