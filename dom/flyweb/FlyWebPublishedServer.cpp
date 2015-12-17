/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/FlyWebPublishedServer.h"
#include "mozilla/dom/FlyWebPublishBinding.h"
#include "mozilla/dom/FlyWebClientConnectEvent.h"

namespace mozilla {
namespace dom {

NS_IMPL_ISUPPORTS_INHERITED(FlyWebPublishedServer,
                            mozilla::DOMEventTargetHelper,
                            nsIServerSocketListener)

FlyWebPublishedServer::FlyWebPublishedServer(nsPIDOMWindow* aOwner,
                                             const nsAString& aName,
                                             const FlyWebPublishOptions& aOptions)
  : mozilla::DOMEventTargetHelper(aOwner)
  , mName(aName)
  , mCategory(aOptions.mCategory)
  , mHttp(aOptions.mHttp)
  , mMessage(aOptions.mMessage)
  , mUiUrl(aOptions.mUiUrl)
{
  if (mCategory.IsEmpty()) {
    mCategory.SetIsVoid(true);
  }
}

JSObject*
FlyWebPublishedServer::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return FlyWebPublishedServerBinding::Wrap(aCx, this, aGivenProto);
}

void
FlyWebPublishedServer::SetServerSocket(nsIServerSocket* aServerSocket)
{
  mServerSocket = aServerSocket;

  DebugOnly<nsresult> rv = aServerSocket->GetPort(&mPort);
  MOZ_ASSERT(NS_SUCCEEDED(rv));
}

NS_IMETHODIMP
FlyWebPublishedServer::OnSocketAccepted(nsIServerSocket* aServ,
                                        nsISocketTransport* aTransport)
{
  MOZ_ASSERT(aServ == mServerSocket);

  nsresult rv;

  FlyWebClientConnectEventInit init;
  init.mConnection = 
    new FlyWebConnection(GetOwner(), aTransport, rv);
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<FlyWebClientConnectEvent> event =
    FlyWebClientConnectEvent::Constructor(this,
                                          NS_LITERAL_STRING("connect"),
                                          init);

  DispatchTrustedEvent(event);

  return NS_OK;
}

NS_IMETHODIMP
FlyWebPublishedServer::OnStopListening(nsIServerSocket* aServ,
                                       nsresult aStatus)
{
  MOZ_ASSERT(aServ == mServerSocket || !mServerSocket);

  DispatchTrustedEvent(NS_LITERAL_STRING("error"));

  mServerSocket = nullptr;

  return NS_OK;
}

} // namespace dom
} // namespace mozilla


