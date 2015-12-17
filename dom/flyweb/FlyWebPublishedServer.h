/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_FlyWebPublishedServer_h
#define mozilla_dom_FlyWebPublishedServer_h

#include "nsISupportsImpl.h"
#include "mozilla/DOMEventTargetHelper.h"
#include "mozilla/ErrorResult.h"
#include "nsIServerSocket.h"

class nsPIDOMWindow;

namespace mozilla {
namespace dom {

class Promise;
struct FlyWebPublishOptions;

class FlyWebPublishedServer final : public mozilla::DOMEventTargetHelper
                                  , public nsIServerSocketListener
{
public:
  FlyWebPublishedServer(nsPIDOMWindow* aOwner,
                        const nsAString& aName,
                        const FlyWebPublishOptions& aOptions);

  virtual JSObject* WrapObject(JSContext *cx, JS::Handle<JSObject*> aGivenProto) override;

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSISERVERSOCKETLISTENER

  void SetServerSocket(nsIServerSocket* aServerSocket);

  int32_t Port()
  {
    return mPort;
  }

  void GetName(nsAString& aName)
  {
    aName = mName;
  }

  void GetCategory(nsAString& aCategory)
  {
    aCategory = mCategory;
  }

  bool Http()
  {
    return mHttp;
  }

  bool Message()
  {
    return mMessage;
  }

  void GetUiUrl(nsAString& aUiUrl)
  {
    aUiUrl = mUiUrl;
  }

  IMPL_EVENT_HANDLER(connect)
  IMPL_EVENT_HANDLER(error)

private:
  ~FlyWebPublishedServer() {}

  nsCOMPtr<nsIServerSocket> mServerSocket;

  int32_t mPort;

  nsString mName;
  nsString mCategory;
  bool mHttp;
  bool mMessage;
  nsString mUiUrl;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_FlyWebPublishedServer_h
