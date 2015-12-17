/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_FlyWebService_h
#define mozilla_dom_FlyWebService_h

#include "nsISupportsImpl.h"
#include "mozilla/ErrorResult.h"
#include "nsIProtocolHandler.h"
#include "nsDataHashtable.h"

class nsPIDOMWindow;

namespace mozilla {
namespace dom {

class Promise;
struct FlyWebPublishOptions;
struct FlyWebFilter;
class FlyWebConnection;

class FlyWebService final : public nsIProtocolHandler
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIPROTOCOLHANDLER

  static FlyWebService* GetOrCreate();
  static already_AddRefed<FlyWebService> GetOrCreateAddRefed()
  {
    return do_AddRef(GetOrCreate());
  }

  already_AddRefed<Promise> PublishServer(const nsAString& aName,
                                          const FlyWebPublishOptions& aOptions,
                                          nsPIDOMWindow* aWindow,
                                          ErrorResult& aRv);

  already_AddRefed<Promise> ConnectToServer(const FlyWebFilter& aFilter,
                                            nsPIDOMWindow* aWindow,
                                            ErrorResult& aRv);

  nsresult RegisterConnection(FlyWebConnection* connection, nsAString& aRoot);
  void UnregisterConnection(FlyWebConnection* connection);
  FlyWebConnection* GetConnection(nsIURI* aURI);

private:
  ~FlyWebService() {};

  nsDataHashtable<nsCStringHashKey, FlyWebConnection*> mConnections;

};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_FlyWebService_h