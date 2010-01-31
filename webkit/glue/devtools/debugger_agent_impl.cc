/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
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
 */

#include "config.h"

#include <wtf/HashSet.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>

#include "Document.h"
#include "Frame.h"
#include "Page.h"
#include "V8Binding.h"
#include "V8DOMWindow.h"
#include "V8Index.h"
#include "V8Proxy.h"
#undef LOG

#include "grit/webkit_resources.h"
#include "third_party/WebKit/WebKit/chromium/src/WebViewImpl.h"
#include "webkit/glue/devtools/debugger_agent_impl.h"
#include "webkit/glue/devtools/debugger_agent_manager.h"
#include "webkit/glue/webdevtoolsagent_impl.h"
#include "webkit/glue/webkit_glue.h"

using WebCore::DOMWindow;
using WebCore::Document;
using WebCore::Frame;
using WebCore::Page;
using WebCore::String;
using WebCore::V8ClassIndex;
using WebCore::V8Custom;
using WebCore::V8DOMWindow;
using WebCore::V8DOMWrapper;
using WebCore::V8Proxy;
using WebKit::WebViewImpl;

namespace WebKit {

DebuggerAgentImpl::DebuggerAgentImpl(
    WebViewImpl* webViewImpl,
    DebuggerAgentDelegate* delegate,
    WebDevToolsAgentImpl* webdevtoolsAgent)
    : m_webViewImpl(webViewImpl)
    , m_delegate(delegate)
    , m_webdevtoolsAgent(webdevtoolsAgent)
    , m_autoContinueOnException(false)
{
    DebuggerAgentManager::debugAttach(this);
}

DebuggerAgentImpl::~DebuggerAgentImpl()
{
    DebuggerAgentManager::debugDetach(this);
}

void DebuggerAgentImpl::getContextId()
{
    m_delegate->setContextId(m_webdevtoolsAgent->hostId());
}

void DebuggerAgentImpl::debuggerOutput(const String& command)
{
    m_delegate->debuggerOutput(command);
    m_webdevtoolsAgent->forceRepaint();
}

// static
void DebuggerAgentImpl::createUtilityContext(Frame* frame, v8::Persistent<v8::Context>* context)
{
    v8::HandleScope scope;

    // Set up the DOM window as the prototype of the new global object.
    v8::Handle<v8::Context> windowContext = V8Proxy::context(frame);
    v8::Handle<v8::Object> windowGlobal = windowContext->Global();
    v8::Handle<v8::Object> windowWrapper = V8DOMWrapper::lookupDOMWrapper(V8ClassIndex::DOMWINDOW, windowGlobal);

    ASSERT(V8DOMWindow::toNative(windowWrapper) == frame->domWindow());

    v8::Handle<v8::ObjectTemplate> globalTemplate = v8::ObjectTemplate::New();

    // TODO(yurys): provide a function in v8 bindings that would make the
    // utility context more like main world context of the inspected frame,
    // otherwise we need to manually make it satisfy various invariants
    // that V8Proxy::getEntered and some other V8Proxy methods expect to find
    // on v8 contexts on the contexts stack.
    // See V8Proxy::createNewContext.
    //
    // Install a security handler with V8.
    globalTemplate->SetAccessCheckCallbacks(
        V8DOMWindow::namedSecurityCheck,
        V8DOMWindow::indexedSecurityCheck,
        v8::Integer::New(V8ClassIndex::DOMWINDOW));
    // We set number of internal fields to match that in V8DOMWindow wrapper.
    // See http://crbug.com/28961
    globalTemplate->SetInternalFieldCount(V8DOMWindow::internalFieldCount);

    *context = v8::Context::New(0 /* no extensions */, globalTemplate, v8::Handle<v8::Object>());
    v8::Context::Scope contextScope(*context);
    v8::Handle<v8::Object> global = (*context)->Global();

    v8::Handle<v8::String> implicitProtoString = v8::String::New("__proto__");
    global->Set(implicitProtoString, windowWrapper);

    // Give the code running in the new context a way to get access to the
    // original context.
    global->Set(v8::String::New("contentWindow"), windowGlobal);

    // Inject javascript into the context.
    base::StringPiece injectjsWebkit = webkit_glue::GetDataResource(IDR_DEVTOOLS_INJECT_WEBKIT_JS);
    v8::Script::Compile(v8::String::New(injectjsWebkit.as_string().c_str()))->Run();
    base::StringPiece injectDispatchjs = webkit_glue::GetDataResource(IDR_DEVTOOLS_INJECT_DISPATCH_JS);
    v8::Script::Compile(v8::String::New(injectDispatchjs.as_string().c_str()))->Run();
}

String DebuggerAgentImpl::executeUtilityFunction(
    v8::Handle<v8::Context> context,
    int callId,
    const char* object,
    const String &functionName,
    const String& jsonArgs,
    bool async,
    String* exception)
{
    v8::HandleScope scope;
    ASSERT(!context.IsEmpty());
    if (context.IsEmpty()) {
        *exception = "No window context.";
        return "";
    }
    v8::Context::Scope contextScope(context);

    DebuggerAgentManager::UtilityContextScope utilityScope;

    v8::Handle<v8::Object> dispatchObject = v8::Handle<v8::Object>::Cast(
        context->Global()->Get(v8::String::New(object)));

    v8::Handle<v8::Value> dispatchFunction = dispatchObject->Get(v8::String::New("dispatch"));
    ASSERT(dispatchFunction->IsFunction());
    v8::Handle<v8::Function> function = v8::Handle<v8::Function>::Cast(dispatchFunction);

    v8::Handle<v8::String> functionNameWrapper = v8::Handle<v8::String>(
        v8::String::New(functionName.utf8().data()));
    v8::Handle<v8::String> jsonArgsWrapper = v8::Handle<v8::String>(
        v8::String::New(jsonArgs.utf8().data()));
    v8::Handle<v8::Number> callIdWrapper = v8::Handle<v8::Number>(
        v8::Number::New(async ? callId : 0));

    v8::Handle<v8::Value> args[] = {
      functionNameWrapper,
      jsonArgsWrapper,
      callIdWrapper
    };

    v8::TryCatch tryCatch;
    v8::Handle<v8::Value> resObj = function->Call(context->Global(), 3, args);
    if (tryCatch.HasCaught()) {
        v8::Local<v8::Message> message = tryCatch.Message();
        if (message.IsEmpty())
            *exception = "Unknown exception";
        else
            *exception = WebCore::toWebCoreString(message->Get());
        return "";
    } else
        return WebCore::toWebCoreStringWithNullCheck(resObj);
}

void DebuggerAgentImpl::executeVoidJavaScript(v8::Handle<v8::Context> context)
{
    v8::HandleScope scope;
    ASSERT(!context.IsEmpty());
    v8::Context::Scope contextScope(context);
    DebuggerAgentManager::UtilityContextScope utilityScope;

    v8::Handle<v8::Value> function =
        context->Global()->Get(v8::String::New("devtools$$void"));
    ASSERT(function->IsFunction());
    v8::Handle<v8::Value> args[] = {
      v8::Local<v8::Value>()
    };
    v8::Handle<v8::Function>::Cast(function)->Call(context->Global(), 0, args);
}

WebCore::Page* DebuggerAgentImpl::page()
{
  return m_webViewImpl->page();
}

} // namespace WebKit
