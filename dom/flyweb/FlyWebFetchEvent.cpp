/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/EventBinding.h"
#include "mozilla/dom/FlyWebFetchEvent.h"
#include "mozilla/dom/FlyWebFetchEventBinding.h"
#include "js/GCAPI.h"
#include "mozilla/dom/FlyWebFetchEvent.h"
#include "mozilla/dom/Nullable.h"
#include "mozilla/dom/FlyWebConnection.h"
//#include "mozilla/dom/PrimitiveConversions.h"

namespace mozilla {
namespace dom {


NS_IMPL_CYCLE_COLLECTION_CLASS(FlyWebFetchEvent)

NS_IMPL_ADDREF_INHERITED(FlyWebFetchEvent, Event)
NS_IMPL_RELEASE_INHERITED(FlyWebFetchEvent, Event)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(FlyWebFetchEvent, Event)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mRequest)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(FlyWebFetchEvent, Event)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(FlyWebFetchEvent, Event)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mRequest)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(FlyWebFetchEvent)
NS_INTERFACE_MAP_END_INHERITING(Event)

FlyWebFetchEvent::FlyWebFetchEvent(FlyWebConnection* aConnection,
                                   class Request* aRequest,
                                   const nsACString& aPath)
  : Event(aConnection, nullptr, nullptr)
  , mRequest(aRequest)
  , mConnection(aConnection)
  , mPath(aPath)
  , mResponded(false)
{
}

FlyWebFetchEvent::~FlyWebFetchEvent()
{
}

JSObject*
FlyWebFetchEvent::WrapObjectInternal(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return FlyWebFetchEventBinding::Wrap(aCx, this, aGivenProto);
}

void
FlyWebFetchEvent::RespondWith(Promise& aArg, ErrorResult& aRv)
{
  if (mResponded){
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }

  mResponded = true;

  aArg.AppendNativeHandler(this);
}

void
FlyWebFetchEvent::ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue)
{
  if (!aValue.isObject()) {
    mConnection->OnFetchResult(NS_ERROR_UNEXPECTED, mPath);
  }

  RefPtr<Response> response;
  nsresult rv = UNWRAP_OBJECT(Response, &aValue.toObject(), response);
  if (NS_FAILED(rv)) {
    mConnection->OnFetchResult(rv, mPath);
  }

  if (response->Type() == ResponseType::Opaque) {
    mConnection->OnFetchResult(NS_ERROR_DOM_SECURITY_ERR, mPath);
  }

  mConnection->OnFetchResult(NS_OK, mPath, response);
}

void
FlyWebFetchEvent::RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue)
{
  mConnection->OnFetchResult(NS_ERROR_FAILURE, mPath);
}


} // namespace dom
} // namespace mozilla
