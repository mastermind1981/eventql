/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include "stx/inspect.h"
#include "stx/assets.h"
#include "zbase/JavaScriptContext.h"
#include "js/Conversions.h"
#include "jsapi.h"

using namespace stx;

namespace zbase {

JSClass JavaScriptContext::kGlobalJSClass = { "global", JSCLASS_GLOBAL_FLAGS };

static bool write_json_to_buf(const char16_t* str, uint32_t strlen, void* out) {
  *static_cast<String*>(out) += StringUtil::convertUTF16To8(
      std::basic_string<char16_t>(str, strlen));

  return true;
}

void JavaScriptContext::dispatchError(
    JSContext* ctx,
    const char* message,
    JSErrorReport* report) {
  auto rt = JS_GetRuntime(ctx);
  auto rt_userdata = JS_GetRuntimePrivate(rt);
  if (rt_userdata) {
    auto req = static_cast<JavaScriptContext*>(rt_userdata);
    req->storeError(StringUtil::format(
        "<script:$0:$1> $2",
        report->lineno,
        report->column,
        String(message)));
  }
}

JavaScriptContext::JavaScriptContext(
    size_t memlimit /* = kDefaultMemLimit */) {
  {
    runtime_ = JS_NewRuntime(memlimit);
    if (!runtime_) {
      RAISE(kRuntimeError, "error while initializing JavaScript runtime");
    }

    JS::RuntimeOptionsRef(runtime_)
        .setBaseline(true)
        .setIon(true)
        .setAsmJS(true);

    JS_SetRuntimePrivate(runtime_, this);
    JS_SetErrorReporter(runtime_, &JavaScriptContext::dispatchError);

    ctx_ = JS_NewContext(runtime_, 8192);
    if (!ctx_) {
      RAISE(kRuntimeError, "error while initializing JavaScript context");
    }

    JSAutoRequest js_req(ctx_);

    global_ = JS_NewGlobalObject(
        ctx_,
        &kGlobalJSClass,
        nullptr,
        JS::FireOnNewGlobalHook);

    if (!global_) {
      RAISE(kRuntimeError, "error while initializing JavaScript context");
    }

    {
      JSAutoCompartment ac(ctx_, global_);
      JS_InitStandardClasses(ctx_, global_);
    }
  }

  loadProgram(Assets::getAsset("zbase/mapreduce/prelude.js"));
}

JavaScriptContext::~JavaScriptContext() {
  JS_DestroyContext(ctx_);
  JS_DestroyRuntime(runtime_);
}

void JavaScriptContext::storeError(const String& error) {
  current_error_ = error;
}

void JavaScriptContext::raiseError() {
  RAISEF("JavaScriptError", "$0", current_error_);
}

void JavaScriptContext::loadProgram(const String& program) {
  JSAutoRequest js_req(ctx_);
  JSAutoCompartment ac(ctx_, global_);

  JS::RootedValue rval(ctx_);

  JS::CompileOptions opts(ctx_);
  opts.setFileAndLine("<mapreduce>", 1);

  if (!JS::Evaluate(
        ctx_,
        global_,
        opts,
        program.c_str(),
        program.size(),
        &rval)) {
    raiseError();
  }
}

void JavaScriptContext::callMapFunction(
    const String& method_name,
    const String& json_string,
    Vector<Pair<String, String>>* tuples) {
  auto json_wstring = StringUtil::convertUTF8To16(json_string);

  JSAutoRequest js_req(ctx_);
  JSAutoCompartment js_comp(ctx_, global_);

  JS::RootedValue json(ctx_);
  if (!JS_ParseJSON(ctx_, json_wstring.c_str(), json_wstring.size(), &json)) {
    raiseError();
  }

  JS::AutoValueArray<1> argv(ctx_);
  argv[0].set(json);

  JS::RootedValue rval(ctx_);
  if (!JS_CallFunctionName(ctx_, global_, method_name.c_str(), argv, &rval)) {
    raiseError();
  }

  enumerateTuples(&rval, tuples);
}

void JavaScriptContext::callReduceFunction(
    const String& method_name,
    const String& key,
    const Vector<String>& values,
    Vector<Pair<String, String>>* tuples) {
  JSAutoRequest js_req(ctx_);
  JSAutoCompartment js_comp(ctx_, global_);

  JS::AutoValueArray<3> argv(ctx_);
  auto method_str_ptr = JS_NewStringCopyN(
      ctx_,
      method_name.data(),
      method_name.size());
  if (!method_str_ptr) {
    RAISE(kRuntimeError, "reduce function execution error: out of memory");
  } else {
    argv[0].setString(method_str_ptr);
  }

  auto key_str_ptr = JS_NewStringCopyN(ctx_, key.data(), key.size());
  if (!key_str_ptr) {
    RAISE(kRuntimeError, "reduce function execution error: out of memory");
  } else {
    argv[1].setString(key_str_ptr);
  }

  ReduceCollectionIter val_iter;
  val_iter.data = &values;
  val_iter.cur = 0;

  auto val_iter_obj_ptr = JS_NewObject(ctx_, &ReduceCollectionIter::kJSClass);
  if (!val_iter_obj_ptr) {
    RAISE(kRuntimeError, "reduce function execution error: out of memory");
  }

  JS::RootedObject val_iter_obj(ctx_, val_iter_obj_ptr);
  JS_SetPrivate(val_iter_obj, &val_iter);
  argv[2].setObject(*val_iter_obj);

  JS_DefineFunction(
      ctx_,
      val_iter_obj,
      "hasNext",
      &ReduceCollectionIter::hasNext,
      0,
      0);

  JS_DefineFunction(
      ctx_,
      val_iter_obj,
      "next",
      &ReduceCollectionIter::getNext,
      0,
      0);

  JS::RootedValue rval(ctx_);
  if (!JS_CallFunctionName(ctx_, global_, "__call_with_iter", argv, &rval)) {
    raiseError();
  }

  enumerateTuples(&rval, tuples);
}

void JavaScriptContext::enumerateTuples(
    JS::RootedValue* src,
    Vector<Pair<String, String>>* dst) const {
  if (!src->isObject()) {
    RAISE(kRuntimeError, "reduce function must return a list/array of tuples");
  }

  JS::RootedObject list(ctx_, &src->toObject());
  JS::AutoIdArray list_enum(ctx_, JS_Enumerate(ctx_, list));
  for (size_t i = 0; i < list_enum.length(); ++i) {
    JS::RootedValue elem(ctx_);
    JS::RootedValue elem_key(ctx_);
    JS::RootedValue elem_value(ctx_);
    JS::Rooted<jsid> elem_id(ctx_, list_enum[i]);
    if (!JS_GetPropertyById(ctx_, list, elem_id, &elem)) {
      RAISE(kIllegalStateError);
    }

    if (!elem.isObject()) {
      RAISE(kRuntimeError, "reduce function must return a list/array of tuples");
    }

    JS::RootedObject elem_obj(ctx_, &elem.toObject());

    if (!JS_GetProperty(ctx_, elem_obj, "0", &elem_key)) {
      RAISE(kRuntimeError, "reduce function must return a list/array of tuples");
    }

    if (!JS_GetProperty(ctx_, elem_obj, "1", &elem_value)) {
      RAISE(kRuntimeError, "reduce function must return a list/array of tuples");
    }

    auto tkey_jstr = JS::ToString(ctx_, elem_key);
    if (!tkey_jstr) {
      RAISE(kRuntimeError, "first tuple element must be a string");
    }

    auto tkey_cstr = JS_EncodeString(ctx_, tkey_jstr);
    String tkey(tkey_cstr);
    JS_free(ctx_, tkey_cstr);

    String tval;
    JS::RootedObject replacer(ctx_);
    JS::RootedValue space(ctx_);
    if (!JS_Stringify(
            ctx_,
            &elem_value,
            replacer,
            space,
            &write_json_to_buf,
            &tval)) {
      RAISE(kRuntimeError, "second tuple element must be convertible to JSON");
    }

    dst->emplace_back(tkey, tval);
  }
}

Option<String> JavaScriptContext::getMapReduceJobJSON() {
  JSAutoRequest js_req(ctx_);
  JSAutoCompartment js_comp(ctx_, global_);

  JS::RootedValue job_def(ctx_);
  if (!JS_GetProperty(ctx_, global_, "__z1_mr_jobs", &job_def)) {
    return None<String>();
  }

  String json_str;
  JS::RootedObject replacer(ctx_);
  JS::RootedValue space(ctx_);
  if (!JS_Stringify(
          ctx_,
          &job_def,
          replacer,
          space,
          &write_json_to_buf,
          &json_str)) {
    RAISE(kRuntimeError, "illegal job definition");
  }

  return Some(json_str);
}

JSClass JavaScriptContext::ReduceCollectionIter::kJSClass = {
  "global",
  JSCLASS_HAS_PRIVATE
};

bool JavaScriptContext::ReduceCollectionIter::hasNext(
    JSContext* ctx,
    unsigned argc,
    JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  if (!args.thisv().isObject()) {
     return false;
  }

  auto thisv = args.thisv();
  if (JS_GetClass(&thisv.toObject()) != &kJSClass) {
    return false;
  }

  auto iter = static_cast<ReduceCollectionIter*>(
      JS_GetPrivate(&thisv.toObject()));
  if (!iter) {
    return false;
  }

  if (iter->cur < iter->data->size()) {
    args.rval().set(JSVAL_TRUE);
  } else {
    args.rval().set(JSVAL_FALSE);
  }

  return true;
}

bool JavaScriptContext::ReduceCollectionIter::getNext(
    JSContext* ctx,
    unsigned argc,
    JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  if (!args.thisv().isObject()) {
     return false;
  }

  auto thisv = args.thisv();
  if (JS_GetClass(&thisv.toObject()) != &kJSClass) {
    return false;
  }

  auto iter = static_cast<ReduceCollectionIter*>(
      JS_GetPrivate(&thisv.toObject()));
  if (!iter) {
    return false;
  }

  if (iter->cur >= iter->data->size()) {
    return false;
  }

  const auto& value = (*iter->data)[iter->cur];
  ++iter->cur;

  auto val_str_ptr = JS_NewStringCopyN(ctx, value.data(), value.size());
  if (val_str_ptr) {
    args.rval().setString(val_str_ptr);
    return true;
  } else {
    return false;
  }
}

} // namespace zbase

